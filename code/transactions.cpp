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

bool PRINT_LOCKS = false;
bool SAVE_TRANSACTIONS = false;

extern int64_t AT;
extern int64_t D;
extern uint32_t AGING_MONITOR_EACH_MILLISECONDS;
extern uint32_t TRANSACTION_THROUGHPUT_EACH_NODE;
extern uint32_t IDLE_START_TIME;
extern bool BIZANTINE;
extern tcp_server* ser;
extern string my_ip_or_hostname;
extern uint32_t my_port;
extern uint32_t CLIENTS_PER_NODE;
string my_address;
const string EMPTY_DEPENDENCY = std::string(ADDRESS_SIZE_IN_DWORDS * 8, '0') + ":0";

map<string, aging_info> aging_transactions; // key: "sender_addr:seq", value: aging_info { tx, time }
boost::mutex aging_transactions_mtx;

map<string, aged_info> aged_transactions; // key: "sender_addr:seq", value: aged_info { tx, age }
boost::mutex aged_transactions_mtx;       // remove transactions from aged on commit event 

map<string, uint64_t> next_seqs; // of promised transactions (ignore for now because some transactions are commited and we can't update seq)
boost::mutex next_seqs_mtx;

map<string, string> pending_transactions; // transactions that don't all dependencies met
boost::mutex pending_transactions_mtx;    // dependencies are the explicit ones and previous sequence no

deque<string> mempool;
boost::mutex mempool_mtx;

map<string, vector<string>> dependencies; // key: dependency, value: dependent (when key finishes move value from pending to mempool)
boost::mutex dependencies_mtx;


boost::mutex promise_stats_mtx;
double avg_promise_time = 0;
uint64_t promise_count = 0;

string last_promised = EMPTY_DEPENDENCY;


void update_promise_stats(int64_t age) {
	if (PRINT_LOCKS) { cout << "Locking update_promise_stats in update_promise_stats" << endl; } promise_stats_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked update_promise_stats in update_promise_stats" << endl << flush; }
	promise_count++;
	
	double pc = static_cast<double>(promise_count);

	avg_promise_time = ((pc - 1) / pc) *  avg_promise_time + (1 / pc) * age;
	promise_stats_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked update_promise_stats in update_promise_stats" << endl << flush; }
}


int64_t get_average_promise_time()
{
	int64_t ret;
	if (PRINT_LOCKS) { cout << "Locking promise_stats_mtx in get_average_promise_time" << endl; } promise_stats_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked promise_stats_mtx in get_average_promise_time" << endl << flush; }
	ret = avg_promise_time;
	promise_stats_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked promise_stats_mtx in get_average_promise_time" << endl << flush; }
	return static_cast<int64_t>(ret);
	
}

int get_promised_count()
{
	int64_t ret;
	if (PRINT_LOCKS) { cout << "Locking promise_stats_mtx in get_promised_count" << endl; } promise_stats_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked promise_stats_mtx in get_promised_count" << endl << flush; }
	ret = promise_count;
	promise_stats_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked promise_stats_mtx in get_promised_count" << endl << flush; }
	return ret;
}

int get_aged_count()
{
	return aged_transactions.size();
}

int get_pending_count()
{
	return pending_transactions.size();
}

int get_mempool_count()
{
	return mempool.size();
}

