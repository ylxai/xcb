#include "StratumClient.hpp"
#include <iostream>
#include <cstring>
#include <sstream>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <atomic>
#include <algorithm>

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; }
        out += c;
    }
    return out;
}

static std::string json_find_str(const std::string& json, const std::string& key) {
    auto pos = json.find('"' + key + '"');
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + key.size() + 2);
    if (pos == std::string::npos) return "";
    size_t end = pos + 1;
    std::string val;
    while (end < json.size()) {
        if (json[end] == '\\') { val += json[end+1]; end += 2; continue; }
        if (json[end] == '"') break;
        val += json[end++];
    }
    return val;
}

static std::string json_array_elem(const std::string& json, const std::string& key, int idx) {
    auto pos = json.find('"' + key + '"');
    if (pos == std::string::npos) return "";
    pos = json.find('[', pos);
    if (pos == std::string::npos) return "";
    pos++;
    int depth = 0, cur = 0;
    while (pos < json.size() && cur <= idx) {
        if (json[pos] == '[') depth++;
        if (json[pos] == ']') { if (--depth < 0) break; }
        if (json[pos] == ',' && depth == 0) cur++;
        else if (cur == idx) {
            if (json[pos] == '"') {
                std::string val;
                pos++;
                while (pos < json.size()) {
                    if (json[pos] == '\\') { val += json[pos+1]; pos += 2; continue; }
                    if (json[pos] == '"') break;
                    val += json[pos++];
                }
                return val;
            } else if (json[pos] == '-' || json[pos] == '+' ||
                      (json[pos] >= '0' && json[pos] <= '9')) {
                std::string num;
                while (pos < json.size() && (json[pos] == '-' || json[pos] == '+' ||
                       json[pos] == '.' || (json[pos] >= '0' && json[pos] <= '9') ||
                       json[pos] == 'e' || json[pos] == 'E'))
                    num += json[pos++];
                return num;
            } else if (json.substr(pos, 4) == "true") return "true";
            else if (json.substr(pos, 5) == "false") return "false";
        }
        pos++;
    }
    return "";
}

static bool json_is_true(const std::string& json) {
    auto pos = json.find("\"result\"");
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + 7);
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    return json.substr(pos, 4) == "true";
}

static uint64_t json_id_val(const std::string& json) {
    auto pos = json.find("\"id\":");
    if (pos == std::string::npos) return 0;
    pos += 5;
    while (pos < json.size() && json[pos] == ' ') pos++;
    uint64_t id = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        id = id * 10 + (json[pos++] - '0');
    }
    return id;
}

static std::string strip_0x(const std::string& s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return s.substr(2);
    return s;
}

static std::string get_method(const std::string& line) {
    return json_find_str(line, "method");
}

// ------------------------------------------------------------
StratumClient::StratumClient(const std::string& host, uint16_t port,
                             const std::string& wallet, const std::string& worker,
                             const std::string& password)
    : m_host(host), m_port(port), m_wallet(wallet),
      m_worker(worker), m_password(password) {
    m_workerName = wallet + "." + worker;
    PoolInfo pi;
    pi.host = host; pi.port = port; pi.wallet = wallet;
    pi.worker = worker; pi.password = password;
    m_pools.push_back(pi);
    uint64_t h = 1469598103934665603ULL;
    for (char c : m_workerName) { h ^= static_cast<uint8_t>(c); h *= 1099511628211ULL; }
    m_workerIdHex = h;
}

void StratumClient::setPools(const std::vector<PoolInfo>& pools) {
    if (pools.empty()) return;
    m_pools = pools;
    m_poolIndex = 0;
    applyPool(0);
}

void StratumClient::applyPool(size_t idx) {
    if (idx >= m_pools.size()) return;
    m_poolIndex = idx;
    const PoolInfo& pi = m_pools[idx];
    m_host = pi.host;
    m_port = pi.port;
    m_wallet = pi.wallet;
    m_worker = pi.worker;
    m_password = pi.password;
    m_workerName = m_wallet + "." + m_worker;
}

void StratumClient::switchToNextPool() {
    if (m_pools.size() <= 1) return;
    size_t next = (m_poolIndex + 1) % m_pools.size();
    applyPool(next);
    m_connectFails = 0;
    std::cout << "[Stratum] Failover ke pool " << (m_poolIndex + 1)
              << "/" << m_pools.size() << ": " << m_host << ":" << m_port
              << std::endl;
}

