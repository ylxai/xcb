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
#include <openssl/ssl.h>
#include <openssl/err.h>

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

static std::string json_result_array(const std::string& json, int idx) {
    auto pos = json.find("\"result\"");
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
    pos++;  // skip ':'
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

// Strip "0x" or "0X" prefix from hex string
static std::string strip_0x(const std::string& s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return s.substr(2);
    return s;
}

// ------------------------------------------------------------
StratumClient::StratumClient(const std::string& host, uint16_t port,
                             const std::string& wallet, const std::string& worker,
                             const std::string& password, bool tls)
    : m_host(host), m_port(port), m_wallet(wallet),
      m_worker(worker), m_password(password), m_tls(tls),
      m_rng(std::random_device{}()) {
    m_workerName = wallet + "." + worker;
}

StratumClient::~StratumClient() {
    disconnect();
    if (m_sslCtx) { SSL_CTX_free(m_sslCtx); m_sslCtx = nullptr; }
}

bool StratumClient::isConnected() const { return m_sock >= 0 && m_running.load(); }

// ------------------------------------------------------------
// Socket lifecycle (plain TCP atau TCP+TLS)
// ------------------------------------------------------------
bool StratumClient::openSocket() {
    // Inisialisasi SSL_CTX sekali saja
    if (m_tls && !m_sslCtx) {
        m_sslCtx = SSL_CTX_new(TLS_client_method());
        if (!m_sslCtx) {
            std::cerr << "[Stratum] SSL_CTX init failed" << std::endl;
            return false;
        }
        SSL_CTX_set_default_verify_paths(m_sslCtx);
    }

    std::string portStr = std::to_string(m_port);
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (::getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        std::cerr << "[Stratum] DNS fail: " << m_host << std::endl;
        if (res) ::freeaddrinfo(res);
        return false;
    }
    int fd = -1;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        if (::connect(fd, rp->ai_addr, rp->ai_addrlen) < 0) {
            // Non-blocking: EINPROGRESS = normal, cek dengan poll
        }
        struct pollfd pfd = {fd, POLLOUT, 0};
        if (::poll(&pfd, 1, 10000) <= 0) { ::close(fd); fd = -1; continue; }
        int soError = 0;
        socklen_t errLen = sizeof(soError);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &errLen);
        if (soError != 0) { ::close(fd); fd = -1; continue; }
        ::fcntl(fd, F_SETFL, 0);
        break;  // connect OK
    }
    ::freeaddrinfo(res);
    if (fd < 0) return false;

    m_sock = fd;
    m_ssl = nullptr;

    if (m_tls) {
        m_ssl = SSL_new(m_sslCtx);
        if (!m_ssl || SSL_set_fd(m_ssl, fd) != 1 || SSL_connect(m_ssl) != 1) {
            std::cerr << "[Stratum] TLS handshake failed: " << m_host << ":" << m_port << std::endl;
            if (m_ssl) { SSL_free(m_ssl); m_ssl = nullptr; }
            ::close(fd);
            m_sock = -1;
            return false;
        }
    }

    m_recvBuf.clear();
    return true;
}

void StratumClient::closeSocket() {
    if (m_ssl) {
        SSL_shutdown(m_ssl);
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }
    if (m_sock >= 0) {
        ::shutdown(m_sock, SHUT_RDWR);
        ::close(m_sock);
        m_sock = -1;
    }
}

bool StratumClient::sendRaw(const std::string& data) {
    if (m_ssl) {
        int n = SSL_write(m_ssl, data.data(), (int)data.size());
        return n == (int)data.size();
    }
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(m_sock, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;  // koneksi putus — caller wajib reconnect
        }
        off += (size_t)n;
    }
    return true;
}

bool StratumClient::recvRaw(char* buf, size_t len, ssize_t* out) {
    if (m_ssl) {
        int n = SSL_read(m_ssl, buf, (int)len);
        *out = n;
        return n >= 0;  // n<0 = error/putus
    }
    ssize_t n = ::recv(m_sock, buf, len, 0);
    *out = n;
    return n >= 0;
}

