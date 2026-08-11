#ifndef STRATUMCLIENT_HPP
#define STRATUMCLIENT_HPP

#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <cstdint>

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
                  const std::string& password = "x");
    ~StratumClient();

    void connect();
    void disconnect();
    bool isConnected() const;
    bool submitShare(const std::string& headerHex, const std::string& nonceHex,
                     const std::string& mixHashHex);
    void setJobCallback(std::function<void(const Job&)> cb) { m_onJob = cb; }
    void setResultCallback(std::function<void(bool, const std::string&)> cb) { m_onResult = cb; }

private:
    void run();
    void sendLine(const std::string& line);
    bool sendFrame(const std::string& json);
    std::string recvLine(double timeoutSec);
    bool doEthLogin();
    bool doEthGetWork();
    void handleResponse(const std::string& line);
    void reconnect();
    
    std::string m_host;
    uint16_t m_port;
    std::string m_wallet;
    std::string m_worker;
    std::string m_password;
    std::string m_workerName;

    int m_sock = -1;
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
    
    // Callbacks
    std::function<void(const Job&)> m_onJob;
    std::function<void(bool, const std::string&)> m_onResult;
};

#endif
