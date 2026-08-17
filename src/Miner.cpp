#include "Miner.hpp"
#include <iostream>
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
    std::cout << "[Miner] Starting with " << cfg.threads << " threads" << std::endl;
    
    m_numThreads = cfg.threads;

    // Verbose share logging? (LOG_SHARES=0/false default = quiet)
    const char* envLog = getenv("LOG_SHARES");
    m_verboseShares = (envLog && envLog[0] != '\0'
        && std::string(envLog) != "0" && std::string(envLog) != "false"
        && std::string(envLog) != "no");
    
    // --- RandomY flags ---
    m_flags = randomx_get_flags();
    if (cfg.fullMem) {
        // Full mem: explicitly add FULL_MEM (randomx_get_flags may not set it)
        m_flags = static_cast<randomx_flags>(m_flags | RANDOMX_FLAG_FULL_MEM);
    } else {
        // Light mode: ensure FULL_MEM is removed
        m_flags = static_cast<randomx_flags>(m_flags & ~RANDOMX_FLAG_FULL_MEM);
    }
    if (cfg.useJIT)
        m_flags = static_cast<randomx_flags>(m_flags | RANDOMX_FLAG_JIT);
    if (cfg.hardAES)
        m_flags = static_cast<randomx_flags>(m_flags | RANDOMX_FLAG_HARD_AES);
    // Large pages: auto-detect. Try a probe allocation with LARGE_PAGES.
    // If the kernel/container provides no hugepages (or mlock is limited)
    // fall back to normal pages instead of failing.
    if (cfg.largePages) {
        randomx_cache* probe = randomx_alloc_cache(
            static_cast<randomx_flags>(m_flags | RANDOMX_FLAG_LARGE_PAGES));
        if (probe) {
            randomx_release_cache(probe);
            m_flags = static_cast<randomx_flags>(m_flags | RANDOMX_FLAG_LARGE_PAGES);
            std::cout << "[Miner] LARGE_PAGES available - enabled" << std::endl;
        } else {
            std::cout << "[Miner] LARGE_PAGES unavailable - falling back to normal pages"
                      << std::endl;
        }
    } else {
        std::cout << "[Miner] LARGE_PAGES disabled by config" << std::endl;
    }
    
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
        if (!m_cache && (m_flags & RANDOMX_FLAG_LARGE_PAGES)) {
            std::cerr << "[Miner] Cache alloc failed with LARGE_PAGES - retrying normal pages"
                      << std::endl;
            m_flags = static_cast<randomx_flags>(m_flags & ~RANDOMX_FLAG_LARGE_PAGES);
            m_cache = randomx_alloc_cache(m_flags);
        }
        if (!m_cache) { std::cerr << "[Miner] Cache alloc failed" << std::endl; return; }
        randomx_init_cache(m_cache, key, sizeof(key));

        m_dataset = randomx_alloc_dataset(m_flags);
        if (!m_dataset && (m_flags & RANDOMX_FLAG_LARGE_PAGES)) {
            std::cerr << "[Miner] Dataset alloc failed with LARGE_PAGES - retrying normal pages"
                      << std::endl;
            m_flags = static_cast<randomx_flags>(m_flags & ~RANDOMX_FLAG_LARGE_PAGES);
            randomx_release_cache(m_cache);
            m_cache = nullptr;
            m_cache = randomx_alloc_cache(m_flags);
            if (!m_cache) { std::cerr << "[Miner] Cache alloc failed" << std::endl; return; }
            randomx_init_cache(m_cache, key, sizeof(key));
            m_dataset = randomx_alloc_dataset(m_flags);
        }
        if (!m_dataset) { std::cerr << "[Miner] Dataset alloc failed" << std::endl; return; }
        if (!m_dataset) { std::cerr << "[Miner] Dataset alloc failed" << std::endl; return; }

        uint32_t datasetItems = randomx_dataset_item_count();
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
        if (!m_cache && (m_flags & RANDOMX_FLAG_LARGE_PAGES)) {
            std::cerr << "[Miner] Cache alloc failed with LARGE_PAGES - retrying normal pages"
                      << std::endl;
            m_flags = static_cast<randomx_flags>(m_flags & ~RANDOMX_FLAG_LARGE_PAGES);
            m_cache = randomx_alloc_cache(m_flags);
        }
        if (!m_cache) { std::cerr << "[Miner] Cache alloc failed" << std::endl; return; }
        randomx_init_cache(m_cache, key, sizeof(key));
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
        std::cout << "[Miner] Worker " << i << " VM created" << std::endl;
        w->thread = std::thread(&Miner::workerLoop, this, w.get());
        m_workers.push_back(std::move(w));
    }
    
    m_running = true;

    // --- Stratum client ---
    auto& pool = cfg.pools[0];
    m_client = std::make_unique<StratumClient>(
        pool.host, pool.port, pool.wallet, pool.worker, pool.password
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

static int parse_target_bytes(const std::string& hex, uint8_t out[32]) {
    memset(out, 0, 32);
    std::string t = hex;
    if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    int hexLen = (int)t.size();
    int bytes = 0;
    for (int i = 0; i < 32; i++) {
        int hIdx = hexLen - 2 - 2 * i;  // nibbles from the right (big-endian)
        if (hIdx < 0) break;
        unsigned int byte = 0;
        for (int k = 0; k < 2; k++) {
            char c = t[hIdx + k];
            unsigned int v = (c >= '0' && c <= '9') ? c - '0'
                           : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                           : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
            byte = (byte << 4) | v;
        }
        out[31 - i] = (uint8_t)byte;
        bytes++;
    }
    return bytes;
}

// 8-byte MSB of the target as delivered (left-aligned in the hex string),
// right-padded with zeros. Used for the ethproxy 64-bit share check.
static void parse_target_msb8(const std::string& hex, uint8_t out[8]) {
    memset(out, 0, 8);
    std::string t = hex;
    if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    for (int i = 0; i < 8; i++) {
        int hIdx = 2 * i;
        if (hIdx + 1 >= (int)t.size()) break;
        auto hexval = [](char c) -> unsigned int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        out[i] = (uint8_t)((hexval(t[hIdx]) << 4) | hexval(t[hIdx + 1]));
    }
}

void Miner::onNewJob(const Job& job) {
    std::lock_guard<std::mutex> lock(m_jobMutex);
    m_jobStorage = job;
    m_currentJobId = job.jobId;
    
    // Recompute header hex from parsed bytes (for submission)
    m_currentHeaderHex = bytes_to_hex(job.header.data(), (int)job.header.size());
    m_targetBytesUsed = parse_target_bytes(job.targetHex, m_targetBytes);
    parse_target_msb8(job.targetHex, m_targetMsb8);
    // Full 256-bit target (stratum style) only when the pool sends a
    // complete 32-byte target. Shorter/odd-length targets (ethproxy,
    // e.g. 60-hex as sent by catchthatrabbit) are checked as 64-bit MSB,
    // which is exactly how the pool validates shares.
    m_useFullTarget = (job.targetHex.length() >= 63);
    
    m_globalNonce.store(0, std::memory_order_relaxed);
    
    std::cout << "[Miner] Job " << job.jobId << " — target=" 
              << std::hex << job.targetInt << std::dec << std::endl;
}

void Miner::onShareResult(bool accepted, const std::string& msg) {
    if (accepted) {
        m_acceptedShares++;
        if (m_verboseShares || m_acceptedShares % 50 == 0)
            std::cout << "[Miner] ✅ Share ACCEPTED (#" << m_acceptedShares << ")" << std::endl;
    } else {
        m_rejectedShares++;
        if (m_rejectedShares <= 5 || m_rejectedShares % 10 == 0)
            std::cout << "[Miner] ❌ Share rejected: " << msg << std::endl;
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
    std::cout << "[Worker " << w->index << "] warmup hash OK" << std::endl;

    while (m_running) {
        // --- Snapshot current job under the job lock ---
        // (fix: no data race with onNewJob, and every share is submitted
        //  with the header of the job it was actually mined against)
        Job snapshot;
        std::string snapHeaderHex;
        uint8_t snapTarget[32];
        int snapTargetUsed;
        uint8_t snapMsb8[8];
        bool snapUseFull;
        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            snapshot = m_jobStorage;
            snapHeaderHex = bytes_to_hex(snapshot.header.data(), (int)snapshot.header.size());
            memcpy(snapTarget, m_targetBytes, 32);
            snapTargetUsed = m_targetBytesUsed;
            memcpy(snapMsb8, m_targetMsb8, 8);
            snapUseFull = m_useFullTarget;
        }
        if (snapshot.header.empty()) {
            idleSpins++;
            std::this_thread::sleep_for(std::chrono::milliseconds(
                idleSpins > 10 ? 100 : 10));
            continue;
        }
        idleSpins = 0;

        // Pre-decoded header bytes (already in snapshot.header from onNewJob)
        const uint8_t* headerPtr = snapshot.header.data();

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

            // Target check — two modes:
            //  - ethproxy (short target): 64-bit compare of hash MSB vs
            //    target MSB, exactly how the pool validates shares
            //  - stratum (full 256-bit target): big-endian hash <= target
            bool meetsTarget;
            if (snapUseFull) {
                meetsTarget = true;
                for (int b = 0; b < snapTargetUsed; b++) {
                    if (hashout[b] < snapTarget[b]) break;
                    if (hashout[b] > snapTarget[b]) { meetsTarget = false; break; }
                }
            } else {
                uint64_t hv = 0, tv = 0;
                for (int b = 0; b < 8; b++) {
                    hv = (hv << 8) | hashout[b];
                    tv = (tv << 8) | snapMsb8[b];
                }
                meetsTarget = (hv < tv);
            }

            if (meetsTarget) {
                // SHARE FOUND! Submit via Stratum
                uint64_t nonceBE = htobe64(nonce);
                std::string nonceHex = "0x" + bytes_to_hex(
                    reinterpret_cast<const uint8_t*>(&nonceBE), 8);
                std::string mixHex = "0x" + bytes_to_hex(hashout, 32);
                
                if (m_verboseShares)
                    std::cout << "\n[Worker " << w->index << "] ⭐ Share found!"
                              << " nonce=" << nonceHex
                              << " hash=" << bytes_to_hex(hashout, 12) << "..."
                              << std::endl;

                if (m_client && m_client->isConnected()) {
                    m_client->submitShare(snapHeaderHex, nonceHex, mixHex);
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
