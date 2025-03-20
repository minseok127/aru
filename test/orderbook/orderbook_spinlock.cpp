#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <map>
#include <cstring>   // for strdup
#include <cstdlib>   // for abort
#include <pthread.h>
#include "../nlohmann/json.hpp"

// -------------------------------------
// Book struct: 한 오더북 (bids, asks)
// -------------------------------------
struct Book {
    std::map<std::string, std::string> bids;
    std::map<std::string, std::string> asks;

	pthread_spinlock_t spinlock;
};

// 전역 N개 오더북
static std::vector<Book> g_books;
static int g_numBooks = 0;

// 통계 카운터
static std::atomic<int> g_updateCount(0);
static std::atomic<int> g_readCount(0);

// 실행 중 여부
static std::atomic<bool> g_running(true);

// 고정 가격 20개
static const int fixedPrices[20] = {
    19000, 19050, 19100, 19150, 19200,
    19250, 19300, 19350, 19400, 19450,
    19500, 19550, 19600, 19650, 19700,
    19750, 19800, 19850, 19900, 19950
};

// ---------------------------------------------------
// generate JSON for a single random book_id
// all 20 bids/asks have the same qty
// ---------------------------------------------------
std::string generateOrderbookJson_forRandomBook(int numBooks, std::mt19937 &rng)
{
    // book_id: 0 ~ numBooks-1
    std::uniform_int_distribution<int> bookDist(0, numBooks - 1);
    int book_id = bookDist(rng);

    // qtyDist(0.0 ~ 5.0)
    std::uniform_real_distribution<double> qtyDist(0.0, 5.0);
    double finalQty = qtyDist(rng);

    // build JSON
    nlohmann::json j;
    j["book_id"] = book_id;

    nlohmann::json bids = nlohmann::json::array();
    nlohmann::json asks = nlohmann::json::array();

    for (int i = 0; i < 20; i++) {
        int price = fixedPrices[i];
        bids.push_back({std::to_string(price), std::to_string(finalQty)});
    }
    for (int i = 0; i < 20; i++) {
        int price = fixedPrices[i];
        asks.push_back({std::to_string(price), std::to_string(finalQty)});
    }
    j["b"] = bids;
    j["a"] = asks;

    return j.dump();
}

// ---------------------------------------------------
// update 콜백: parse JSON, update g_books[book_id]
// ---------------------------------------------------
void updateOrderbookCallback(void* args)
{
    char* clonedJson = static_cast<char*>(args);
    g_updateCount.fetch_add(1, std::memory_order_relaxed);

    try {
        nlohmann::json j = nlohmann::json::parse(clonedJson);
        int book_id = j["book_id"].get<int>();

		pthread_spin_lock(&g_books[book_id].spinlock);
        // 매수
        if (j.contains("b")) {
            for (auto& bid : j["b"]) {
                std::string price = bid[0].get<std::string>();
                std::string qty   = bid[1].get<std::string>();
                g_books[book_id].bids[price] = qty;
            }
        }
        // 매도
        if (j.contains("a")) {
            for (auto& ask : j["a"]) {
                std::string price = ask[0].get<std::string>();
                std::string qty   = ask[1].get<std::string>();
                g_books[book_id].asks[price] = qty;
            }
        }
		pthread_spin_unlock(&g_books[book_id].spinlock);
    } catch (...) {
        // parse error
    }

    free(clonedJson);
	//std::cout << "update done" << std::endl;
}