int64_t get_now()
{
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint32_t RSS_simple(aged_info agd_info)
{
	int64_t age = agd_info.end_time - agd_info.start_time;
	uint32_t C = T_DISCARD[0]; 
	if (age >= AT - 2 * D)
	{
		return C;
	}

	return 0;
}

// assuming AT = 2(C + 1), with C being T_DISCARD[0] (ig)
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

string get_random_from_address(uint32_t size_in_dwords) //might be here
{
	int client_id = rng() % CLIENTS_PER_NODE; 
	string address = my_ip_or_hostname + to_string(my_port) + to_string(client_id);
	for (int i = 0; (size_in_dwords * 8 - address.size()); i++)
		address += "0";
	return address;
}

uint64_t get_next_seq(string address)
{
	return next_seqs[address]; //returns 0 if key not present
}

void update_next_seq(string address)
{
	next_seqs[address] += 1;
}

bool is_promised(string tx) { //promised or commited
	vector<string> s = split(tx, ":");
	string from = s[0];
	string seq = s[1];
	return get_next_seq(from) > stoull(seq); // all seqs before get_next_seq(from) are promised/committed
}

string get_dependency(string to) {
	return last_promised;
}


void save_dependency(string from, uint64_t seq_N, string dependency) {
	string tx_key = from + ":" + to_string(seq_N);
	vector<string> deps = dependencies[dependency];
	if (std::find(deps.begin(), deps.end(), tx_key) ==  deps.end()) {
		dependencies[dependency].push_back(tx_key); 
	}
} 

void clear_dependencies(string dependency) {
	dependencies[dependency].clear();
}

bool check_dependencies(string from, uint64_t seq_N, string dependency)
{	
	if (dependency != EMPTY_DEPENDENCY && !is_promised(dependency)) {
		save_dependency(from, seq_N, dependency);
		// std::cout << "Because of explicit dependency: "; 
		return false;
	}
	// std::cout << "Explicit dependency: " << dependency << std::endl << flush; 
	return next_seqs[from] == seq_N;
}

string get_one_transaction(uint32_t rank) // Maybe rename to get_one_transaction
	// se mempool estiver vazio retornar nada e parar geração do bloco
{
	while (1)
	{	
		string tx;
		if (PRINT_LOCKS) { cout << "Locking mempool_mtx in get_one_transaction" << endl; } mempool_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked mempool_mtx in get_one_transaction" << endl << flush; }
		if (!mempool.empty())
		{
			tx = mempool.front();
			mempool.pop_front();
		}
		mempool_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked mempool_mtx in get_one_transaction" << endl << flush; } 
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
			return 0;
		}

		while (l < BLOCK_SIZE_IN_BYTES)
		{
			string tx = get_one_transaction(rank);
			file << tx << endl;
			l += tx.size();
			no_txs++;
		}
		file.close();
	} else
	{
		while (l < BLOCK_SIZE_IN_BYTES)
		{
			string tx = get_one_transaction(rank);
			l += tx.size();
			no_txs++;
		}
	}
	return no_txs;
}

