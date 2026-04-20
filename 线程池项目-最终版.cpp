#include <iostream>
#include <vector>
#include <future>
#include <thread>
#include <chrono>
#include <numeric>
#include <cmath>
#include <atomic>
#include "threadpool.h"

using Clock = std::chrono::high_resolution_clock;

static inline int cpu_task(int seed) {
    // 纯 CPU 任务，避免 I/O 干扰
    uint64_t x = static_cast<uint64_t>(seed) + 0x9e3779b97f4a7c15ULL;
    for (int i = 0; i < 2000; ++i) {
        x ^= (x << 13);
        x ^= (x >> 7);
        x ^= (x << 17);
        x += i;
    }
    return static_cast<int>(x & 0x7fffffff);
}

struct Result {
    double ms = 0.0;
    double tps = 0.0;
};

Result run_serial(int N) {
    auto t0 = Clock::now();
    volatile int sink = 0;
    for (int i = 0; i < N; ++i) {
        sink ^= cpu_task(i);
    }
    auto t1 = Clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return { ms, N / (ms / 1000.0) };
}

Result run_fixed(int N, int threads) {
    ThreadPool pool;
    pool.setMode(PoolMode::MODE_FIXED);
    pool.start(threads);

    std::vector<std::future<int>> futs;
    futs.reserve(N);

    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
        futs.emplace_back(pool.submitTask(cpu_task, i));
    }
    volatile int sink = 0;
    for (auto& f : futs) sink ^= f.get();
    auto t1 = Clock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return { ms, N / (ms / 1000.0) };
}

Result run_cached(int N, int baseThreads, int maxThreads) {
    ThreadPool pool;
    pool.setMode(PoolMode::MODE_CACHED);
    pool.setThreadSizeThreashHold(maxThreads);
    pool.start(baseThreads);

    std::vector<std::future<int>> futs;
    futs.reserve(N);

    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
        futs.emplace_back(pool.submitTask(cpu_task, i));
    }
    volatile int sink = 0;
    for (auto& f : futs) sink ^= f.get();
    auto t1 = Clock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return { ms, N / (ms / 1000.0) };
}

// 多生产者并发提交压力（检验 submit 竞争）
Result run_cached_multi_producer(int N, int producerCount, int baseThreads, int maxThreads) {
    ThreadPool pool;
    pool.setMode(PoolMode::MODE_CACHED);
    pool.setThreadSizeThreashHold(maxThreads);
    pool.start(baseThreads);

    std::vector<std::future<int>> futs;
    futs.reserve(N);
    std::mutex futsMtx;
    std::atomic<int> idx{ 0 };

    auto t0 = Clock::now();
    std::vector<std::thread> producers;
    for (int p = 0; p < producerCount; ++p) {
        producers.emplace_back([&]() {
            while (true) {
                int i = idx.fetch_add(1);
                if (i >= N) break;
                auto f = pool.submitTask(cpu_task, i);
                std::lock_guard<std::mutex> lk(futsMtx);
                futs.emplace_back(std::move(f));
            }
                               });
    }
    for (auto& t : producers) t.join();

    volatile int sink = 0;
    for (auto& f : futs) sink ^= f.get();
    auto t1 = Clock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return { ms, N / (ms / 1000.0) };
}

int main() {
    const int N = 100000;
    const int hw = std::max(1u, std::thread::hardware_concurrency());

    // 预热
    (void)run_serial(10000);
    (void)run_fixed(10000, hw);
    (void)run_cached(10000, hw / 2 + 1, hw * 2);

    // 正式
    auto s = run_serial(N);
    auto f = run_fixed(N, hw);
    auto c = run_cached(N, hw / 2 + 1, hw * 2);
    auto mp = run_cached_multi_producer(N, 4, hw / 2 + 1, hw * 2);

    std::cout << "N=" << N << "\n";
    std::cout << "Serial  : " << s.ms << " ms, " << s.tps << " tasks/s\n";
    std::cout << "Fixed   : " << f.ms << " ms, " << f.tps << " tasks/s, speedup=" << (s.ms / f.ms) << "x\n";
    std::cout << "Cached  : " << c.ms << " ms, " << c.tps << " tasks/s, speedup=" << (s.ms / c.ms) << "x\n";
    std::cout << "Cached+MP(4 producers): " << mp.ms << " ms, " << mp.tps << " tasks/s, speedup=" << (s.ms / mp.ms) << "x\n";
}