// ------------------------------------------------------------
void StratumClient::sendLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (m_sock < 0) return;
    std::string payload = line + "\n";
    if (!sendRaw(payload)) {
        std::cerr << "[Stratum] send failed — marking disconnected" << std::endl;
        m_running = false;  // force reconnect dari run loop
    }
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
        pfd.fd = m_ssl ? SSL_get_fd(m_ssl) : m_sock;
        pfd.events = POLLIN;
        int ms = static_cast<int>(timeoutSec * 1000);
        int ret = ::poll(&pfd, 1, ms);
        if (ret <= 0) return "";
        if (pfd.revents & (POLLERR | POLLHUP)) return "";
        char buf[8192];
        ssize_t n = 0;
        if (!recvRaw(buf, sizeof(buf) - 1, &n) || n <= 0) return "";
        buf[n] = 0;
        m_recvBuf += std::string(buf, n);
    }
}

void StratumClient::disconnect() {
    if (m_running.exchange(false)) {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        closeSocket();
    }
    if (m_thread.joinable()) m_thread.join();
}

void StratumClient::connect() {
    if (m_running) return;
    if (!openSocket()) return;
    m_running = true;
    std::cout << "[Stratum] Connected to " << m_host << ":" << m_port
              << (m_tls ? " (TLS)" : "") << std::endl;
    m_thread = std::thread(&StratumClient::run, this);
}

// ------------------------------------------------------------
// Backoff: delay base 2s, max 30s, + jitter acak 0..delay/2
// (jitter menghindari ribuan pool connection bertabrakan +
//  membuat pola reconnect tidak diprediksi rate-limiter)
// ------------------------------------------------------------
int StratumClient::nextBackoff() {
    int base = m_reconnectDelaySec;
    std::uniform_int_distribution<int> dist(0, base / 2);
    int delay = base + dist(m_rng);
    m_reconnectDelaySec = std::min(base * 2, 30);
    return delay;
}

void StratumClient::run() {
    while (m_running) {
        m_msgId = 1;

        // Step 1: eth_submitLogin
        if (!doEthLogin()) {
            int delay = nextBackoff();
            std::cerr << "[Stratum] Login failed, reconnecting in " << delay << "s" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            {
                std::lock_guard<std::mutex> lock(m_sendMutex);
                closeSocket();
            }
            if (!openSocket()) {
                // masih gagal — loop, backoff naik
                continue;
            }
            continue;
        }
        m_reconnectDelaySec = 2;
        std::cout << "[Stratum] Authorized as " << m_workerName << std::endl;

        // Step 2: polling loop eth_getWork
        auto lastPoll = std::chrono::steady_clock::now();

        while (m_running) {
            std::string line = recvLine(0.1);  // 100ms poll

            if (line.empty()) {
                // Timeout — cek apakah perlu poll job baru
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastPoll).count();

                if (elapsed >= 3) {  // poll tiap 3 detik
                    if (!doEthGetWork()) {
                        std::cerr << "[Stratum] getWork failed" << std::endl;
                        break;
                    }
                    lastPoll = now;
                }
                continue;
            }

            handleResponse(line);
        }

        if (!m_running) break;  // shutdown sengaja

        // Reconnect dengan backoff
        int delay = nextBackoff();
        std::cerr << "[Stratum] Disconnected, reconnecting in " << delay << "s" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(delay));
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            closeSocket();
        }
        if (!openSocket()) continue;  // backoff naik, coba lagi
    }

    m_running = false;
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        closeSocket();
    }
}

