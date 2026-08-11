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
    int threads = 1;
    bool useJIT = true;
    bool fullMem = true;
    bool hardAES = true;
    bool largePages = true;
};

class Config {
public:
    static MinerConfig parse(int argc, char* argv[]);
    static MinerConfig loadFile(const std::string& path);
};
