#include "Config.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <unistd.h>
#include <pwd.h>

// ============================================================
// Helpers
// ============================================================
static std::string expandHome(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("HOME");
    if (!home) home = getpwuid(getuid())->pw_dir;
    return std::string(home) + path.substr(1);
}

bool parseBool(const std::string& val, bool def) {
    std::string v;
    v.reserve(val.size());
    for (char c : val) v += (char)tolower((unsigned char)c);
    if (v == "true" || v == "1" || v == "yes" || v == "on" || v == "tls") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    return def;
}

std::string maskWallet(const std::string& w) {
    if (w.size() <= 8) return std::string(w.size(), '*');
    return w.substr(0, 4) + "****" + w.substr(w.size() - 4);
}

// ------------------------------------------------------------
// Thread auto-detect yang aware cgroup (k8s pod / container)
// Default lama = sysconf(_SC_NPROCESSORS_ONLN) = jumlah core NODE,
// yang di pod dengan limits.cpu lebih kecil menyebabkan oversubscribe.
// cgroup v2: /sys/fs/cgroup/cpu.max          ("300000 100000" -> 3)
// cgroup v1: /sys/fs/cgroup/cpu/cpu.cfs_quota_us + cpu.cfs_period_us
// Fallback:  nproc node.
// ------------------------------------------------------------
static int readCgroupCpuLimit() {
    {
        std::ifstream f("/sys/fs/cgroup/cpu.max");
        std::string quota, period;
        if (f >> quota >> period) {
            if (quota != "max") {
                try {
                    long long q = std::stoll(quota), p = std::stoll(period);
                    if (q > 0 && p > 0) {
                        long long v = q / p;
                        return v < 1 ? 1 : (int)v;
                    }
                } catch (...) { /* malformed -> ignore */ }
            }
        }
    }
    {
        std::ifstream fq("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
        std::ifstream fp("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
        long long q = -1, p = 100000;
        if (fq >> q) fp >> p;
        if (q > 0 && p > 0) {
            long long v = q / p;
            return v < 1 ? 1 : (int)v;
        }
    }
    return -1;
}

int detectThreadCount() {
    int cgroup = readCgroupCpuLimit();
    if (cgroup > 0) return cgroup;
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
}

// ============================================================
// pool.cfg
// ============================================================
MinerConfig Config::loadFile(const std::string& path) {
    MinerConfig cfg;
    std::string expanded = expandHome(path);
    std::ifstream file(expanded);
    if (!file.is_open()) {
        return cfg;
    }

    std::cout << "[Config] Loading: " << expanded << std::endl;
    PoolConfig pool;
    std::string line;

    while (std::getline(file, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);

        auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        auto end = line.find_last_not_of(" \t\r");
        line = line.substr(start, end - start + 1);

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);

        if (key == "wallet") {
            if (cfg.pools.empty()) {
                pool.wallet = val;
            } else {
                for (auto& p : cfg.pools) p.wallet = val;
            }
        } else if (key == "worker") {
            pool.worker = val;
        } else if (key.compare(0, 6, "server") == 0 && key.back() == ']') {
            if (!pool.host.empty()) {
                cfg.pools.push_back(pool);
                pool = PoolConfig();
                if (!cfg.pools.empty()) {
                    pool.wallet = cfg.pools[0].wallet;
                    pool.worker = cfg.pools[0].worker;
                    pool.tls = cfg.pools[0].tls;
                }
            }
            // Format: host[:port[:tls]]
            auto c1 = val.find(':');
            if (c1 != std::string::npos) {
                auto c2 = val.find(':', c1 + 1);
                if (c2 != std::string::npos) {
                    pool.host = val.substr(0, c1);
                    try { pool.port = (uint16_t)std::stoul(val.substr(c1 + 1, c2 - c1 - 1)); }
                    catch (...) { pool.port = 8008; }
                    pool.tls = parseBool(val.substr(c2 + 1), false);
                } else {
                    pool.host = val.substr(0, c1);
                    try { pool.port = (uint16_t)std::stoul(val.substr(c1 + 1)); }
                    catch (...) { pool.port = 8008; }
                }
            } else {
                pool.host = val;
            }
        } else if (key.compare(0, 4, "port") == 0 && key.back() == ']') {
            try { pool.port = (uint16_t)std::stoul(val); }
            catch (...) { /* keep default */ }
        } else if (key == "tls") {
            pool.tls = parseBool(val, false);
            if (!cfg.pools.empty())
                for (auto& p : cfg.pools) p.tls = pool.tls;
        } else if (key == "threads") {
            try { cfg.threads = std::stoi(val); }
            catch (...) { /* keep default */ }
        } else if (key == "no_jit") {
            cfg.useJIT = !parseBool(val, false);
        } else if (key == "light") {
            cfg.fullMem = !parseBool(val, true);
        } else if (key == "no_aes") {
            cfg.hardAES = !parseBool(val, false);
        } else if (key == "large_pages") {
            cfg.largePages = parseBool(val, true);
        } else if (key == "log_level") {
            try {
                int lv = std::stoi(val);
                cfg.logLevel = std::max(0, std::min(2, lv));
            } catch (...) { /* keep default */ }
        }
    }

    if (!pool.host.empty()) {
        cfg.pools.push_back(pool);
    }

    return cfg;
}

// ============================================================
// Env vars (Docker/Akash friendly)
// WALLET + POOL lengkap => cfg.pools terisi, file tidak perlu dibaca.
// ============================================================
bool Config::applyEnv(MinerConfig& cfg) {
    const char* e = nullptr;
    (void)e;
    const char* envWallet = getenv("WALLET");
    const char* envPool = getenv("POOL");
    const char* envWorker = getenv("WORKER");
    const char* envThreads = getenv("THREADS");
    const char* envFullMem = getenv("FULL_MEM");
    const char* envLargePages = getenv("LARGE_PAGES");
    const char* envLog = getenv("LOG_LEVEL");
    const char* envLogShares = getenv("LOG_SHARES");  // legacy
    const char* envTls = getenv("TLS");

    // FULL_MEM default true; "0"/"false"/"no" => light
    if (envFullMem && envFullMem[0] != '\0')
        cfg.fullMem = parseBool(envFullMem, true);

    // LARGE_PAGES default true; "0"/"false"/"no" => disable
    // (container tanpa hugepages/mlock wajib disable, selain itu cache alloc gagal)
    if (envLargePages && envLargePages[0] != '\0')
        cfg.largePages = parseBool(envLargePages, true);

    // Log level: 0=QUIET 1=SHARE (default) 2=FULL
    // Precedence: LOG_LEVEL > LOG_SHARES (legacy: 1 berarti "cetak share found")
    if (envLog && envLog[0] != '\0') {
        try {
            int lv = std::stoi(envLog);
            cfg.logLevel = std::max(0, std::min(2, lv));
        } catch (...) { /* keep default */ }
    } else if (envLogShares && envLogShares[0] != '\0') {
        cfg.logLevel = parseBool(envLogShares, false) ? 2 : 0;
    }

    if (envWallet && envWallet[0] != '\0' && envPool && envPool[0] != '\0') {
        PoolConfig p;
        std::string poolStr = envPool;
        // Format: host:port[:tls]
        auto c1 = poolStr.find(':');
        if (c1 != std::string::npos) {
            auto c2 = poolStr.find(':', c1 + 1);
            if (c2 != std::string::npos) {
                p.host = poolStr.substr(0, c1);
                try { p.port = (uint16_t)std::stoul(poolStr.substr(c1 + 1, c2 - c1 - 1)); }
                catch (...) { p.port = 8008; }
                p.tls = parseBool(poolStr.substr(c2 + 1), false);
            } else {
                p.host = poolStr.substr(0, c1);
                try { p.port = (uint16_t)std::stoul(poolStr.substr(c1 + 1)); }
                catch (...) { p.port = 8008; }
            }
        } else {
            p.host = poolStr;
            p.port = 8008;
        }
        p.wallet = envWallet;
        p.worker = (envWorker && envWorker[0] != '\0') ? envWorker : "worker";
        if (envTls && envTls[0] != '\0')
            p.tls = parseBool(envTls, p.tls);
        cfg.pools.push_back(p);
        if (envThreads && envThreads[0] != '\0') {
            try { cfg.threads = (int)std::stoul(envThreads); }
            catch (...) { /* keep auto */ }
        }
        std::cout << "[Config] Using env: POOL=" << p.host << ":" << p.port
                  << " WALLET=" << maskWallet(p.wallet)
                  << " worker=" << p.worker << std::endl;
        return true;  // env lengkap — file tidak perlu dibaca
    }
    return false;
}

// ============================================================
// CLI flags
// ============================================================
int Config::applyCli(int argc, char* argv[], MinerConfig& cfg) {
    // Wallet/worker yang di-set -u SEBELUM pool ditambahkan (-o) perlu
    // disimpan dulu agar pool baru mewarisi. Precedence: -u terakhir menang.
    std::string pendingWallet, pendingWorker;
    bool hasPending = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-o" && i + 1 < argc) {
            // pool[:port[:tls]]
            PoolConfig p;
            std::string s = argv[++i];
            auto c1 = s.find(':');
            if (c1 != std::string::npos) {
                auto c2 = s.find(':', c1 + 1);
                if (c2 != std::string::npos) {
                    p.host = s.substr(0, c1);
                    try { p.port = (uint16_t)std::stoul(s.substr(c1 + 1, c2 - c1 - 1)); }
                    catch (...) { p.port = 8008; }
                    p.tls = parseBool(s.substr(c2 + 1), false);
                } else {
                    p.host = s.substr(0, c1);
                    try { p.port = (uint16_t)std::stoul(s.substr(c1 + 1)); }
                    catch (...) { p.port = 8008; }
                }
            } else {
                p.host = s;
                p.port = 8008;
            }
            if (hasPending) {
                p.wallet = pendingWallet;
                if (!pendingWorker.empty()) p.worker = pendingWorker;
            }
            cfg.pools.push_back(p);

        } else if (arg == "-u" && i + 1 < argc) {
            std::string s = argv[++i];
            auto dot = s.find('.');
            if (dot != std::string::npos) {
                pendingWallet = s.substr(0, dot);
                pendingWorker = s.substr(dot + 1);
            } else {
                pendingWallet = s;
                pendingWorker.clear();
            }
            hasPending = true;
            for (auto& p : cfg.pools) {
                p.wallet = pendingWallet;
                if (!pendingWorker.empty()) p.worker = pendingWorker;
            }

        } else if (arg == "-p" && i + 1 < argc) {
            for (auto& p : cfg.pools) p.password = argv[++i];

        } else if (arg == "-t" && i + 1 < argc) {
            try { cfg.threads = std::stoi(argv[++i]); }
            catch (...) { /* keep auto */ }

        } else if (arg == "-c" && i + 1 < argc) {
            cfg.configFile = argv[++i];

        } else if (arg == "--light") {
            cfg.fullMem = false;

        } else if (arg == "--no-jit") {
            cfg.useJIT = false;

        } else if (arg == "--no-tls") {
            for (auto& p : cfg.pools) p.tls = false;

        } else if (arg == "--version") {
            std::cout << "miner-saya v1.1.0 (RandomY / Core Coin XCB)" << std::endl;
            exit(0);

        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: miner-saya [options]\n"
                      << "  -o host:port[:tls]   Pool address (TLS via suffix :tls / :true)\n"
                      << "  -u wallet[.worker]   Wallet address\n"
                      << "  -p password          Pool password\n"
                      << "  -t N                 Thread count (0/absen = auto, cgroup-aware)\n"
                      << "  -c path              Config file (default: pool.cfg, /miner/pool.cfg, ~/xcb/pool.cfg)\n"
                      << "  --light              Light dataset (cache 256MB, bukan full 2.6GB)\n"
                      << "  --no-jit             Disable JIT compiler\n"
                      << "  --no-tls             Force plain TCP (override host:port:tls)\n"
                      << "  --version            Show version\n"
                      << "  -h, --help           Show this help\n"
                      << "Env: WALLET POOL WORKER THREADS FULL_MEM LARGE_PAGES TLS LOG_LEVEL\n";
            return -1;  // caller exit(0)
        }
    }
    return 0;
}

