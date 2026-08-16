// Unit tests untuk Config (parseBool, maskWallet, detectThreadCount,
// loadFile, applyEnv, applyCli, parse + precedence).
// Framework: assert sederhana, main() return 0 = semua lulus.
#include "Config.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

// argv builder: string literal -> char* (aman: argv tidak dimodifikasi)
static std::vector<char*> mkArgs(std::initializer_list<const char*> list) {
    std::vector<char*> v;
    for (auto s : list) v.push_back(const_cast<char*>(s));
    return v;
}

// ------------------------------------------------------------
// parseBool
// ------------------------------------------------------------
static void test_parseBool() {
    CHECK(parseBool("true", false) == true, "parseBool true");
    CHECK(parseBool("TRUE", false) == true, "parseBool TRUE");
    CHECK(parseBool("1", false) == true, "parseBool 1");
    CHECK(parseBool("yes", false) == true, "parseBool yes");
    CHECK(parseBool("on", false) == true, "parseBool on");
    CHECK(parseBool("false", true) == false, "parseBool false");
    CHECK(parseBool("0", true) == false, "parseBool 0");
    CHECK(parseBool("no", true) == false, "parseBool no");
    CHECK(parseBool("off", true) == false, "parseBool off");
    CHECK(parseBool("", true) == true, "parseBool empty -> default");
    CHECK(parseBool("garbage", false) == false, "parseBool garbage -> default");
    CHECK(parseBool("garbage", true) == true, "parseBool garbage -> default true");
}

// ------------------------------------------------------------
// maskWallet
// ------------------------------------------------------------
static void test_maskWallet() {
    std::string w = "cb23d6d8557e776f5ff9ab6a7fb7f59a3d385245fa7a";
    std::string m = maskWallet(w);
    CHECK(m == "cb23****fa7a", "maskWallet exact format");
    CHECK(maskWallet("abc") == "***", "maskWallet short");
    CHECK(maskWallet("").empty(), "maskWallet empty");
}

// ------------------------------------------------------------
// detectThreadCount
// ------------------------------------------------------------
static void test_detectThreadCount() {
    int t = detectThreadCount();
    CHECK(t >= 1, "detectThreadCount >= 1");
}

// ------------------------------------------------------------
// loadFile
// ------------------------------------------------------------
static std::string writeTempCfg(const std::string& content) {
    for (int i = 0; i < 100; i++) {
        std::string p = "/tmp/miner-test-cfg-" + std::to_string(getpid()) + "-" + std::to_string(i);
        int fd = open(p.c_str(), O_CREAT | O_WRONLY | O_EXCL, 0644);
        if (fd < 0) continue;
        std::ofstream f(p);
        f << content;
        f.close();
        return p;
    }
    return "";
}

static void test_loadFile() {
    std::string content =
        "wallet=cb23d6d8557e776f5ff9ab6a7fb7f59a3d385245fa7a\n"
        "worker=testworker\n"
        "server[1]=sg.pool.example:8008:tls\n"
        "server[2]=hk.pool.example\n"
        "port[2]=9009\n"
        "threads=3\n"
        "light=true\n"
        "no_jit=1\n"
        "log_level=2\n";
    std::string path = writeTempCfg(content);
    CHECK(!path.empty(), "temp file created");
    if (path.empty()) return;

    MinerConfig cfg = Config::loadFile(path);
    CHECK(cfg.pools.size() == 2, "loadFile 2 pools");
    if (cfg.pools.size() == 2) {
        CHECK(cfg.pools[0].host == "sg.pool.example", "pool1 host");
        CHECK(cfg.pools[0].port == 8008, "pool1 port");
        CHECK(cfg.pools[0].tls == true, "pool1 tls from host:port:tls");
        CHECK(cfg.pools[0].wallet == "cb23d6d8557e776f5ff9ab6a7fb7f59a3d385245fa7a", "pool1 wallet");
        CHECK(cfg.pools[0].worker == "testworker", "pool1 worker");
        CHECK(cfg.pools[1].host == "hk.pool.example", "pool2 host");
        CHECK(cfg.pools[1].port == 9009, "pool2 port from port[2]");
        CHECK(cfg.pools[1].tls == true, "pool2 tls inherited");
        CHECK(cfg.pools[1].wallet == cfg.pools[0].wallet, "pool2 wallet inherited");
    }
    CHECK(cfg.threads == 3, "threads=3");
    CHECK(cfg.fullMem == false, "light=true -> fullMem=false");
    CHECK(cfg.useJIT == false, "no_jit=1 -> useJIT=false");
    CHECK(cfg.logLevel == 2, "log_level=2");

    unlink(path.c_str());

    // File tidak ada -> pools kosong, tidak crash
    MinerConfig empty = Config::loadFile("/nonexistent/nope.cfg");
    CHECK(empty.pools.empty(), "missing file -> empty pools");
}

