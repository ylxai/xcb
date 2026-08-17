#include "Config.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <pwd.h>

static std::string expandHome(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("HOME");
    if (!home) home = getpwuid(getuid())->pw_dir;
    return std::string(home) + path.substr(1);
}

MinerConfig Config::loadFile(const std::string& path) {
    MinerConfig cfg;
    std::string expanded = expandHome(path);
    std::ifstream file(expanded);
    if (!file.is_open()) {
        std::cerr << "[Config] Cannot open: " << expanded << std::endl;
        return cfg;
    }
    
    std::cout << "[Config] Loading: " << expanded << std::endl;
    PoolConfig pool;
    std::string line;
    
    while (std::getline(file, line)) {
        // Strip comments
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        
        // Trim whitespace
        auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        auto end = line.find_last_not_of(" \t\r");
        line = line.substr(start, end - start + 1);
        
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        
        // Remove surrounding quotes
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);
        
        if (key == "wallet") {
            // Pool config uses single wallet for all servers
            if (cfg.pools.empty()) {
                pool.wallet = val;
            } else {
                for (auto& p : cfg.pools) p.wallet = val;
            }
        } else if (key == "worker") {
            pool.worker = val;
        } else if (key.compare(0, 6, "server") == 0 && key.back() == ']') {
            // server[N]=host — push previous pool if exists
            if (!pool.host.empty()) {
                cfg.pools.push_back(pool);
                pool = PoolConfig();
                // Apply shared wallet/worker across all pools
                if (!cfg.pools.empty()) {
                    pool.wallet = cfg.pools[0].wallet;
                    pool.worker = cfg.pools[0].worker;
                }
            }
            pool.host = val;
        } else if (key.compare(0, 4, "port") == 0 && key.back() == ']') {
            pool.port = static_cast<uint16_t>(std::stoul(val));
        } else if (key == "threads") {
            cfg.threads = std::stoi(val);
        } else if (key == "no_jit") {
            cfg.useJIT = (val != "true" && val != "1" && val != "yes");
        } else if (key == "light") {
            cfg.fullMem = (val == "false" || val == "0" || val == "no");
        } else if (key == "no_aes") {
            cfg.hardAES = (val != "true" && val != "1" && val != "yes");
        }
    }
    
    // Push last pool
    if (!pool.host.empty()) {
        cfg.pools.push_back(pool);
    }
    
    return cfg;
}