StratumClient::~StratumClient() { disconnect(); }

bool StratumClient::isConnected() const { return m_sock >= 0 && m_running.load(); }

void StratumClient::sendLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (m_sock < 0) return;
    std::string payload = line + "\n";
    ::send(m_sock, payload.data(), payload.size(), MSG_NOSIGNAL);
}

std::string StratumClient::recvLine(double timeoutSec) {
    std::lock_guard<std::mutex> lock(m_recvMutex);
    while (true) {
        auto nl = m_recvBuf.find('\n');
        if (nl != std::string::npos) {
            std::string line = m_recvBuf.substr(0, nl);
            m_recvBuf.erase(0, nl + 1);
            return line;
        }
        struct pollfd pfd;
        pfd.fd = m_sock;
        pfd.events = POLLIN;
        int ms = static_cast<int>(timeoutSec * 1000);
        int ret = ::poll(&pfd, 1, ms);
        if (ret <= 0) return "";
        char buf[8192];
        ssize_t n = ::recv(m_sock, buf, sizeof(buf)-1, 0);
        if (n <= 0) { m_socketDead = true; return ""; }
        buf[n] = 0;
        m_recvBuf += std::string(buf, n);
    }
}

void StratumClient::disconnect() {
    if (m_running.exchange(false)) {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (m_sock >= 0) {
            ::shutdown(m_sock, SHUT_RDWR);
            ::close(m_sock);
            m_sock = -1;
        }
    }
    if (m_thread.joinable()) m_thread.join();
}

void StratumClient::connect() {
    if (m_running) return;

    std::string portStr = std::to_string(m_port);
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (::getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        std::cerr << "[Stratum] DNS fail: " << m_host << std::endl;
        if (res) ::freeaddrinfo(res);
        return;
    }
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { ::freeaddrinfo(res); return; }

    ::fcntl(fd, F_SETFL, O_NONBLOCK);
    ::connect(fd, res->ai_addr, res->ai_addrlen);
    ::freeaddrinfo(res);

    struct pollfd pfd = {fd, POLLOUT, 0};
    if (::poll(&pfd, 1, 10000) <= 0) { ::close(fd); return; }
    int soError = 0;
    socklen_t errLen = sizeof(soError);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &errLen);
    if (soError != 0) { ::close(fd); return; }
    ::fcntl(fd, F_SETFL, 0);

    m_sock = fd;
    m_recvBuf.clear();
    m_socketDead = false;
    m_running = true;
    std::cout << "[Stratum] Connected to " << m_host << ":" << m_port << std::endl;

    m_thread = std::thread(&StratumClient::run, this);
}

static std::string to_hex64(uint64_t v) {
    const char* d = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; i--) { out[i] = d[v & 0xf]; v >>= 4; }
    return out;
}

void StratumClient::run() {
    int reconnectDelay = 2;

    while (m_running) {
        m_msgId.store(1, std::memory_order_relaxed);
        m_socketDead = false;

        // Step 1: eth_submitLogin (blocking, before event loop)
        if (!doEthLogin()) {
            m_connectFails++;
            std::cerr << "[Stratum] Login failed, reconnecting in " << reconnectDelay << "s"
                      << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(reconnectDelay));
            reconnectDelay = std::min(reconnectDelay * 2, 30);
            if (m_connectFails >= 3 && m_pools.size() > 1) {
                switchToNextPool();
                reconnectDelay = 2;
            }
            reconnect();
            continue;
        }
        reconnectDelay = 2;
        m_connectFails = 0;
        std::cout << "[Stratum] Authorized as " << m_workerName << std::endl;

        // Step 2: single event loop — poll socket, route every line by id,
        // and issue eth_getWork on a timer. Never block on a single response,
        // never disconnect because of an unexpected line.
        auto lastPoll = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        auto lastHrReport = std::chrono::steady_clock::now() - std::chrono::seconds(61);
        bool needJob = true;

        while (m_running && !m_socketDead) {
            std::string line = recvLine(0.1);
            if (!line.empty()) {
                handleResponse(line);
                continue;
            }
            if (m_socketDead) break;

            auto now = std::chrono::steady_clock::now();
            auto hrElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastHrReport).count();
            if (m_hashrateProvider && hrElapsed >= 60) {
                double rate = m_hashrateProvider();
                if (rate > 0) {
                    uint64_t rateHs = static_cast<uint64_t>(rate);
                    std::string hrMsg = "{\"id\":" + std::to_string(m_msgId.fetch_add(1, std::memory_order_relaxed)) +
                                        ",\"method\":\"eth_submitHashrate\",\"params\":[\"0x" +
                                        to_hex64(rateHs) + "\",\"0x" + to_hex64(m_workerIdHex) + "\"]}";
                    sendLine(hrMsg);
                    std::cout << "[Stratum] eth_submitHashrate: " << rate << " H/s" << std::endl;
                }
                lastHrReport = now;
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastPoll).count();
            if (needJob || elapsed >= 3) {
                uint64_t id = m_msgId.fetch_add(1, std::memory_order_relaxed);
                m_pendingGetWorkId = id;
                sendLine("{\"id\":" + std::to_string(id) +
                         ",\"method\":\"eth_getWork\",\"params\":[]}");
                needJob = false;
                lastPoll = now;
            }
        }

        if (!m_running) break;
        std::cerr << "[Stratum] Disconnected, reconnecting in " << reconnectDelay << "s"
                  << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(reconnectDelay));
        reconnectDelay = std::min(reconnectDelay * 2, 30);
        reconnect();
    }

    m_running = false;
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (m_sock >= 0) { ::close(m_sock); m_sock = -1; }
    }
}