// ------------------------------------------------------------
// env vars (via parse)
// ------------------------------------------------------------
static void test_env() {
    // Case 1: env lengkap -> menang, file diabaikan
    setenv("WALLET", "envwallet1234567890abcdef", 1);
    setenv("POOL", "envpool.example:8009:tls", 1);
    setenv("WORKER", "envworker", 1);
    setenv("THREADS", "2", 1);
    setenv("FULL_MEM", "0", 1);
    setenv("LOG_LEVEL", "0", 1);

    auto args = mkArgs({ "miner" });
    MinerConfig cfg = Config::parse((int)args.size(), args.data());
    CHECK(cfg.pools.size() == 1, "env: 1 pool");
    if (!cfg.pools.empty()) {
        CHECK(cfg.pools[0].host == "envpool.example", "env pool host");
        CHECK(cfg.pools[0].port == 8009, "env pool port");
        CHECK(cfg.pools[0].tls == true, "env pool tls");
        CHECK(cfg.pools[0].wallet == "envwallet1234567890abcdef", "env wallet");
        CHECK(cfg.pools[0].worker == "envworker", "env worker");
    }
    CHECK(cfg.threads == 2, "env THREADS=2");
    CHECK(cfg.fullMem == false, "env FULL_MEM=0");
    CHECK(cfg.logLevel == 0, "env LOG_LEVEL=0");

    unsetenv("WALLET"); unsetenv("POOL"); unsetenv("WORKER");
    unsetenv("THREADS"); unsetenv("FULL_MEM"); unsetenv("LOG_LEVEL");

    // Case 2: POOL tanpa port -> default 8008
    setenv("WALLET", "w1", 1);
    setenv("POOL", "onlyhost.example", 1);
    auto args2 = mkArgs({ "miner" });
    MinerConfig cfg2 = Config::parse((int)args2.size(), args2.data());
    CHECK(cfg2.pools.size() == 1 && cfg2.pools[0].port == 8008, "env no port -> 8008");
    unsetenv("WALLET"); unsetenv("POOL");

    // Case 3: LOG_LEVEL clamped
    setenv("WALLET", "w1", 1);
    setenv("POOL", "h:8008", 1);
    setenv("LOG_LEVEL", "99", 1);
    auto args3 = mkArgs({ "miner" });
    MinerConfig cfg3 = Config::parse((int)args3.size(), args3.data());
    CHECK(cfg3.logLevel == 2, "LOG_LEVEL=99 clamped to 2");
    unsetenv("LOG_LEVEL");
    setenv("LOG_LEVEL", "-5", 1);
    auto args3b = mkArgs({ "miner" });
    MinerConfig cfg3b = Config::parse((int)args3b.size(), args3b.data());
    CHECK(cfg3b.logLevel == 0, "LOG_LEVEL=-5 clamped to 0");
    unsetenv("LOG_LEVEL");
    unsetenv("WALLET"); unsetenv("POOL");

    // Case 4: LOG_SHARES legacy -> 2
    setenv("WALLET", "w1", 1);
    setenv("POOL", "h:8008", 1);
    setenv("LOG_SHARES", "1", 1);
    auto args4 = mkArgs({ "miner" });
    MinerConfig cfg4 = Config::parse((int)args4.size(), args4.data());
    CHECK(cfg4.logLevel == 2, "legacy LOG_SHARES=1 -> level 2");
    unsetenv("LOG_SHARES");
    unsetenv("WALLET"); unsetenv("POOL");
}