void verify_transaction(string full_tx, bool* is_new)
{
	//receive transaction before inserting in mempool

	//NOT SAVING TRANSACTIONS BECAUSE VERIFICATIONS FAIL OOPSSSSS

	vector<string> s = split(full_tx, ":");
	if (s.size() == 7)
	{
		string from = s[0];
		string seq = s[1];
		string to = s[2];
		string amount = s[3];
		string dependency = s[4] + ":" + s[5];
		string sign = s[6];

		uint64_t seq_N = stoull(seq);
		auto it_seq = next_seqs.find(from);

		if (from.size() != 8 * ADDRESS_SIZE_IN_DWORDS || to.size() != 8 * ADDRESS_SIZE_IN_DWORDS || amount.size() <= 0)
		{ // todo: problema com next seqs
			return; //ignore
        }  
		
		string tx = from + ":" + seq + ":" + to + ":" + amount + ":" + dependency;


		if (false && !verify_message(tx, sign)) //maybe ignore signatures
			return; //ignore


		string tx_key = from + ":" + seq;

		if (PRINT_LOCKS) { cout << "Locking aging_transactions_mtx in verify_transaction" << endl; } aging_transactions_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked aging_transactions_mtx in verify_transaction" << endl << flush; } //printf("Locked aging_transactions_mtx\n");
		if (PRINT_LOCKS) { cout << "Locking next_seqs_mt in verify_transactionx" << endl; } next_seqs_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked next_seqs_mtx in verify_transaction" << endl << flush; }
		if (PRINT_LOCKS) { cout << "Locking pending_transactions_mtx in verify_transaction" << endl; } pending_transactions_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked pending_transactions_mtx in verify_transaction" << endl << flush; }
		if (PRINT_LOCKS) { cout << "Locking mempool_mtx in verify_transaction" << endl; } mempool_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked mempool_mtx in verify_transaction" << endl << flush; } //printf("Locked mempool_mtx\n");
		if (PRINT_LOCKS) { cout << "Locking aged_transactions_mtx in verify_transaction" << endl; } aged_transactions_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked aged_transactions_mtx in verify_transaction" << endl << flush; } //printf("Locked aged_transactions_mtx\n");
		auto it_aging = aging_transactions.find(tx_key);
		auto it_aged = aged_transactions.find(tx_key);

		if (it_aging == aging_transactions.end() &&
			!is_promised(tx) &&
			it_aged == aged_transactions.end())
		{	// new transaction key never received
			if (!check_dependencies(from, seq_N, dependency) && pending_transactions.find(tx_key) == pending_transactions.end() /*verificação redundante pq transacao nnc foi recebida*/) {
				pending_transactions[tx_key] = full_tx;
			} else {
				mempool.push_back(full_tx);
			}
			aging_transactions[tx_key] = { full_tx, get_now() }; 
			*is_new = true;
		} else if (it_aging != aging_transactions.end())
		{ // transaction key is aging
			string previous_full_tx = it_aging->second.full_tx;
			if (previous_full_tx.compare(full_tx) != 0) // double spend
			{ 	

				aged_transactions[from + ":" + seq] = { it_aging->second.full_tx, 
														it_aging->second.time,
														get_now() }; //unsuccessfully aged
				aging_transactions.erase(it_aging);
			}
		} else // transaction key has been seen before but it's not aging
		{
			//  ignore since it's been promised/committed or unsucessfully aged
		}
		aged_transactions_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked aged_transactions_mtx in verify_transaction" << endl << flush; }  //printf("Unlocked aged_transactions_mtx\n");
		mempool_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked mempool_mtx in verify_transaction" << endl << flush; }  //printf("Unlocked mempool_mtx\n"); 
		pending_transactions_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked pending_transactions_mtx in verify_transaction" << endl << flush; } 
		next_seqs_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked next_seqs_mtx in verify_transaction" << endl << flush; } 
		aging_transactions_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked aging_transactions_mtx in verify_transaction" << endl << flush; }  //printf("Unlocked aging_transactions_mtx\n");
	} else
	{
		if (PRINT_TRANSMISSION_ERRORS)
		{
			cout << "Incorrect transaction size:" << s.size() << endl;
			cout << "tx:" << full_tx << endl;
		}
	}
}

string get_transaction_key(string tx) {
	vector<string> s = split(tx, ":");
	string from = s[0];
	string seq = s[1];
	string tx_key = from + ":" + seq ;
	return tx_key;
}

void remove_from_mempool_and_pending(string tx) {
	if (PRINT_LOCKS) { cout << "Locking pending_transactions_mtx in update_transactions_block" << endl; } pending_transactions_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked pending_transactions_mtx in update_transactions_block" << endl << flush; }
	if (PRINT_LOCKS) { cout << "Locking mempool_mtx in update_transactions_block" << endl; } mempool_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked pendinmempool_mtxg_transactions_mtx in update_transactions_block" << endl << flush; }
	string tx_key = get_transaction_key(tx);
	//transaction_block[tx_key] = block_hash;
	
	// transaction is in a block, so remove from pending transactions or mempool 
	auto it_pending_transactions = pending_transactions.find(tx_key);
	if (it_pending_transactions != pending_transactions.end()) {
		pending_transactions.erase(it_pending_transactions);
		return; // if it's in pending transactions then it's not in mempool
	}

	auto it_mempool = std::find_if(mempool.begin(), mempool.end(), [tx_key](string tx) { return get_transaction_key(tx) == tx_key; });
	if (it_mempool != mempool.end()) {
		//std::cout << "WORKS" << std::endl << flush;
		mempool.erase(it_mempool);
	}
	mempool_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked mempool_mtx in update_transactions_block" << endl << flush; }
	pending_transactions_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked pending_transactions_mtx in update_transactions_block" << endl << flush; }
}

