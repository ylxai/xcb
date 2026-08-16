#include "Config.hpp"
#include "Miner.hpp"
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>

static std::atomic<bool> g_running{true};
static Miner* g_miner = nullptr;

extern "C" void signal_handler(int sig) {
    (void)sig;
    if (!g_running.exchange(false)) return;
    std::cout << "\nShutting down..." << std::endl;
    if (g_miner) g_miner->stop();
}

int main(int argc, char* argv[]) {
    // Signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "=== miner-saya v1.1 ===" << std::endl;

    // Parse config
    auto config = Config::parse(argc, argv);

    // Display config — wallet selalu di-mask (jangan pernah print full wallet)
    std::cout << "[Config] Pools: " << config.pools.size() << std::endl;
    for (size_t i = 0; i < config.pools.size(); i++) {
        auto& p = config.pools[i];
        std::cout << "  Pool " << (i+1) << ": " << p.host << ":" << p.port
                  << (p.tls ? " (tls)" : " (tcp)")
                  << " wallet=" << maskWallet(p.wallet)
                  << " worker=" << p.worker << std::endl;
    }
    std::cout << "[Config] Threads: " << config.threads
              << " logLevel=" << config.logLevel << std::endl;

    // Start miner
    Miner miner;
    g_miner = &miner;
    miner.start(config);

    // Wait
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    g_miner = nullptr;
    return 0;
}