// ------------------------------------------------------------
// CLI overrides
// ------------------------------------------------------------
static void test_cli() {
    // File via -c, wallet via -u, threads via -t, --light
    std::string content =
        "wallet=filewallet\n"
        "server[1]=filepool.example\n"
        "port[1]=7007\n";
    std::string path = writeTempCfg(content);
    CHECK(!path.empty(), "cli temp file created");
    if (path.empty()) return;

    auto args = mkArgs({ "miner", "-c", path.c_str(), "-u", "cliwallet123456",
                         "-t", "5", "--light" });
    MinerConfig cfg = Config::parse((int)args.size(), args.data());
    CHECK(cfg.pools.size() == 1, "cli: 1 pool");
    if (!cfg.pools.empty()) {
        CHECK(cfg.pools[0].host == "filepool.example", "cli: file pool kept");
        CHECK(cfg.pools[0].port == 7007, "cli: file port kept");
        CHECK(cfg.pools[0].wallet == "cliwallet123456", "cli: -u overrides wallet");
    }
    CHECK(cfg.threads == 5, "cli: -t 5");
    CHECK(cfg.fullMem == false, "cli: --light");

    // -o adds pool
    auto args2 = mkArgs({ "miner", "-c", path.c_str(), "-u", "w",
                          "-o", "clipool.example:9999:tls" });
    MinerConfig cfg2 = Config::parse((int)args2.size(), args2.data());
    CHECK(cfg2.pools.size() == 2, "cli: -o adds pool");
    if (cfg2.pools.size() == 2) {
        CHECK(cfg2.pools[1].host == "clipool.example", "cli: -o host");
        CHECK(cfg2.pools[1].port == 9999, "cli: -o port");
        CHECK(cfg2.pools[1].tls == true, "cli: -o tls");
        CHECK(cfg2.pools[1].wallet == "w", "cli: -o pool gets -u wallet");
    }

    unlink(path.c_str());
}

// ------------------------------------------------------------
// parse() tanpa pool -> exit(1) (di-test via fork)
// ------------------------------------------------------------
static void test_parse_no_pool_exits() {
    unsetenv("WALLET"); unsetenv("POOL");
    pid_t pid = fork();
    if (pid == 0) {
        // child: /tmp supaya pool.cfg tidak ketemu
        chdir("/tmp");
        const char* a0 = "miner";
        char* a1 = const_cast<char*>(a0);
        char* argv[] = { a1, nullptr };
        Config::parse(1, argv);
        _exit(0);  // tidak boleh sampai sini
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "no pool -> exit(1)");
}

// ------------------------------------------------------------
// pool:port:tls parsing di -o
// ------------------------------------------------------------
static void test_pool_tls_parse() {
    auto a1 = mkArgs({ "miner", "-o", "h:8008:1", "-u", "w" });
    MinerConfig c1 = Config::parse((int)a1.size(), a1.data());
    CHECK(c1.pools.size() == 1 && c1.pools[0].tls, "tls=1 true");

    auto a2 = mkArgs({ "miner", "-o", "h:8008:0", "-u", "w" });
    MinerConfig c2 = Config::parse((int)a2.size(), a2.data());
    CHECK(c2.pools.size() == 1 && !c2.pools[0].tls, "tls=0 false");
}

// ------------------------------------------------------------
int main() {
    // Isolasi: HOME + cwd ke temp dir agar fallback ~/xcb/pool.cfg
    // dan pool.cfg di cwd (repo) tidak mengganggu test.
    std::string home = "/tmp/miner-test-home-" + std::to_string(getpid());
    mkdir(home.c_str(), 0755);
    setenv("HOME", home.c_str(), 1);
    if (chdir(home.c_str()) != 0) {
        std::printf("FATAL: chdir test home\n");
        return 2;
    }

    std::printf("=== miner-saya config tests ===\n");
    test_parseBool();
    test_maskWallet();
    test_detectThreadCount();
    test_loadFile();
    test_env();
    test_cli();
    test_parse_no_pool_exits();
    test_pool_tls_parse();
    std::printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}