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
};

struct MinerConfig {
    std::vector<PoolConfig> pools;
    int threads = 0;   // 0 = auto (CPU cores)
    bool useJIT = true;
    bool fullMem = false;       // set when FULL_MEM/light/--full explicitly
    bool fullMemAuto = true;    // true = pick full/light from available RAM
    bool hardAES = true;
    bool largePages = true;
    int submitIntervalMs = 0;     // 0 = unlimited (default); set to protect against strict pools
};

class Config {
public:
    static MinerConfig parse(int argc, char* argv[]);
    static MinerConfig loadFile(const std::string& path);
};
