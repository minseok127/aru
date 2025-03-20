#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <map>
#include <cstring>  // for strdup
#include <cstdlib>  // for abort
#include "../nlohmann/json.hpp"

#include "../../aru.h"

// -------------------------------------
// Book struct
//  - map bids, asks
//  - aru* for concurrency mgmt
// -------------------------------------
struct Book {
    // 실제 호가 데이터
    std::map<std::string, std::string> bids;
    std::map<std::string, std::string> asks;

    // 이 책 고유의 aru 인스턴스
    aru* book_aru;
};

// 전역 N개 오더북
static std::vector<Book> g_books;
static int g_numBooks = 0;

// 실행 중 여부
static std::atomic<bool> g_running(true);

// 통계 카운터
static std::atomic<int> g_updateCount(0);
static std::atomic<int> g_updateCount2(0);
static std::atomic<int> g_readCount(0);
static std::atomic<int> g_readCount2(0);

// 고정 가격 20개
static const int fixedPrices[20] = {
    19000, 19050, 19100, 19150, 19200,
    19250, 19300, 19350, 19400, 19450,
    19500, 19550, 19600, 19650, 19700,
    19750, 19800, 19850, 19900, 19950
};

// ---------------------------------------------------
// JSON 생성 (book_id, 20개 고정가격, 동일 qty)
// ---------------------------------------------------
std::string generateOrderbookJson_forOneBook(int book_id, std::mt19937 &rng)
{
    std::uniform_real_distribution<double> qtyDist(0.0, 5.0);
    double finalQty = qtyDist(rng);

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
// update 콜백: 해당 book_id의 bids/asks 수정
// ---------------------------------------------------
void updateBookCallback(void* args)
{
    char* clonedJson = static_cast<char*>(args);

    g_updateCount.fetch_add(1, std::memory_order_relaxed);
    if (!g_running.load(std::memory_order_relaxed)) {
	std::cout << "upadate: " << g_updateCount2.load() - g_updateCount.load() << std::endl;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(clonedJson);
        int book_id = j["book_id"].get<int>();

        // 매수(b)
        if (j.contains("b")) {
            for (auto &bid : j["b"]) {
                std::string price = bid[0].get<std::string>();
                std::string qty   = bid[1].get<std::string>();
                g_books[book_id].bids[price] = qty;
            }
        }
        // 매도(a)
        if (j.contains("a")) {
            for (auto &ask : j["a"]) {
                std::string price = ask[0].get<std::string>();
                std::string qty   = ask[1].get<std::string>();
                g_books[book_id].asks[price] = qty;
            }
        }
    } catch (...) {
        // parse error
    }

    free(clonedJson);

	//std::cout << "update done" << std::endl;
}

// ---------------------------------------------------
// read 콜백: 해당 book_id 전체 호가가 동일 qty인지 확인
// ---------------------------------------------------
void readBookCallback(void* args)
{
    g_readCount.fetch_add(1, std::memory_order_relaxed);
    if (!g_running.load(std::memory_order_relaxed)) {
	std::cout << "read: " << g_readCount2.load() - g_readCount.load() << std::endl;
    }

    // args = book_id
    int book_id = reinterpret_cast<intptr_t>(args);

    double referenceQty = -1.0;
    bool firstFound = false;

    // bids
    for (auto &kv : g_books[book_id].bids) {
        double d = std::stod(kv.second);
        if (!firstFound) {
            referenceQty = d;
            firstFound = true;
        } else {
            if (d != referenceQty) {
                std::cerr << "[ERROR] Mismatch (bids) in book " 
                          << book_id << ": " << d << " != " << referenceQty << "\n";
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
                std::cerr << "[ERROR] Mismatch (asks) in book "
                          << book_id << ": " << d << " != " << referenceQty << "\n";
                std::abort();
            }
        }
    }

	//std::cout << "read done@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@" << std::endl;
}

// ---------------------------------------------------
// Update 스레드
//  - 무작위로 book_id 골라서 book_aru에 업데이트
// ---------------------------------------------------
void updateThreadFunc()
{
    std::random_device rd;
    std::mt19937 rng(rd());

    std::uniform_int_distribution<int> bookDist(0, g_numBooks - 1);

    while (g_running.load(std::memory_order_relaxed)) {
        int book_id = bookDist(rng);

        std::string jsonData = generateOrderbookJson_forOneBook(book_id, rng);
        char* cloned = strdup(jsonData.c_str());
        if (!cloned) break;

        // book별 aru
        aru* myAru = g_books[book_id].book_aru;
        // tag = nullptr
        aru_update(myAru, nullptr, updateBookCallback, cloned);
	g_updateCount2.fetch_add(1, std::memory_order_relaxed);

        // CPU 부담 낮추기 위해 잠깐 sleep할 수 있음
        //std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
}

// ---------------------------------------------------
// Read 스레드
//  - 모든 book에 대해 aru_read -> 모두 끝나면 반복
// ---------------------------------------------------
void readThreadFunc_allBooks()
{
    while (g_running.load(std::memory_order_relaxed)) {
        // 태그 배열
        //std::vector<aru_tag> tags(g_numBooks, ARU_TAG_PENDING);

        // 1) 모든 book에 대해 read 요청
        for (int i = 0; i < g_numBooks; i++) {
            // args: book_id
            void* bookIdPtr = reinterpret_cast<void*>(static_cast<intptr_t>(i));
            aru* myAru = g_books[i].book_aru;
            //aru_read(myAru, &tags[i], readBookCallback, bookIdPtr);
	    aru_read(myAru, NULL, readBookCallback, bookIdPtr);
	    g_readCount2.fetch_add(1, std::memory_order_relaxed);
        }

#if 0
        // 2) 태그가 모두 DONE인지 polling
        bool allDone = false;
        while (!allDone && g_running.load(std::memory_order_relaxed)) {
            allDone = true;
            for (int i = 0; i < g_numBooks; i++) {
                if (tags[i] != ARU_TAG_DONE) {
					aru_sync(g_books[i].book_aru);
                    allDone = false;
                    break;
                }
            }
            // 1ms sleep 제거 -> 바로 재확인 가능
        }
#endif
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
        std::cerr << "Invalid args\n";
        return 1;
    }

    // 1) Book + aru init
    g_books.resize(g_numBooks);
    for (int i = 0; i < g_numBooks; i++) {
        // 각 book마다 aru_init()
        g_books[i].book_aru = aru_init();
        if (!g_books[i].book_aru) {
            std::cerr << "aru_init() failed on book " << i << "\n";
            return 1;
        }
    }

    // 2) 스레드
    std::vector<std::thread> threads;
    threads.reserve(updateThreads + readThreads);

    for (int i = 0; i < updateThreads; i++) {
        threads.emplace_back(updateThreadFunc);
    }
    for (int i = 0; i < readThreads; i++) {
        threads.emplace_back(readThreadFunc_allBooks);
    }

    // 3) runSeconds 후 종료
    auto startTime = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds((int)(runSeconds * 1000)));

    g_running.store(false, std::memory_order_relaxed);

	// 4) join
    for (auto &th : threads) {
        th.join();
    }	
    auto endTime = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(endTime - startTime).count();

    // 5) 결과 출력
    std::cout << "Update callbacks: " << g_updateCount.load() << "\n";
    std::cout << "Read callbacks:   " << g_readCount.load() << "\n";
    std::cout << "Elapsed time:     " << elapsedSec << " sec\n";

	// 6) aru_destroy
    for (int i = 0; i < g_numBooks; i++) {
        aru_destroy(g_books[i].book_aru);
    }

    return 0;
}