void StratumClient::reconnect() {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (m_sock >= 0) { ::close(m_sock); m_sock = -1; }

    std::string portStr = std::to_string(m_port);
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (::getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        if (res) ::freeaddrinfo(res);
        m_sock = -1;
        return;
    }
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { ::freeaddrinfo(res); return; }
    ::fcntl(fd, F_SETFL, O_NONBLOCK);
    ::connect(fd, res->ai_addr, res->ai_addrlen);
    ::freeaddrinfo(res);
    struct pollfd pfd = {fd, POLLOUT, 0};
    if (::poll(&pfd, 1, 10000) <= 0) { ::close(fd); return; }
    int soErr = 0;
    socklen_t sl = sizeof(soErr);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &sl);
    if (soErr != 0) { ::close(fd); return; }
    ::fcntl(fd, F_SETFL, 0);
    m_sock = fd;
    m_recvBuf.clear();
    m_socketDead = false;
    std::cout << "[Stratum] Reconnected" << std::endl;
}

bool StratumClient::doEthLogin() {
    uint64_t id = m_msgId.fetch_add(1, std::memory_order_relaxed);
    m_loginId = id;
    std::string msg = "{\"id\":" + std::to_string(id) +
                      ",\"method\":\"eth_submitLogin\",\"params\":[\"" +
                      json_escape(m_wallet) + "\"]}";
    sendLine(msg);

    std::string resp = recvLine(15.0);
    if (resp.empty()) return false;

    std::cout << "[Stratum] Login raw: " << resp.substr(0, 300) << std::endl;

    bool ok = json_is_true(resp);
    if (!ok) {
        ok = (resp.find("\"result\":\"0x") != std::string::npos);
    }
    if (!ok) {
        auto rPos = resp.find("\"result\"");
        if (rPos != std::string::npos) {
            auto colon = resp.find(':', rPos + 7);
            if (colon != std::string::npos) {
                while (colon < resp.size() && resp[colon] == ' ') colon++;
                ok = (colon < resp.size() && resp[colon] != 'n');
            }
        }
    }
    if (!ok) {
        std::string err = json_find_str(resp, "message");
        std::cerr << "[Stratum] Auth error: '" << err << "'" << std::endl;
    }
    return ok;
}

// Build a Job out of header/seed/target (hex, no 0x) and dispatch if changed.
void StratumClient::processJob(const std::string& jobId, const std::string& header,
                               const std::string& seed, const std::string& target) {
    if (header.empty() || target.empty()) {
        std::cerr << "[Stratum] Invalid job (empty header/target)" << std::endl;
        return;
    }
    if (header == m_currentHeader && seed == m_currentSeed && target == m_currentTarget) {
        return; // same job, skip
    }

    m_currentHeader = header;
    m_currentSeed = seed;
    m_currentTarget = target;
    m_currentJobId = jobId.empty() ? header.substr(0, 16) : jobId;

    Job job;
    job.jobId = m_currentJobId;
    job.header.clear();
    for (size_t i = 0; i + 1 < header.length(); i += 2) {
        job.header.push_back(static_cast<uint8_t>(
            std::stoul(header.substr(i, 2), nullptr, 16)));
    }
    job.seedHex = seed;
    job.targetHex = target;
    if (target.length() >= 16) {
        job.targetInt = std::stoull(target.substr(0, 16), nullptr, 16);
    }

    std::cout << "[Stratum] New job: " << job.jobId
              << " header=" << header.substr(0, 12) << "..."
              << " target=" << target.substr(0, 8) << "..." << std::endl;

    if (m_onJob) m_onJob(job);
}