// ---------------------------------------------------
// read 콜백: check that ALL prices in this book
// have the same qty
// ---------------------------------------------------
void readBookCallback(void* args)
{
    g_readCount.fetch_add(1, std::memory_order_relaxed);

    // args: book_id
    int book_id = reinterpret_cast<intptr_t>(args);

    // bids
    double referenceQty = -1.0;
    bool firstFound = false;

	pthread_spin_lock(&g_books[book_id].spinlock);
    for (auto &kv : g_books[book_id].bids) {
        double d = std::stod(kv.second);
        if (!firstFound) {
            referenceQty = d;
            firstFound = true;
        } else {
            if (d != referenceQty) {
                std::cerr << "[ERROR] Mismatch in bids of book " 
                          << book_id << ": " << d << " != " << referenceQty << std::endl;
                std::abort();
            }
        }
    }
    // asks
    for (auto &kv : g_books[book_id].asks) {
        double d = std::stod(kv.second);
        if (!firstFound) {
            referenceQty = d;
            firstFound = true;
        } else {
            if (d != referenceQty) {
                std::cerr << "[ERROR] Mismatch in asks of book " 
                          << book_id << ": " << d << " != " << referenceQty << std::endl;
                std::abort();
            }
        }
    }
	pthread_spin_unlock(&g_books[book_id].spinlock);
	//std::cout << "read done@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@" << std::endl;
    // if empty, skip. if all same => OK
}

// ---------------------------------------------------
// 업데이트 스레드
// ---------------------------------------------------
void updateThreadFunc(void *)
{
    std::random_device rd;
    std::mt19937 rng(rd());

    while (g_running.load(std::memory_order_relaxed)) {
        // create JSON for a random book
        std::string jsonData = generateOrderbookJson_forRandomBook(g_numBooks, rng);

        // strdup
        char* cloned = strdup(jsonData.c_str());
        if (!cloned) break; // memory fail

        updateOrderbookCallback(cloned);
        // optional sleep to reduce CPU
        //std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
}

// ---------------------------------------------------
// 읽기 스레드
// "청크" 제거: 모든 book에 대해 비동기 read -> 태그 다 확인 -> 반복
// ---------------------------------------------------
void readThreadFunc_allBooks(void *)
{
    while (g_running.load(std::memory_order_relaxed)) {
        // 1) 모든 book에 대해 비동기 read
        for (int i = 0; i < g_numBooks; i++) {
            // args: book_id (int)
            // 여기서 intptr_t로 변환
            void* bookIdPtr = reinterpret_cast<void*>(static_cast<intptr_t>(i));
            readBookCallback(bookIdPtr);
        }
    }
}

// ---------------------------------------------------
// main
// ---------------------------------------------------
int main(int argc, char* argv[])
{
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <updateThreads> <readThreads> <numBooks> <runSeconds>\n";
        return 1;
    }

    int updateThreads = std::atoi(argv[1]);
    int readThreads   = std::atoi(argv[2]);
    g_numBooks        = std::atoi(argv[3]);
    double runSeconds = std::atof(argv[4]);

    if (updateThreads < 0 || readThreads < 0 || g_numBooks <= 0 || runSeconds <= 0.0) {
        std::cerr << "Invalid arguments.\n";
        return 1;
    }


    // 1) g_books 초기화
    g_books.resize(g_numBooks);
	for (int i = 0; i < g_numBooks; i++) {
		pthread_spin_init(&g_books[i].spinlock, PTHREAD_PROCESS_PRIVATE);
	}
	
    // 3) 스레드 생성
    std::vector<std::thread> threads;
    threads.reserve(updateThreads + readThreads);

    // update threads
    for (int i = 0; i < updateThreads; i++) {
        threads.emplace_back([=](){
            updateThreadFunc(NULL);
        });
    }

    // read threads
    for (int i = 0; i < readThreads; i++) {
        threads.emplace_back([=](){
            readThreadFunc_allBooks(NULL);
        });
    }

    auto startTime = std::chrono::steady_clock::now();

    // 4) runSeconds 후 종료
    std::this_thread::sleep_for(std::chrono::milliseconds((int)(runSeconds * 1000)));

    g_running.store(false, std::memory_order_relaxed);

    // 5) join
    for (auto &th : threads) {
        th.join();
    }

    auto endTime = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(endTime - startTime).count();

    // 6) 결과
    std::cout << "Update callbacks: " << g_updateCount.load() << "\n";
    std::cout << "Read callbacks:   " << g_readCount.load() << "\n";
    std::cout << "Elapsed time:     " << elapsedSec << " sec\n";

    return 0;
}

