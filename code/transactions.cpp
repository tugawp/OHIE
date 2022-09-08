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

extern int64_t AT;
extern int64_t D;
extern uint32_t AGING_MONITOR_EACH_MILLISECONDS;
extern uint32_t TRANSACTION_THROUGHPUT_EACH_NODE;
extern bool BIZANTINE;
extern tcp_server* ser;
extern string my_ip;
extern uint32_t my_port;
extern uint32_t CLIENTS_PER_NODE;
string my_address;

map<string, aging_info> aging_transactions; // key: "sender_addr:seq", value: aging_info { tx, time }
boost::mutex aging_transactions_mtx;

map<string, aged_info> aged_transactions; // key: "sender_addr:seq", value: aged_info { tx, age }
boost::mutex aged_transactions_mtx;

map<string, uint64_t> next_seqs; // of promised transactions (ignore for now because some transactions are commited and we can't update seq)
boost::mutex next_seqs_mtx;

deque<string> mempool;
boost::mutex mempool_mtx;

map<string, BlockHash> transaction_block; // saves the block of each transaction (string is from:seq)
boost::mutex transaction_block_mtx;

int64_t get_average_promise_time()
{
	int64_t total_promise_time = 0;
	int64_t count = 0;

	aged_transactions_mtx.lock();
	for (auto it = aged_transactions.begin(); it != aged_transactions.end(); it++)
	{
		int64_t age = it->second.end_time - it->second.start_time;
		if (age > AT)
		{
			total_promise_time += age;
			count++;
		}
	}
	aged_transactions_mtx.unlock();

	if (count > 0)
		return total_promise_time / count; //milliseconds
	else
		return 0;
}

int64_t get_now()
{
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint32_t RSS_simple(aged_info agd_info)
{
	int64_t age = agd_info.end_time - agd_info.start_time;
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
	int64_t age = agd_info.end_time - agd_info.start_time;
	return age >> 1;
}

uint32_t RSS(aged_info agd_info) { return RSS_simple(agd_info); }

string get_random_address(uint32_t size_in_dwords)
{
	stringstream sstream;
	for (int i = 0; i < size_in_dwords; i++)
		sstream << setfill('0') << setw(8) << hex << rng(); 
		// makes sense to generate address of client that exists

	return sstream.str();
}


string get_my_address(uint32_t size_in_dwords) 
{
	int client_id = rng() % CLIENTS_PER_NODE;
	if (my_address.empty()) {
		my_address = my_ip + to_string(my_port) + to_string(client_id);
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
		//printf("Oops, mempool empty\n");
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

	//NOT SAVING TRANSACTIONS BECAUSE VERIFICATIONS FAIL OOPSSSSS

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

		if (from.size() != 8 * ADDRESS_SIZE_IN_DWORDS || to.size() != 8 * ADDRESS_SIZE_IN_DWORDS || amount.size() <= 0 || (it_seq != next_seqs.end() && it_seq->second < seq_N && false))
		{ // todo: problema com next seqs
			return; //ignore
        }  
		
		string tx = from + ":" + seq + ":" + to + ":" + amount;


		if (false && !verify_message(tx, sign)) //maybe ignore signatures
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
															ai.time, get_now() };
					aging_transactions.erase(it_aging);
				}
			}
			//todo: add to mempool?
		}
		aged_transactions_mtx.unlock(); //printf("Unlocked aged_transactions_mtx\n");
		aging_transactions_mtx.unlock(); //printf("Unlocked aging_transactions_mtx\n");
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
		if (from.size() != 8 * ADDRESS_SIZE_IN_DWORDS || to.size() != 8 * ADDRESS_SIZE_IN_DWORDS || amount.size() <= 0 || (it_seq != next_seqs.end() && it_seq->second < seq_N))
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

void update_transactions_block(list<string> txs, BlockHash block_hash) {
	//update the block of transactions received from a new block that is accepted
	transaction_block_mtx.lock();
	for (string t : txs) {
		vector<string> s = split(t, ":");
		string from = s[0];
		string seq = s[1];
		transaction_block[from + ":" + seq] = block_hash;
	}
	transaction_block_mtx.unlock();
}

int get_aging_count()
{
	aging_transactions_mtx.lock(); 
	int aging_total = aging_transactions.size();
	aging_transactions_mtx.unlock();
	return aging_total;
}

int get_promised_count()
{
	aged_transactions_mtx.lock();
	int promised_count = 0;
	for (auto it = aged_transactions.begin(); it != aged_transactions.end(); it++)
	{
		if ((it->second.end_time - it->second.start_time) > AT)
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
		int64_t start = get_now();
		aging_transactions_mtx.lock();
		next_seqs_mtx.lock();	
		aged_transactions_mtx.lock();
		int count = 0;

		for (auto it = aging_transactions.begin(); it != aging_transactions.end();)
		{	
			int64_t now = get_now();
			int64_t age = now - it->second.time;
			//std::cout << "now: " << now << ", start: " << it->second.time << ", age: " << age << std::endl << flush;
			if (age > AT)
			{
				vector<string> s = split(it->first, ":");
				string from = s[0];
				string seq = s[1];
				uint64_t *next_seq = &next_seqs[from]; // como next seqs é atualizado, podemos limpar transação do aged transactions
				uint64_t seqN = stoull(seq);
				if (seqN > *next_seq) {
					*next_seq++;
				}
				
				aged_transactions[it->first] = { it->second.full_tx, it->second.time, get_now() };
				it = aging_transactions.erase(it);
				count++;
			} else
			{
				++it;
			}
		}
		aged_transactions_mtx.unlock();
		next_seqs_mtx.unlock();	
		aging_transactions_mtx.unlock();

		int64_t end = get_now(); //overkill?
		boost::this_thread::sleep(boost::posix_time::milliseconds(AGING_MONITOR_EACH_MILLISECONDS));
	}
}

void transaction_creator()
{
	uint64_t seqN = 0; // no persistance but okay, also maybe use uint64_t or smth

	while (1)
	{
		int64_t start = get_now();

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
		//printf("Sending transactions\n");
		ser->write_to_all_peers(s);

		int64_t end = get_now();
		boost::this_thread::sleep(boost::posix_time::milliseconds(1000)); //remaining time to complete second
	}
}

void commit_block(BlockHash block_hash) {
	// ver block_transactions para saber as transações do block
	// iterar transações do bloco e atualizar a age caso n esteja promised 
	// atualizar next seqs?
	list<string> txs;
	transaction_block_mtx.lock();
	aged_transactions_mtx.lock();
	next_seqs_mtx.lock();
	for (auto it = transaction_block.begin(); it != transaction_block.end(); it++) {
		if (it->second == block_hash) {
			string tx = it->first; // from:seq
			aged_info *ai = &aged_transactions[tx];
			if ((ai->end_time - ai->start_time) < AT) {
				// didn't age, AKA not promised yet 
				ai->end_time = get_now(); //todo - we need to keep the start instant
				vector<string> s = split(tx, ":");
				string from = s[0];
				string seq = s[1];
				uint64_t *next_seq = &next_seqs[from];
				uint64_t seqN = stoull(seq);
				if (seqN > *next_seq) {
					*next_seq++; //probably wont happen
				}
			}
		}
	}
	next_seqs_mtx.unlock();		
	aged_transactions_mtx.unlock();
	transaction_block_mtx.unlock();
}

