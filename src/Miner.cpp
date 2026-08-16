#include "Miner.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <pthread.h>
#include <unistd.h>
#include <sys/resource.h>
#include <cmath>
#include <endian.h>

// ============================================================
// SHA3-512 — via picosha3 (same as coreminer, header-only, fast)
// ============================================================
#include "picosha3.h"

static void sha3_512(const uint8_t* input, size_t len, uint8_t output[64]) {
    auto gen = picosha3::get_sha3_generator<512>();
    gen(input, input + len, output, output + 64);
}

// ============================================================
// Hex helpers
// ============================================================
static std::string bytes_to_hex(const uint8_t* data, int len) {
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.resize(len * 2);
    for (int i = 0; i < len; i++) {
        s[i*2]   = hex[(data[i] >> 4) & 0xf];
        s[i*2+1] = hex[data[i] & 0xf];
    }
    return s;
}

// ============================================================
// Miner
// ============================================================
Miner::Miner() : m_statsStart(std::chrono::steady_clock::now()) {}
Miner::~Miner() { stop(); }

void Miner::start(const MinerConfig& cfg) {
    if (m_running) return;

    // Log level: 0=QUIET (error+hashrate), 1=SHARE (default), 2=FULL (segala hal)
    m_logLevel = std::max(0, std::min(2, cfg.logLevel));

    if (m_logLevel >= 1)
        std::cout << "[Miner] Starting with " << cfg.threads << " threads" << std::endl;

    m_numThreads = cfg.threads;
    
    // --- RandomY flags ---
    m_flags = randomx_get_flags();
    if (cfg.fullMem) {
        // Full mem: explicitly add FULL_MEM (randomx_get_flags may not set it)
        m_flags = static_cast<randomx_flags>(m_flags | RANDOMX_FLAG_FULL_MEM);
    } else {
        // Light mode: ensure FULL_MEM is removed
        m_flags = static_cast<randomx_flags>(m_flags & ~RANDOMX_FLAG_FULL_MEM);
    }
    // JIT & hardware AES: hormati cfg.useJIT (flag --no-jit)
    if (cfg.useJIT)
        m_flags = static_cast<randomx_flags>(m_flags | RANDOMX_FLAG_JIT);
    else
        m_flags = static_cast<randomx_flags>(m_flags & ~RANDOMX_FLAG_JIT);
    m_flags = static_cast<randomx_flags>(m_flags | RANDOMX_FLAG_HARD_AES);

    // --- Auto-detect hugepages ---
    // Kalau cfg.largePages=true tapi kernel tak menyediakan hugepages
    // (umumnya di container/k8s tanpa privileged), alloc cache bakal gagal.
    // Deteksi /proc/meminfo (terlihat di dalam container, sysfs di-mask
    // Docker/k8s), set flag kalau tersedia, auto-disable kalau 0.
    if (cfg.largePages) {
        long freeHuge = -1;
        {
            std::ifstream f("/proc/meminfo");
            std::string line;
            while (std::getline(f, line)) {
                if (line.rfind("HugePages_Free:", 0) == 0) {
                    std::istringstream iss(line.substr(15));
                    iss >> freeHuge;
                }
            }
        }
        if (freeHuge <= 0) {
            m_flags = static_cast<randomx_flags>(m_flags & ~RANDOMX_FLAG_LARGE_PAGES);
            std::cerr << "[Miner] No free hugepages available (free=" << freeHuge << ") — "
                      << "auto-disabling LARGE_PAGES (set vm.nr_hugepages for boost)"
                      << std::endl;
        } else {
            m_flags = static_cast<randomx_flags>(m_flags | RANDOMX_FLAG_LARGE_PAGES);
        }
    } else {
        m_flags = static_cast<randomx_flags>(m_flags & ~RANDOMX_FLAG_LARGE_PAGES);
    }
    
    if (m_logLevel >= 2)
        std::cout << "[Miner] RandomY flags: "
                  << "JIT="         << ((m_flags & RANDOMX_FLAG_JIT) != 0)
                  << " FULL_MEM="   << ((m_flags & RANDOMX_FLAG_FULL_MEM) != 0)
                  << " HARD_AES="   << ((m_flags & RANDOMX_FLAG_HARD_AES) != 0)
                  << " LARGE_PAGES="<< ((m_flags & RANDOMX_FLAG_LARGE_PAGES) != 0)
                  << std::endl;

    // --- Dataset ---
    const char key[] = {'5', '6', '7', '8', '9'}; // RandomY Core Coin fixed key
    auto t1 = std::chrono::steady_clock::now();

    if (cfg.fullMem) {
        // Full mem: cache → dataset → release cache
        m_cache = randomx_alloc_cache(m_flags);
        if (!m_cache) { std::cerr << "[Miner] Cache alloc failed" << std::endl; return; }
        randomx_init_cache(m_cache, key, sizeof(key));

        m_dataset = randomx_alloc_dataset(m_flags);
        if (!m_dataset) { std::cerr << "[Miner] Dataset alloc failed" << std::endl; return; }

        uint32_t datasetItems = randomx_dataset_item_count();
        if (m_logLevel >= 2)
            std::cout << "[Miner] Dataset items: " << datasetItems << std::endl;

        // Parallel dataset init: split work across threads
        int nthreads = std::min(cfg.threads, static_cast<int>(std::thread::hardware_concurrency()));
        if (nthreads < 1) nthreads = 1;
        uint32_t chunk = datasetItems / nthreads;
        std::vector<std::thread> initThreads;
        for (int t = 0; t < nthreads; t++) {
            uint32_t start = t * chunk;
            uint32_t count = (t == nthreads - 1) ? (datasetItems - start) : chunk;
            initThreads.emplace_back([this, start, count]() {
                randomx_init_dataset(m_dataset, m_cache, start, count);
            });
        }
        for (auto& th : initThreads) th.join();

        randomx_release_cache(m_cache);
        m_cache = nullptr;
    } else {
        // Light mode: cache only (VM will use cache for hash calculations)
        m_cache = randomx_alloc_cache(m_flags);
        if (!m_cache) { std::cerr << "[Miner] Cache alloc failed" << std::endl; return; }
        randomx_init_cache(m_cache, key, sizeof(key));
        if (m_logLevel >= 1)
            std::cout << "[Miner] Light mode — using cache (no full dataset)" << std::endl;
    }
    
    auto t2 = std::chrono::steady_clock::now();
    auto initMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    std::cout << "[Miner] Ready in " << (initMs / 1000.0) << "s" << std::endl;

    // --- Worker threads ---
    for (int i = 0; i < m_numThreads; i++) {
        auto w = std::make_unique<Worker>();
        w->index = i;
        // Light mode: VM uses cache. Full mem: VM uses dataset.
        w->vm = randomx_create_vm(m_flags, m_cache, m_dataset);
        if (!w->vm) {
            std::cerr << "[Miner] VM creation failed for worker " << i << std::endl;
            continue;
        }
        if (m_logLevel >= 2)
            std::cout << "[Miner] Worker " << i << " VM created" << std::endl;
        w->thread = std::thread(&Miner::workerLoop, this, w.get());
        m_workers.push_back(std::move(w));
    }
    
    m_running = true;

    // --- Stratum client ---
    auto& pool = cfg.pools[0];
    m_client = std::make_unique<StratumClient>(
        pool.host, pool.port, pool.wallet, pool.worker, pool.password, pool.tls
    );
    m_client->setJobCallback([this](const Job& job) { onNewJob(job); });
    m_client->setResultCallback([this](bool ok, const std::string& msg) { onShareResult(ok, msg); });
    m_client->connect();

    // --- Stats printer thread ---
    std::thread statsThread([this]() {
        int count = 0;
        while (m_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (m_running) {
                printStats();
                if (++count % 12 == 0) { // every ~60s
                    std::cout << "[Miner] Shares: " << m_acceptedShares 
                              << " accepted, " << m_rejectedShares << " rejected" << std::endl;
                }
            }
        }
    });
    statsThread.detach();
}

