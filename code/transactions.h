#ifndef TRANSACTIONS_H
#define TRANSACTIONS_H

#include <algorithm>
#include <stdint.h>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <random>
#include <boost/chrono.hpp>
#include <boost/thread/thread.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <chrono>

#include "Blockchain.hpp"
#include "params.h"
using namespace std::chrono;

extern mt19937 rng;

using namespace std;

typedef struct aginginfo {
    string full_tx;
    int64_t time;
} aging_info;

typedef struct agedinfo {
    string full_tx;
    int64_t start_time;
    int64_t end_time;
} aged_info;

string get_one_transaction(uint32_t rank);
int create_transaction_block(BlockHash hash, string filename, uint32_t rank);
bool verify_transaction_from_block(string tx, uint32_t rank, uint32_t last_rank);
void update_transactions_block(list<string> txs, BlockHash block_hash);
void aging_monitor();
int64_t get_average_promise_time();
void transaction_creator();
int get_aging_count();
int get_aged_count();
int get_pending_count();
int get_mempool_count();
int get_promised_count();
vector<string> add_transactions(vector<string> transactions);
void commit_block(BlockHash block_hash);

#endif