#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct PoolConfig {
    std::string host;
    uint16_t port = 8008;
    std::string wallet;
    std::string worker = "worker";
    std::string password = "x";
    bool tls = false;   // host:port:tls / env TLS=1 / key tls=true di pool.cfg
};

struct MinerConfig {
    std::vector<PoolConfig> pools;
    int threads = 0;   // 0 = auto (cgroup-aware: cgroup limit > nproc)
    bool useJIT = true;
    bool fullMem = true;
    bool hardAES = true;
    bool largePages = true;   // otodisabled di Miner::start kalau hugepages tak tersedia
    int logLevel = 1;        // 0=QUIET (error+hashrate), 1=SHARE (default), 2=FULL (segala hal)
    std::string configFile;  // path eksplisit via -c (kosong = deteksi otomatis)
};

// --- Helper parsing bool (dipakai Config + di-test) ---
// "true"/"1"/"yes"/"on" => true; "false"/"0"/"no"/"off" => false; lain => default
bool parseBool(const std::string& val, bool def);
// Mask wallet: 4 digit pertama + *** + 4 digit terakhir
std::string maskWallet(const std::string& wallet);
// Thread count default: hormati cgroup CPU limit (k8s pod), fallback nproc
int detectThreadCount();

class Config {
public:
    static MinerConfig parse(int argc, char* argv[]);
    static MinerConfig loadFile(const std::string& path);

    // Load env vars ke cfg (WALLET, POOL, WORKER, THREADS, FULL_MEM, LARGE_PAGES,
    // LOG_LEVEL/LOG_SHARES, TLS). Return true kalau env wallet+pool lengkap.
    static bool applyEnv(MinerConfig& cfg);
    // Apply CLI flags ke cfg. Return -1 kalau --help diminta (caller exit(0)).
    static int applyCli(int argc, char* argv[], MinerConfig& cfg);
};