void Miner::stop() {
    if (!m_running.exchange(false)) return;
    std::cout << "[Miner] Stopping..." << std::endl;
    
    if (m_client) { m_client->disconnect(); m_client.reset(); }
    
    for (auto& w : m_workers) {
        if (w->thread.joinable()) w->thread.join();
        if (w->vm) randomx_destroy_vm(w->vm);
    }
    m_workers.clear();
    
    if (m_dataset) { randomx_release_dataset(m_dataset); m_dataset = nullptr; }
    if (m_cache) { randomx_release_cache(m_cache); m_cache = nullptr; }
    std::cout << "[Miner] Stopped" << std::endl;
}

bool Miner::isRunning() const { return m_running; }

void Miner::onNewJob(const Job& job) {
    std::lock_guard<std::mutex> lock(m_jobMutex);
    m_jobStorage = job;

    m_jobp.store(&m_jobStorage, std::memory_order_release);
    m_globalNonce.store(0, std::memory_order_relaxed);
    
    if (m_logLevel >= 1)
        std::cout << "[Miner] Job " << job.jobId << " — target=" 
                  << std::hex << job.targetInt << std::dec << std::endl;
}

void Miner::onShareResult(bool accepted, const std::string& msg) {
    if (accepted) {
        m_acceptedShares++;
        if (m_logLevel >= 1 || m_acceptedShares % 50 == 0)
            std::cout << "[Miner] ✅ Share ACCEPTED (#" << m_acceptedShares << ")" << std::endl;
    } else {
        m_rejectedShares++;
        if (m_rejectedShares <= 5 || m_rejectedShares % 10 == 0)
            std::cerr << "[Miner] ❌ Share rejected: " << msg << std::endl;
    }
}

