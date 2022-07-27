/*
Copyright (c) 2018, Ivica Nikolic <cube444@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "transactions.h"
#include "crypto_stuff.h"
#include "misc.h"
#include "requests.h"

using namespace std;

extern uint64_t AT;
extern uint64_t D;
extern uint32_t AGING_MONITOR_EACH_MILLISECONDS;
extern uint32_t TRANSACTION_THROUGHPUT_EACH_NODE;
extern bool BIZANTINE;
extern tcp_server* ser;
extern string my_ip;
extern uint32_t my_port;
string my_address;

map<string, aging_info> aging_transactions; // key: "sender_addr:seq", value: aging_info { tx, time }
boost::mutex aging_transactions_mtx;

map<string, aged_info> aged_transactions; // key: "sender_addr:seq", value: aged_info { tx, age }
boost::mutex aged_transactions_mtx;

map<string, uint64_t> next_seqs; // of promised transactions (ignore for now because some transactions are commited and we can't update seq)
boost::mutex next_seqs_mtx;

deque<string> mempool;
boost::mutex mempool_mtx;

uint64_t get_average_promise_time()
{
	uint64_t total_promise_time = 0;
	uint64_t count = 0;

	aged_transactions_mtx.lock();
	for (auto it = aged_transactions.begin(); it != aged_transactions.end(); it++)
	{
		if (it->second.age > AT)
		{
			total_promise_time += it->second.age;
			count++;
		}
	}
	aged_transactions_mtx.unlock();

	if (count > 0)
		return total_promise_time / count; //milliseconds
	else
		return 0;
}

uint64_t get_now()
{
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint32_t RSS_simple(aged_info agd_info)
{
	uint64_t age = agd_info.age;
	uint32_t C = T_DISCARD[NO_T_DISCARDS - 1]; //what is this NO_T_DISCARD (expands to 1)?
	if (age >= AT - 2 * D)
	{
		return C;
	}

	return 0;
}

// assuming AT = 2(C + 1), with C being T_DISCARD[NO_T_DISCARDS - 1] (ig)
uint32_t RSS_prog(aged_info agd_info)
{
	return agd_info.age >> 1;
}

uint32_t RSS(aged_info agd_info) { return RSS_simple(agd_info); }

string get_random_address(uint32_t size_in_dwords)
{
	stringstream sstream;
	for (int i = 0; i < size_in_dwords; i++)
		sstream << setfill('0') << setw(8) << hex << rng();

	return sstream.str();
}


string get_my_address(uint32_t size_in_dwords) 
{
	if (my_address.empty()) {
		my_address = my_ip + to_string(my_port);
		for (int i = 0; (size_in_dwords * 8 - my_address.size()); i++)
			my_address += "0";
	}
	return my_address;
}


string get_next_seq(string address)
{
	auto it = next_seqs.find(address);
	if (it != next_seqs.end())
	{
		uint64_t seq = it->second;
		next_seqs[address] = seq + 1;
		return to_string(seq);
	}
	next_seqs[address] = 1;
	return to_string(0);
}

string create_one_transaction(uint32_t rank) //não sabemos rank, como saber?
{
	// transactions are: "address1:sequence:address2:amount:signature"
	// me: como identificar as transações? 3º param?
	// me: fake_transactions está ativo por defeito. Desativar ou rever
	// me: identificar transações com add1 e seq. Double spend tem mesmo add1 e seq e resto diferente.
	/*
	if( fake_transactions ){
		return "0000000000000000000000000000000000000000:00000000:0000000000000000000000000000000000000000:0000000000:00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
	}

	string from = get_random_address(ADDRESS_SIZE_IN_DWORDS); // 40 caracteres hexa
	string seq = get_next_seq(from);
	string to = get_random_address(ADDRESS_SIZE_IN_DWORDS);
	string amount = to_string(rng());

	string tx = from + ":" + seq + ":" + to + ":" + amount;
	string sign = sign_message(tx);

	string full_tx = tx + ":" + sign;

	aging_transactions[from + ":" + seq] = { full_tx, get_now(), rank };

	return full_tx;
	*/

	// IMPORTANT!!!
	// Instead of creating random transactions, from now on get transaction
	// from mempool
	while (1)
	{	
		string tx;
		mempool_mtx.lock();
		if (!mempool.empty())
		{
			tx = mempool.front();
			mempool.pop_front();
		}
		mempool_mtx.unlock();
		if (!tx.empty()) {
			//printf("Returning transaction %s\n", tx.c_str());
			return tx;
		}
		printf("Oops, mempool empty\n");
		boost::this_thread::sleep(boost::posix_time::milliseconds(100)); //wait not to overload
	}
}

