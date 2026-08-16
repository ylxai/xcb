#ifndef STRATUMCLIENT_HPP
#define STRATUMCLIENT_HPP

#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <cstdint>
#include <random>
#include <openssl/types.h>   // SSL, SSL_CTX (OpenSSL 1.1+)

struct Job {
    std::string jobId;
    std::vector<uint8_t> header;  // 32 bytes binary
    std::string seedHex;
    std::string targetHex;         // hex target
    uint64_t targetInt = 0x00ffffffffffffffULL;
    bool clean = true;
};

class StratumClient {
public:
    StratumClient(const std::string& host, uint16_t port,
                  const std::string& wallet, const std::string& worker,
                  const std::string& password = "x", bool tls = false);
    ~StratumClient();

    void connect();
    void disconnect();
    bool isConnected() const;
    bool submitShare(const std::string& headerHex, const std::string& nonceHex,
                     const std::string& mixHashHex);
    void setJobCallback(std::function<void(const Job&)> cb) { m_onJob = cb; }
    void setResultCallback(std::function<void(bool, const std::string&)> cb) { m_onResult = cb; }

private:
    // Low-level send: plain TCP atau via SSL. Return false kalau write gagal.
    bool sendRaw(const std::string& data);
    // Low-level recv: plain TCP atau via SSL. Return false kalau socket error.
    bool recvRaw(char* buf, size_t len, ssize_t* out);

    void run();
    void sendLine(const std::string& line);
    std::string recvLine(double timeoutSec);
    bool doEthLogin();
    bool doEthGetWork();
    void handleResponse(const std::string& line);
    bool openSocket();   // DNS + connect (+ TLS handshake). true = success.
    void closeSocket();  // tutup SSL + socket
    int nextBackoff();   // backoff + jitter untuk reconnect

    std::string m_host;
    uint16_t m_port;
    std::string m_wallet;
    std::string m_worker;
    std::string m_password;
    std::string m_workerName;
    bool m_tls = false;

    int m_sock = -1;
    SSL* m_ssl = nullptr;
    SSL_CTX* m_sslCtx = nullptr;   // dibuat sekali di connect(), dibuang di closeSocket terakhir
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::mutex m_sendMutex;
    std::mutex m_recvMutex;
    uint64_t m_msgId = 1;
    std::string m_recvBuf;
    std::string m_currentHeader;   // hex header of current job (no 0x)
    std::string m_currentSeed;
    std::string m_currentTarget;
    std::string m_currentJobId;    // job ID for mining.submit

    // Backoff reconnect: base delay + jitter (anti thundering herd / rate limit)
    int m_reconnectDelaySec = 2;
    std::mt19937 m_rng;

    // Callbacks
    std::function<void(const Job&)> m_onJob;
    std::function<void(bool, const std::string&)> m_onResult;
};

#endif