MinerConfig Config::parse(int argc, char* argv[]) {
    MinerConfig cfg;
    
    // 1. Try environment variables first (Docker/Akash friendly)
    const char* envWallet = getenv("WALLET");
    const char* envPool = getenv("POOL");
    const char* envWorker = getenv("WORKER");
    const char* envThreads = getenv("THREADS");
    const char* envFullMem = getenv("FULL_MEM");
    const char* envLargePages = getenv("LARGE_PAGES");
    
    // FULL_MEM defaults to true; set "0" or "false" to disable
    if (envFullMem && envFullMem[0] != '\0') {
        std::string fm = envFullMem;
        cfg.fullMem = (fm != "0" && fm != "false" && fm != "no");
    }
    
    // LARGE_PAGES defaults to true; set "0" or "false" to disable
    // (containers without hugepages/mlock must disable or cache alloc fails)
    if (envLargePages && envLargePages[0] != '\0') {
        std::string lp = envLargePages;
        cfg.largePages = (lp != "0" && lp != "false" && lp != "no");
    }
    
    if (envWallet && envPool) {
        PoolConfig p;
        std::string poolStr = envPool;
        auto colon = poolStr.find(':');
        if (colon != std::string::npos) {
            p.host = poolStr.substr(0, colon);
            p.port = static_cast<uint16_t>(std::stoul(poolStr.substr(colon + 1)));
        } else {
            p.host = poolStr;
            p.port = 8008;
        }
        p.wallet = envWallet;
        p.worker = envWorker ? envWorker : "worker";
        cfg.pools.push_back(p);
        if (envThreads && envThreads[0] != '\0') cfg.threads = std::stoul(envThreads);
        // Default threads = CPU cores (same as file/CLI path)
        if (cfg.threads <= 0)
            cfg.threads = std::max(1, static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)));
        std::cout << "[Config] Using env: POOL=" << envPool << " WALLET=" << envWallet << std::endl;
        return cfg;  // Skip file parsing if env vars present
    }
    
    // 2. Try loading pool.cfg
    cfg = loadFile("pool.cfg");
    if (cfg.pools.empty()) cfg = loadFile("/miner/pool.cfg");
    if (cfg.pools.empty()) cfg = loadFile("~/xcb/pool.cfg");
    
    // Parse CLI args (override file)
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-o" && i + 1 < argc) {
            // pool:port format
            PoolConfig p;
            std::string s = argv[++i];
            auto colon = s.find(':');
            if (colon != std::string::npos) {
                p.host = s.substr(0, colon);
                p.port = static_cast<uint16_t>(std::stoul(s.substr(colon + 1)));
            } else {
                p.host = s;
                p.port = 8008;
            }
            cfg.pools.push_back(p);
            
        } else if (arg == "-u" && i + 1 < argc) {
            std::string s = argv[++i];
            auto dot = s.find('.');
            if (dot != std::string::npos) {
                // wallet.worker format
                for (auto& p : cfg.pools) {
                    p.wallet = s.substr(0, dot);
                    p.worker = s.substr(dot + 1);
                }
            } else {
                for (auto& p : cfg.pools) p.wallet = s;
            }
            
        } else if (arg == "-p" && i + 1 < argc) {
            for (auto& p : cfg.pools) p.password = argv[++i];
            
        } else if (arg == "-t" && i + 1 < argc) {
            cfg.threads = std::stoi(argv[++i]);
            
        } else if (arg == "--light") {
            cfg.fullMem = false;
            
        } else if (arg == "--no-jit") {
            cfg.useJIT = false;
            
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: miner-saya [options]\n"
                      << "  -o host:port     Pool address\n"
                      << "  -u wallet[.worker]  Wallet address\n"
                      << "  -p password      Pool password\n"
                      << "  -t N             Thread count\n"
                      << "  --light          Use light dataset (no full mem)\n"
                      << "  --no-jit         Disable JIT\n"
                      << "  Config file: pool.cfg\n";
            exit(0);
        }
    }
    
    // Apply wallet from file if CLI didn't override
    // (already done in loadFile)

    // Re-apply env overrides AFTER file+CLI (precedence: env > pool.cfg > CLI)
    const char* envFullMem2 = getenv("FULL_MEM");
    const char* envLargePages2 = getenv("LARGE_PAGES");
    const char* envThreads2 = getenv("THREADS");
    if (envFullMem2 && envFullMem2[0] != '\0') {
        std::string fm = envFullMem2;
        cfg.fullMem = (fm != "0" && fm != "false" && fm != "no");
    }
    if (envLargePages2 && envLargePages2[0] != '\0') {
        std::string lp = envLargePages2;
        cfg.largePages = (lp != "0" && lp != "false" && lp != "no");
    }
    if (envThreads2 && envThreads2[0] != '\0') cfg.threads = std::stoul(envThreads2);

    // Validate
    if (cfg.pools.empty()) {
            std::cerr << "[Config] No pool configured! Use -o or set in pool.cfg" << std::endl;
        exit(1);
    }
    for (auto& p : cfg.pools) {
        if (p.wallet.empty()) {
            std::cerr << "[Config] No wallet configured! Use -u or set in pool.cfg" << std::endl;
            exit(1);
        }
    }
    
    // Default threads = CPU cores
    if (cfg.threads <= 0)
        cfg.threads = std::max(1, static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)));
    
    return cfg;
}