// ============================================================
// WORKER LOOP — fully optimized hot path
// ============================================================
void Miner::workerLoop(Worker* w) {
    // Pin to specific CPU core (w->index % ncpus)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        int ncpus = std::max(1, static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)));
        CPU_SET(w->index % ncpus, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }
    
    // Lower nice value = higher priority
    if (setpriority(PRIO_PROCESS, 0, -10) != 0) {
        // Not fatal - just continue
    }
    
    if (m_logLevel >= 2)
        std::cout << "[Worker " << w->index << "] started" << std::endl;

    // Stack buffers — ZERO heap alloc in hot path
    uint8_t blob[40];     // header(32) + nonce LE(8)  — matches coreminer
    uint8_t seed[64];     // SHA3-512(blob) → 64 bytes
    uint8_t hashout[32];   // RandomY → 32 bytes

    constexpr int BLOCKSIZE = 32;
    uint64_t localHashes = 0;
    uint64_t lastPrintHashes = 0;
    int idleSpins = 0;
    
    // Immediate test: hash even without job (warmup)
    uint8_t dummy_in[40] = {0};
    uint8_t dummy_seed[64];
    uint8_t dummy_out[32];
    sha3_512(dummy_in, 40, dummy_seed);
    randomx_calculate_hash(w->vm, dummy_seed, 64, dummy_out);
    w->totalHashes = 1;
    if (m_logLevel >= 2)
        std::cout << "[Worker " << w->index << "] warmup hash OK" << std::endl;

    while (m_running) {
        // --- Get current job (atomic pointer) ---
        Job* job = m_jobp.load(std::memory_order_acquire);
        if (!job || job->header.empty()) {
            idleSpins++;
            std::this_thread::sleep_for(std::chrono::milliseconds(
                idleSpins > 10 ? 100 : 10));
            continue;
        }
        idleSpins = 0;

        // Pre-decoded header bytes (already in job.header from onNewJob)
        const uint8_t* headerPtr = job->header.data();
        uint64_t targetInt = job->targetInt;
        // Hex header dihitung lokal dari job (HINDARI data race: thread stratum
        // menulis m_currentHeaderHex di bawah lock sementara worker membaca tanpa lock)
        std::string headerHex = bytes_to_hex(headerPtr, 32);

        // Grab a batch of nonces (atomic — optimasi 4)
        uint64_t nonceBase = m_globalNonce.fetch_add(BLOCKSIZE, 
                                                      std::memory_order_relaxed);

        for (int i = 0; i < BLOCKSIZE && m_running; i++) {
            uint64_t nonce = nonceBase + i;

            // Build blob: header(32) + nonce LE(8) = 40 bytes
            memcpy(blob, headerPtr, 32);
            memcpy(blob + 32, &nonce, 8);

            // SHA3-512(blob, 40) → seed(64), then RandomY
            sha3_512(blob, 40, seed);
            randomx_calculate_hash(w->vm, seed, 64, hashout);

            localHashes++;
            w->totalHashes++;

            // Target check — first 8 bytes of hash as big-endian uint64
            uint64_t hashVal = 0;
            for (int b = 0; b < 8; b++)
                hashVal = (hashVal << 8) | hashout[b];

            if (hashVal < targetInt) {
                // SHARE FOUND! Submit via Stratum
                uint64_t nonceBE = htobe64(nonce);
                std::string nonceHex = "0x" + bytes_to_hex(
                    reinterpret_cast<const uint8_t*>(&nonceBE), 8);
                std::string mixHex = "0x" + bytes_to_hex(hashout, 32);
                
                if (m_logLevel >= 2)
                    std::cout << "\n[Worker " << w->index << "] ⭐ Share found!"
                              << " nonce=" << nonceHex
                              << " hash=" << bytes_to_hex(hashout, 12) << "..."
                              << std::endl;

                if (m_client && m_client->isConnected()) {
                    m_client->submitShare(headerHex, nonceHex, mixHex);
                }
            }
        }

        // Periodic stats
        if (localHashes >= 100000) {
            auto now = std::chrono::steady_clock::now();
            auto sec = std::chrono::duration_cast<std::chrono::duration<double>>(
                now - m_statsStart).count();
            if (sec > 0) {
                double rate = static_cast<double>(w->totalHashes) / sec;
                std::cout << "[Worker " << w->index << "] " << rate << " H/s"
                          << " (total " << w->totalHashes << ")" << std::endl;
            }
            localHashes = 0;
        }
    }
}

void Miner::printStats() const {
    auto now = std::chrono::steady_clock::now();
    auto sec = std::chrono::duration_cast<std::chrono::duration<double>>(
        now - m_statsStart).count();
    if (sec < 1) sec = 1;

    uint64_t total = 0;
    for (auto& w : m_workers) total += w->totalHashes;

    double rate = static_cast<double>(total) / sec;

    std::cout << "\n=== HASHRATE ==="
              << "\n  Total:  " << rate << " H/s"
              << "\n  Shares: " << m_acceptedShares << " accepted / "
              << m_rejectedShares << " rejected"
              << "\n  Workers: " << m_workers.size();
    for (auto& w : m_workers) {
        double wr = static_cast<double>(w->totalHashes) / sec;
        std::cout << "\n    W" << w->index << ": " << wr << " H/s (" 
                  << w->totalHashes << " total)";
    }
    std::cout << "\n================" << std::endl;
}