// ============================================================
// Orkestrasi: env > file > CLI-override, lalu validasi
// Precedence wallet/pool: env > file. CLI -u/-o/-t/-c override semuanya.
// ============================================================
MinerConfig Config::parse(int argc, char* argv[]) {
    MinerConfig cfg;

    // Scan -c dulu (butuh tahu file mana sebelum load)
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-c" && i + 1 < argc) {
            cfg.configFile = argv[i + 1];
            break;
        }
    }

    // 1. Env vars
    bool envComplete = applyEnv(cfg);

    // 2. Config file (hanya kalau env belum lengkap)
    if (!envComplete) {
        std::vector<std::string> candidates;
        if (!cfg.configFile.empty()) {
            candidates.push_back(cfg.configFile);
        } else {
            candidates.push_back("pool.cfg");
            candidates.push_back("/miner/pool.cfg");
            candidates.push_back("~/xcb/pool.cfg");
        }
        for (auto& c : candidates) {
            MinerConfig f = loadFile(c);
            if (f.pools.empty()) continue;
            cfg.pools = std::move(f.pools);
            // env yang sudah di-set menang (detected karena default-nya true/0)
            if (cfg.threads <= 0) cfg.threads = f.threads;
            if (cfg.fullMem) cfg.fullMem = f.fullMem;
            if (cfg.useJIT) cfg.useJIT = f.useJIT;
            if (cfg.largePages) cfg.largePages = f.largePages;
            break;
        }
    }

    // 3. CLI args (override file/env)
    if (applyCli(argc, argv, cfg) == -1) exit(0);

    // 4. Validasi
    if (cfg.pools.empty()) {
        std::cerr << "[Config] No pool configured! Use -o or set in pool.cfg / POOL env" << std::endl;
        exit(1);
    }
    for (auto& p : cfg.pools) {
        if (p.wallet.empty()) {
            std::cerr << "[Config] No wallet configured! Use -u or set in pool.cfg / WALLET env" << std::endl;
            exit(1);
        }
    }

    // 5. Default threads: cgroup-aware (k8s pod limit > nproc node)
    if (cfg.threads <= 0)
        cfg.threads = detectThreadCount();

    return cfg;
}