int create_transaction_block(BlockHash hash, string filename, uint32_t rank)
{
	uint32_t l = 0, no_txs = 0;

	if (WRITE_BLOCKS_TO_HDD)
	{
		ofstream file;
		try
		{
			file.open(filename);
		} catch (const std::string& ex)
		{
			return false;
		}

		while (l < BLOCK_SIZE_IN_BYTES)
		{
			string tx = create_one_transaction(rank);
			file << tx << endl;
			l += tx.size();
			no_txs++;
		}
		file.close();
	} else
	{
		while (l < BLOCK_SIZE_IN_BYTES)
		{
			string tx = create_one_transaction(rank);
			l += tx.size();
			no_txs++;
		}
	}
	return no_txs;
}

void verify_transaction(string full_tx)
{
	//receive transaction before inserting in mempool
	vector<string> s = split(full_tx, ":");
	if (s.size() == 5)
	{
		string from = s[0];
		string seq = s[1];
		string to = s[2];
		string amount = s[3];
		string sign = s[4];

		uint64_t seq_N = stoull(seq);
		auto it_seq = next_seqs.find(from);
		if (from.size() != 8 * ADDRESS_SIZE_IN_DWORDS || to.size() != 8 * ADDRESS_SIZE_IN_DWORDS || amount.size() <= 0 || (it_seq != next_seqs.end() && it_seq->second < seq_N))
		{
			return; //ignore
        }  
		string tx = from + ":" + seq + ":" + to + ":" + amount;

		if (false || !verify_message(tx, sign)) //maybe ignore signatures
			return; //ignore

		string tx_key = from + ":" + seq;

		aging_transactions_mtx.lock(); //printf("Locked aging_transactions_mtx\n");
		aged_transactions_mtx.lock(); //printf("Locked aged_transactions_mtx\n");
		auto it_aging = aging_transactions.find(tx_key);
		auto it_aged = aged_transactions.find(tx_key);

		if (it_aging == aging_transactions.end() &&
			it_aged == aged_transactions.end())
		{
			mempool_mtx.lock(); //printf("Locked mempool_mtx\n");
			mempool.push_back(full_tx); // add to mempool 
			mempool_mtx.unlock(); //printf("Unlocked mempool_mtx\n"); 
			aging_transactions[from + ":" + seq] = { full_tx, get_now() }; // add to aging transactions
		} else
		{
			string previous_full_tx = it_aging != aging_transactions.end() ? it_aging->second.full_tx : it_aged->second.full_tx;
			if (!previous_full_tx.compare(full_tx)) // if not same transaction
			{ 
				// don't add to mempool
				if (it_aging != aging_transactions.end()) // if previous is aging
				{ 
					aging_info ai = it_aging->second;
					aged_transactions[from + ":" + seq] = { ai.full_tx,
														   get_now() - ai.time };
					aging_transactions.erase(it_aging);
				}
			}
			//todo: add to mempool?
		}
		aging_transactions_mtx.unlock(); //printf("Unlocked aging_transactions_mtx\n");
		aged_transactions_mtx.unlock(); //printf("Unlocked aged_transactions_mtx\n");
	} else
	{
		if (PRINT_TRANSMISSION_ERRORS)
		{
			cout << "Incorrect transaction size:" << s.size() << endl;
			cout << "tx:" << full_tx << endl;
		}
	}
}

