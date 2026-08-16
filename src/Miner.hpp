#pragma once
#include "StratumClient.hpp"
#include "Config.hpp"
#include <randomx.h>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>

struct Worker {
    int index;
    std::thread thread;
    randomx_vm* vm = nullptr;
    uint64_t totalHashes = 0;
};

struct MiningStats {
    uint64_t totalHashes = 0;
    double hashrate = 0.0;
    int activeWorkers = 0;
};

class Miner {
public:
    Miner();
    ~Miner();
    void start(const MinerConfig& cfg);
    void stop();
    bool isRunning() const;
    void printStats() const;

private:
    void initDataset();
    void workerLoop(Worker* w);
    void onNewJob(const Job& job);
    void onShareResult(bool accepted, const std::string& msg);
    
    // Dataset shared across threads
    randomx_dataset* m_dataset = nullptr;
    randomx_cache* m_cache = nullptr;
    randomx_flags m_flags;
    
    // Workers
    std::vector<std::unique_ptr<Worker>> m_workers;
    int m_numThreads = 1;
    
    // Mining state — job is swapped atomically
    std::mutex m_jobMutex;
    Job m_jobStorage;       // written under lock, read via pointer
    std::atomic<Job*> m_jobp{nullptr};
    
    // Batch-friendly job data (pre-decoded, pre-parsed)
    std::atomic<uint64_t> m_globalNonce{0};
    
    // Stats
    mutable std::mutex m_statsMutex;
    std::chrono::steady_clock::time_point m_statsStart;
    uint64_t m_acceptedShares = 0;
    uint64_t m_rejectedShares = 0;
    
    // Control
    std::atomic<bool> m_running{false};
    std::unique_ptr<StratumClient> m_client;
    int m_logLevel = 1;        // 0=QUIET 1=SHARE 2=FULL
};