bool verify_transaction_from_block(string full_tx, uint32_t rank, uint32_t last_rank)
// rank é o rank do bloco da transação, e last rank o último rank do último bloco dessa cadeia
{
	// me: guardar transação com instante de chegada (se ainda não existir)
	// me: rejeitar se transação já existir ou n tiver sufixo suficiente
	//     (se calhar passar informação sobre nº do bloco desta transação)
	// TODO: feels incomplete, add transactions to aging if not known?
	//       Also, if dependencies are not promised, ignore withou adding to
	//       pending and aging_transactions?
	vector<string> s = split(full_tx, ":");
	if (s.size() == 7)
	{
		string from = s[0];
		string seq = s[1];
		string to = s[2];
		string amount = s[3];
		string dependency = s[4] + ":" + s[5];
		string sign = s[6];

		uint32_t suffix_size = last_rank - rank;

		uint64_t seq_N = stoull(seq);
		if (PRINT_LOCKS) { cout << "Locking next_seqs_mtx in verify_transaction_from_block" << endl; } next_seqs_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked next_seqs_mtx in verify_transaction_from_block" << endl << flush; }
		uint64_t next_seq = get_next_seq(from);
		next_seqs_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked next_seqs_mtx in verify_transaction_from_block" << endl << flush; } 
		if (from.size() != 8 * ADDRESS_SIZE_IN_DWORDS 
			|| to.size() != 8 * ADDRESS_SIZE_IN_DWORDS || amount.size() <= 0 
			|| seq_N > next_seq || !is_promised(dependency))
			return false;

		string tx = from + ":" + seq + ":" + to + ":" + amount + ":" + dependency;

		// todo
		// se estiver na pending ou mempool remover de lá
		// se não for conhecida por a envelhecer

		remove_from_mempool_and_pending(tx);

		if (false && !verify_message(tx, sign))
			return false;

		string tx_key = from + ":" + seq;


		aged_transactions_mtx.lock();
		auto it_aged = aged_transactions.find(tx_key); //lock?
		if (it_aged != aged_transactions.end() && it_aged->second.full_tx.compare(full_tx) == 0
			&& RSS(it_aged->second) > suffix_size) // unsuccessfully aged and doesn't have enough suffix
			return false; 
		aged_transactions_mtx.unlock();

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

	//ignore for now

	return;

	unsigned long time_A = std::chrono::system_clock::now().time_since_epoch() /  std::chrono::milliseconds(1);


	unsigned long time_B = std::chrono::system_clock::now().time_since_epoch() /  std::chrono::milliseconds(1);
	if ((time_B - time_A) / 1000.0 > 2) {
	std::cout << "WARNING: " << (time_B - time_A) / 1000.0 << std::endl << flush;

	}

	// remove transactions that come in some block from mempool or pending transactions
	//if (PRINT_LOCKS) { cout << "Locking transaction_block_mtx" << endl; } transaction_block_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked transaction_block_mtx" << endl << flush; }
	if (PRINT_LOCKS) { cout << "Locking pending_transactions_mtx in update_transactions_block" << endl; } pending_transactions_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked pending_transactions_mtx in update_transactions_block" << endl << flush; }
	for (string t : txs) {
		string tx_key = get_transaction_key(t);
		//transaction_block[tx_key] = block_hash;
		
		// transaction is in a block, so remove from pending transactions or mempool 
		auto it_pending_transactions = pending_transactions.find(tx_key);
		if (it_pending_transactions != pending_transactions.end()) {
			pending_transactions.erase(it_pending_transactions);
			continue; // if it's in pending transactions then it's not in mempool
		}

		auto it_mempool = std::find_if(mempool.begin(), mempool.end(), [tx_key](string tx) { return get_transaction_key(tx) == tx_key; });
		if (it_mempool != mempool.end()) {
			mempool.erase(it_mempool);
		}
	}
	pending_transactions_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked pending_transactions_mtx in update_transactions_block" << endl << flush; }
	//transaction_block_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked transaction_block_mtx" << endl << flush; }  
}


int get_aging_count()
{
	if (PRINT_LOCKS) { cout << "Locking aging_transactions_mtx in get_aging_count" << endl; } aging_transactions_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked aging_transactions_mtx in get_aging_count" << endl << flush; } 
	int aging_total = aging_transactions.size();
	aging_transactions_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked aging_transactions_mtx in get_aging_count" << endl << flush; } 
	return aging_total;
}

vector<string> add_transactions(vector<string> transactions) // transactions received from broadcast
{
	//printf("Hmm, add_transaction(txs) received %ld transactions, ", transactions.size());
	int i = 0;
	bool is_new;
	vector<string> added_transactions;

	for (string transaction : transactions)
	{	
		is_new = false;
		verify_transaction(transaction, &is_new);
		if (is_new) {
			i++;	
			added_transactions.push_back(transaction);
		}
	}
	return added_transactions;
	//printf("but actually added %ld transactions\n", i);
}

bool bool_with_prob()
{
	return false;
}


void notify_dependencies(string from, string seq) {
	string tx = from + ":" + seq;

	// problems when explicit and implicit dependency are the same

	//implicit dependency
	uint64_t next_seq = get_next_seq(from);
	string next_tx_key = from + ":" + to_string(next_seq);
	auto it = pending_transactions.find(next_tx_key);
	if (it != pending_transactions.end()) {
		vector<string> s = split(it->second, ":");
		string next_tx_dependency = s[4] + ":" + s[5];
		if (check_dependencies(from, next_seq, next_tx_dependency)) {
			// std::cout << "Moving " << next_tx_key << " from pending transactions to mempool" << std::endl << flush;
			mempool.push_back(it->second); 
			pending_transactions.erase(next_tx_key);
		} 
	}
	
	//explicit dependencies
	vector<string> explicit_dependencies = dependencies[tx];
	for (auto it = explicit_dependencies.begin(); it != explicit_dependencies.end(); it++) {
		string tx_key = *it;
		if (tx_key == next_tx_key) continue; // implicit == explicit
		auto pending_tx_it = pending_transactions.find(tx_key);
		if (pending_tx_it == pending_transactions.end()) {
			std::cout << "INFO: Explicit dependency was not in pending transactions: " << std::endl << flush; // happening, whaaaaaaaaaat
			continue; //not found in pending transaction: shouldn't happen, unless received in a block
		}
		vector<string> s = split(pending_tx_it->second, ":");
		string pending_tx_dependency = s[4] + ":" + s[5];
		if (check_dependencies(from, next_seq, pending_tx_dependency)) {
			// std::cout << "Moving " << tx_key << " from pending transactions to mempool" << std::endl << flush;
			mempool.push_back(pending_tx_it->second); 
			pending_transactions.erase(pending_tx_it);
		} 
	}
	clear_dependencies(tx);
}

void promise_transaction(string full_tx, string from, uint64_t seq_N, int64_t age) {
	string seq = to_string(seq_N);
	if (seq_N == get_next_seq(from)) update_next_seq(from);
	notify_dependencies(from, seq);
	update_promise_stats(age);
	last_promised = from + ":" + seq;
	if (SAVE_TRANSACTIONS) {
		string filename = ser->get_transactions_file();
		ofstream file;
		file.open(filename, std::ios::app);
		file << full_tx << " - " << age << std::endl;
		file.close();
	}
}

// Tasks
void aging_monitor()
{
	boost::this_thread::sleep(boost::posix_time::milliseconds(IDLE_START_TIME));
	while (1)
	{
		int64_t start = get_now();
		if (PRINT_LOCKS) { cout << "Locking aging_transactions_mtx in aging_monitor" << endl; } aging_transactions_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked aging_transactions_mtx in aging_monitor" << endl << flush; }
		if (PRINT_LOCKS) { cout << "Locking next_seqs_mtx in aging_monitor" << endl; } next_seqs_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked next_seqs_mtx in aging_monitor" << endl << flush; }	
		if (PRINT_LOCKS) { cout << "Locking pending_transactions_mtx in aging_monitor" << endl; } pending_transactions_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked pending_transactions_mtx in aging_monitor" << endl << flush; }
		if (PRINT_LOCKS) { cout << "Locking mempool_mtx in aging_monitor" << endl; } mempool_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked mempool_mtx in aging_monitor" << endl << flush; }
		if (PRINT_LOCKS) { cout << "Locking dependencies_mtx in aging_monitor" << endl; } dependencies_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked dependencies_mtx in aging_monitor" << endl << flush; }
		if (PRINT_LOCKS) { cout << "Locking aged_transactions_mtx in aging_monitor" << endl; } aged_transactions_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked aged_transactions_mtx in aging_monitor" << endl << flush; }


		int i = 0; 
		for (auto it = aging_transactions.begin(); it != aging_transactions.end(); i++)
		{	
			int64_t now = get_now();
			int64_t age = now - it->second.time;
			vector<string> s = split(it->second.full_tx, ":");
			string from = s[0];
			string seq = s[1];
			uint64_t seq_N = stoull(seq);
			string dependency = s[4] + ":" + s[5];


			uint64_t next_seq = get_next_seq(from); // como next seqs é atualizado, podemos limpar transação do aged transactions
			if (age > AT && check_dependencies(from, seq_N, dependency))
			{	// dependencias explícita têm de ter envelhecido antes de envelhecer uma transação - todo
				promise_transaction(it->second.full_tx, from, seq_N, age);
				it = aging_transactions.erase(it);
			} else
			{ 
				++it;
			}
		}

		aged_transactions_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked aged_transactions_mtx in aging_monitor" << endl << flush; } 
		dependencies_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked dependencies_mtx in aging_monitor" << endl << flush; }
		mempool_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked mempool_mtx in aging_monitor" << endl << flush; } 
		pending_transactions_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked pending_transactions_mtx in aging_monitor" << endl << flush; } 
		next_seqs_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked next_seqs_mtx in aging_monitor" << endl << flush; } 	
		aging_transactions_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked aging_transactions_mtx in aging_monitor" << endl << flush; } 

		int64_t end = get_now(); //overkill?
		boost::this_thread::sleep(boost::posix_time::milliseconds(AGING_MONITOR_EACH_MILLISECONDS));
	}
}



void transaction_creator()
{
	//uint64_t seqN = 0; // no persistance but okay, also maybe use uint64_t or smth
	boost::this_thread::sleep(boost::posix_time::milliseconds(IDLE_START_TIME));

	map<string, uint64_t> next_new_seqs;
	while (1)
	{	
		int64_t start = get_now();
		if (PRINT_LOCKS) { cout << "Locking aging_transactions_mtx in transaction_creator" << endl; } aging_transactions_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked aging_transactions_mtx in transaction_creator" << endl << flush; }
		if (PRINT_LOCKS) { cout << "Locking next_seqs_mtx in transaction_creator" << endl; } next_seqs_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked next_seqs_mtx in transaction_creator" << endl << flush; }
		if (PRINT_LOCKS) { cout << "Locking pending_transactions_mtx in transaction_creator" << endl; } pending_transactions_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked pending_transactions_mtx in transaction_creator" << endl << flush; }
		if (PRINT_LOCKS) { cout << "Locking mempool_mtx in transaction_creator" << endl; } mempool_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked mempool_mtx in transaction_creator" << endl << flush; }

		vector<string> transactions;
		for (int i = 0; i < TRANSACTION_THROUGHPUT_EACH_NODE; i++) //careful with transaction size, are there restrictions?
		{ // tx/s per node
			string from = get_random_from_address(ADDRESS_SIZE_IN_DWORDS);
			uint64_t seq_N = next_new_seqs[from];
			string seq = to_string(seq_N);
			string to = get_random_address(ADDRESS_SIZE_IN_DWORDS);
			string amount = to_string(rng());
			string dependency = get_dependency(to);

			string tx = from + ":" + seq + ":" + to + ":" + amount + ":" + dependency;
			string sign = "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"; //sign_message(tx); //maybe ignore signing

			string full_tx = tx + ":" + sign;
			
			

			transactions.push_back(full_tx); //change create one transaction to use node id

			string tx_key = from + ":" + seq;
			aging_transactions[tx_key] = { full_tx, get_now() }; 
			
			if (!check_dependencies(from, seq_N, dependency)) {
				// std::cout << "Adding " << tx_key << " to pending transactions" << std::endl << flush;
				pending_transactions[tx_key] = full_tx;
			} else {
				// std::cout << "Adding " << tx_key << " to mempool transactions" << std::endl << flush;
				mempool.push_back(full_tx);
			}

			if (BIZANTINE && bool_with_prob())
			{ // todo: if (bizantine && ...)
				//repeat seq
				continue;
			}
			
			next_new_seqs[from] += 1;
		}
		mempool_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked mempool_mtx in transaction_creator" << endl << flush; } 
		pending_transactions_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked pending_transactions_mtx in transaction_creator" << endl << flush; }
		next_seqs_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked next_seqs_mtx in transaction_creator" << endl << flush; } 
		aging_transactions_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked aging_transactions_mtx in transaction_creator" << endl << flush; } 

		string s = create__transactions(transactions);
		//printf("Sending transactions\n");
		ser->write_to_all_peers(s);

		int64_t end = get_now();
		boost::this_thread::sleep(boost::posix_time::milliseconds(1000)); //remaining time to complete second
	}
}

void commit_block(BlockHash block_hash) { 
	//probabily problem because not adding my transactions to node, program fails because then overflow
	
	if (WRITE_BLOCKS_TO_HDD) {

		if (PRINT_LOCKS) { cout << "Locking next_seqs_mtx in commit_block" << endl; } next_seqs_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked next_seqs_mtx in commit_block" << endl << flush; }
		//if (PRINT_LOCKS) { cout << "Locking transaction_block_mtx" << endl; } transaction_block_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked transaction_block_mtx" << endl << flush; }
		if (PRINT_LOCKS) { cout << "Locking dependencies_mtx in commit_block" << endl; } dependencies_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked dependencies_mtx in commit_block" << endl << flush; }
		if (PRINT_LOCKS) { cout << "Locking aged_transactions_mtx in commit_block" << endl; } aged_transactions_mtx.lock(); if (PRINT_LOCKS) { cout << "Locked aged_transactions_mtx in commit_block" << endl << flush; }

		string filename = ser->get_server_folder()+"/"+blockhash_to_string(block_hash);
		std::cout << "Reading file " << filename << std::endl << flush;
		ifstream file(filename);
        
		if (file.is_open()) { 
			string tx;
			int i = 0;
			while (std::getline(file, tx)) {
				string tx_key = get_transaction_key(tx);
				
				if (!is_promised(tx_key)) {
					int64_t now = get_now();
					int64_t age;
					auto it = aged_transactions.find(tx_key);
					if (it != aged_transactions.end() && 
						it->second.full_tx.compare(tx) == 0) { // unsuccessfully aged
						age = now - it->second.start_time;
						aged_transactions.erase(it);
					} else {
						age = 0; // weird if happens, but possible
					}
					vector<string> s = split(tx_key, ":");
					string from = s[0];
					string seq = s[1];
					uint64_t seq_N = stoull(seq);
					promise_transaction(tx, from, seq_N, age);
				}
				
				i++;
			}
			std::cout << "Committed " << i << " transactions" << std::endl << flush;
			file.close();
		} else {  
			std::cout << "PANIC: BLOCK IS NOT STORED" << std::endl << flush;
			exit(-1); 
		}
		aged_transactions_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked aged_transactions_mtx in commit_block" << endl << flush; } 
		dependencies_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked dependencies_mtx in commit_block" << endl << flush; }
		//transaction_block_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked transaction_block_mtx" << endl << flush; } 
		next_seqs_mtx.unlock(); if (PRINT_LOCKS) { cout << "Unlocked next_seqs_mtx in commit_block" << endl << flush; } 		
	} else {
		// doesn't work
	}
}