bool verify_transaction_from_block(string full_tx, uint32_t rank, uint32_t last_rank)
// rank é o rank do bloco da transação, e last rank o último rank do último bloco dessa cadeia
{
	// me: guardar transação com instante de chegada (se ainda não existir)
	// me: rejeitar se transação já existir ou n tiver sufixo suficiente
	//     (se calhar passar informação sobre nº do bloco desta transação)
	vector<string> s = split(full_tx, ":");
	if (s.size() == 5)
	{
		string from = s[0];
		string seq = s[1];
		string to = s[2];
		string amount = s[3];
		string sign = s[4];

		uint32_t suffix_size = last_rank - rank;

		uint64_t seq_N = stoull(seq);
		auto it_seq = next_seqs.find(from);
		if (from.size() != 8 * ADDRESS_SIZE_IN_DWORDS || to.size() != 8 * ADDRESS_SIZE_IN_DWORDS || amount.size() <= 0/* || (it_seq != next_seqs.end() && it_seq->second < seq_N)*/)
			return false;

		string tx = from + ":" + seq + ":" + to + ":" + amount;

		if (false || !verify_message(tx, sign))
			return false;

		string tx_key = from + ":" + seq;

		
		auto it_aged = aged_transactions.find(tx_key);
		if (it_aged != aged_transactions.end() && !it_aged->second.full_tx.compare(full_tx)
			&& RSS(it_aged->second) > suffix_size)
			return false;

		//next_seqs[from] = ++seq_N; // se calhar devia fazer isto só quando é committed (ou prometida)?
		return true;
	} else
	{
		if (PRINT_TRANSMISSION_ERRORS)
		{
			cout << "Incorrect transaction size:" << s.size() << endl;
			cout << "tx:" << full_tx << endl;
		}
		return false;
	}
}

uint64_t get_aging_count()
{
	aging_transactions_mtx.lock(); 
	int aging_total = aging_transactions.size();
	aging_transactions_mtx.unlock();
	return aging_total;
}

uint64_t get_promised_count()
{
	aged_transactions_mtx.lock();
	int promised_count = 0;
	for (auto it = aged_transactions.begin(); it != aged_transactions.end(); it++)
	{
		if (it->second.age > AT)
			promised_count++;
	}
	aged_transactions_mtx.unlock();
	return promised_count;
}

void add_transactions(vector<string> transactions)
{
	//printf("Hmm, add_transaction(txs) received %ld transactions, ", transactions.size());
	int i = 0;

	for (string transaction : transactions)
	{	
		verify_transaction(transaction);
		i++;
	}
	//printf("but actually added %ld transactions\n", i);
}

bool bool_with_prob()
{
	return false;
}

// Tasks
void aging_monitor()
{
	while (1)
	{
		auto start = get_now();

		aging_transactions_mtx.lock();
		for (auto it = aging_transactions.begin(); it != aging_transactions.end();)
		{
			auto age = get_now() - it->second.time;
			if (age > AT)
			{
				aged_transactions[it->first] = { it->second.full_tx, age, it->second.rank };
				it = aging_transactions.erase(it);
			} else
			{
				++it;
			}
		}
		aging_transactions_mtx.unlock();

		auto end = get_now(); //overkill?
		boost::this_thread::sleep(boost::posix_time::milliseconds(AGING_MONITOR_EACH_MILLISECONDS - (end - start)));
	}
}

void transaction_creator()
{
	uint64_t seqN = 0; // no persistance but okay, also maybe use uint64_t or smth

	while (1)
	{
		auto start = get_now();

		vector<string> transactions;
		for (int i = 0; i < TRANSACTION_THROUGHPUT_EACH_NODE; i++) //careful with transaction size, are there restrictinons?
		{ // tx/s per node
			string from = get_my_address(ADDRESS_SIZE_IN_DWORDS);
			string seq = to_string(seqN);
			string to = get_random_address(ADDRESS_SIZE_IN_DWORDS);
			string amount = to_string(rng());

			string tx = from + ":" + seq + ":" + to + ":" + amount;
			string sign = "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"; //sign_message(tx); //maybe ignore signing

			string full_tx = tx + ":" + sign;
			transactions.push_back(full_tx); //change create one transaction to use node id

			mempool_mtx.lock();
			mempool.push_back(full_tx);
			mempool_mtx.unlock();

			if (BIZANTINE && bool_with_prob())
			{ // todo: if (bizantine && ...)
				//repeat seq
				continue;
			}
			seqN += 1; // seq++; erro?

			//todo: add created transactions to aging_transactions
		}

		string s = create__transactions(transactions);
		printf("Sending transactions\n");
		ser->write_to_all_peers(s);

		auto end = get_now();
		boost::this_thread::sleep(boost::posix_time::milliseconds(1000 - (end - start))); //remaining time to complete second
	}
}