// Parse a mining.notify (or eth_getWork-style push) message.
// Accepts both [header, seed, target] and [jobid, header, seed, target].
void StratumClient::handleNotify(const std::string& line) {
    std::string a0 = strip_0x(json_array_elem(line, "params", 0));
    std::string a1 = strip_0x(json_array_elem(line, "params", 1));
    std::string a2 = strip_0x(json_array_elem(line, "params", 2));
    std::string a3 = strip_0x(json_array_elem(line, "params", 3));

    if (a0.empty()) {
        // Some ethproxy pushes put the job in "result"
        a0 = strip_0x(json_array_elem(line, "result", 0));
        a1 = strip_0x(json_array_elem(line, "result", 1));
        a2 = strip_0x(json_array_elem(line, "result", 2));
        a3 = strip_0x(json_array_elem(line, "result", 3));
    }

    if (a0.size() == 64) {
        // [header, seed, target]
        processJob("", a0, a1, a2);
    } else if (a1.size() == 64) {
        // [jobid, header, seed, target]
        processJob(a0, a1, a2, a3);
    } else {
        std::cerr << "[Stratum] Unrecognized job notification" << std::endl;
    }
}

void StratumClient::handleResponse(const std::string& line) {
    uint64_t id = json_id_val(line);
    std::string method = get_method(line);

    // Notifications from pool
    if (id == 0 && !method.empty()) {
        if (method == "mining.notify") {
            handleNotify(line);
        } else if (method == "mining.set_difficulty") {
            // Target always comes with the job itself; nothing to do here.
            std::string d = json_array_elem(line, "params", 0);
            std::cout << "[Stratum] set_difficulty: " << d << std::endl;
        } else if (method == "client.get_version") {
            sendLine("{\"id\":0,\"result\":\"miner-saya/v1.0\"}");
        } else {
            std::cout << "[Stratum] Method: " << method << std::endl;
        }
        return;
    }

    // Response to our eth_getWork request
    if (id != 0 && id == m_pendingGetWorkId) {
        std::string header = strip_0x(json_array_elem(line, "result", 0));
        std::string seed   = strip_0x(json_array_elem(line, "result", 1));
        std::string target = strip_0x(json_array_elem(line, "result", 2));
        if (header.empty() || target.empty()) {
            // Not a getWork result (e.g. server reusing the id). Ignore.
            return;
        }
        processJob("", header, seed, target);
        return;
    }

    // Response to login
    if (id != 0 && id == m_loginId) {
        return; // already handled in doEthLogin
    }

    // Response to share submission (eth_submitWork)
    if (json_is_true(line)) {
        if (m_onResult) m_onResult(true, "");
    } else {
        auto pos = line.find("\"error\"");
        if (pos != std::string::npos) {
            std::string err = json_find_str(line, "message");
            if (err.empty()) err = "rejected";
            if (m_onResult) m_onResult(false, err);
        }
    }
}

bool StratumClient::submitShare(const std::string& headerHex,
                                const std::string& nonceHex,
                                const std::string& mixHashHex) {
    if (!m_running || m_sock < 0) return false;

    std::string h = (headerHex.substr(0, 2) == "0x") ? headerHex : "0x" + headerHex;
    std::string n = (nonceHex.substr(0, 2) == "0x") ? nonceHex : "0x" + nonceHex;
    std::string m = (mixHashHex.substr(0, 2) == "0x") ? mixHashHex : "0x" + mixHashHex;

    uint64_t id = m_msgId.fetch_add(1, std::memory_order_relaxed);
    std::string params = "[\"" + n + "\",\"" + h + "\",\"" + m + "\"]";
    std::string msg = "{\"id\":" + std::to_string(id) +
                      ",\"method\":\"eth_submitWork\",\"params\":" + params + "}";

    sendLine(msg);
    return true;
}