bool StratumClient::doEthLogin() {
    std::string msg = "{\"id\":" + std::to_string(m_msgId++) +
                     ",\"method\":\"eth_submitLogin\",\"params\":[\"" +
                     json_escape(m_wallet) + "\"]}";
    sendLine(msg);

    std::string resp = recvLine(15.0);
    if (resp.empty()) return false;

    std::cout << "[Stratum] Login raw: " << resp.substr(0, 300) << std::endl;

    // Simple check: result:true, result string, atau result non-null
    bool ok = (resp.find("\"result\":true") != std::string::npos);
    if (!ok) {
        ok = (resp.find("\"result\":\"0x") != std::string::npos);
    }
    if (!ok) {
        auto rPos = resp.find("\"result\"");
        if (rPos != std::string::npos) {
            auto colon = resp.find(':', rPos + 7);
            if (colon != std::string::npos) {
                while (colon < resp.size() && resp[colon] == ' ') colon++;
                ok = (colon < resp.size() && resp[colon] != 'n'); // bukan null
            }
        }
    }
    if (!ok) {
        std::string err = json_find_str(resp, "message");
        std::cerr << "[Stratum] Auth error: '" << err << "'" << std::endl;
    }
    return ok;
}

bool StratumClient::doEthGetWork() {
    std::string msg = "{\"id\":" + std::to_string(m_msgId++) +
                     ",\"method\":\"eth_getWork\",\"params\":[]}";
    sendLine(msg);

    std::string resp = recvLine(10.0);
    if (resp.empty()) return false;

    // Parse result array: [header, seed, target]
    std::string header = strip_0x(json_result_array(resp, 0));
    std::string seed = strip_0x(json_result_array(resp, 1));
    std::string target = strip_0x(json_result_array(resp, 2));

    if (header.empty() || target.empty()) {
        std::cerr << "[Stratum] Invalid getWork response" << std::endl;
        return false;
    }

    // Job sama → skip
    if (header == m_currentHeader && seed == m_currentSeed && target == m_currentTarget) {
        return true;
    }

    m_currentHeader = header;
    m_currentSeed = seed;
    m_currentTarget = target;
    m_currentJobId = header.substr(0, 16);

    Job job;
    job.jobId = header.substr(0, 16);

    // Parse header hex → bytes
    job.header.clear();
    for (size_t i = 0; i + 1 < header.length(); i += 2) {
        try {
            job.header.push_back(static_cast<uint8_t>(
                std::stoul(header.substr(i, 2), nullptr, 16)));
        } catch (...) { break; }
    }
    job.seedHex = seed;
    job.targetHex = target;

    // Parse target ke uint64 (16 hex chars pertama as BE)
    if (target.length() >= 16) {
        try {
            job.targetInt = std::stoull(target.substr(0, 16), nullptr, 16);
        } catch (...) { /* keep default */ }
    }

    std::cout << "[Stratum] New job: " << job.jobId
              << " header=" << header.substr(0, 12) << "..."
              << " target=" << target.substr(0, 8) << "..."
              << std::endl;

    if (m_onJob) m_onJob(job);
    return true;
}

void StratumClient::handleResponse(const std::string& line) {
    uint64_t id = json_id_val(line);

    if (id == 0) {
        std::string method = json_find_str(line, "method");
        if (!method.empty()) {
            std::cout << "[Stratum] Method: " << method << std::endl;
        }
        return;
    }

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

    std::string h = (headerHex.size() >= 2 && headerHex.substr(0, 2) == "0x") ? headerHex : "0x" + headerHex;
    std::string n = (nonceHex.size() >= 2 && nonceHex.substr(0, 2) == "0x") ? nonceHex : "0x" + nonceHex;
    std::string m = (mixHashHex.size() >= 2 && mixHashHex.substr(0, 2) == "0x") ? mixHashHex : "0x" + mixHashHex;

    std::string params = "[\"" + n + "\",\"" + h + "\",\"" + m + "\"]";
    std::string msg = "{\"id\":" + std::to_string(m_msgId++) +
                     ",\"method\":\"eth_submitWork\",\"params\":" + params + "}";

    sendLine(msg);
    return true;
}