#include "srtdynamic.h"
#include <srt.h>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>

namespace srtdynamic {

// Internal SRT socket wrapper
class SrtSocket {
private:
    SRTSOCKET socket;
    bool is_valid;

public:
    explicit SrtSocket() : socket(SRT_INVALID_SOCK), is_valid(false) {}
    
    void create() {
        socket = srt_create_socket();
        if (socket == SRT_INVALID_SOCK) {
            throw std::runtime_error("Failed to create SRT socket");
        }
        is_valid = true;
    }

    void configure(const srtdynamic::StreamConfig& config) {
        if (!is_valid) throw std::runtime_error("Invalid socket");
        
        int optval = SRTT_LIVE;
        if (srt_setsockopt(socket, 0, SRTO_TRANSTYPE, &optval, sizeof(optval)) == SRT_ERROR) {
            throw std::runtime_error("Failed to set transport type");
        }

        optval = config.payload_size;
        if (srt_setsockopt(socket, 0, SRTO_PAYLOADSIZE, &optval, sizeof(optval)) == SRT_ERROR) {
            throw std::runtime_error("Failed to set payload size");
        }

        srt_setsockopt(socket, 0, SRTO_INPUTBW, &config.target_bitrate, sizeof(config.target_bitrate));
        srt_setsockopt(socket, 0, SRTO_OHEADBW, &config.overhead_percent, sizeof(config.overhead_percent));
        srt_setsockopt(socket, 0, SRTO_MESSAGEAPI, &config.message_mode, sizeof(config.message_mode));
        srt_setsockopt(socket, 0, SRTO_TSBPDMODE, &config.message_mode, sizeof(config.message_mode));
        srt_setsockopt(socket, 0, SRTO_PEERLATENCY, &config.latency, sizeof(config.latency));
        srt_setsockopt(socket, 0, SRTO_RCVLATENCY, &config.latency, sizeof(config.latency));
        srt_setsockopt(socket, 0, SRTO_RETRANSMITALGO, &config.retrans_algo, sizeof(config.retrans_algo));

        int size_for_buffers = (config.target_bitrate / 8) * (config.latency / 1000);
        srt_setsockopt(socket, 0, SRTO_UDP_RCVBUF, &size_for_buffers, sizeof(size_for_buffers));
        srt_setsockopt(socket, 0, SRTO_UDP_SNDBUF, &size_for_buffers, sizeof(size_for_buffers));
    }

    void bind(const char* addr, int port) {
        if (!is_valid) throw std::runtime_error("Invalid socket");
        
        sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        inet_pton(AF_INET, addr, &sa.sin_addr);
        
        if (srt_bind(socket, (sockaddr*)&sa, sizeof sa) == SRT_ERROR) {
            throw std::runtime_error("Failed to bind socket");
        }
    }

    void listen() {
        if (!is_valid) throw std::runtime_error("Invalid socket");
        if (srt_listen(socket, 1) == SRT_ERROR) {
            throw std::runtime_error("Failed to listen");
        }
    }

    SRTSOCKET accept() {
        if (!is_valid) throw std::runtime_error("Invalid socket");
        return srt_accept(socket, nullptr, nullptr);
    }

    void connect(const char* addr, int port) {
        if (!is_valid) throw std::runtime_error("Invalid socket");
        
        sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        inet_pton(AF_INET, addr, &sa.sin_addr);
        
        if (srt_connect(socket, (sockaddr*)&sa, sizeof sa) == SRT_ERROR) {
            throw std::runtime_error("Failed to connect");
        }
    }

    int sendmsg(const char* buf, int len) {
        if (!is_valid) throw std::runtime_error("Invalid socket");
        return srt_sendmsg(socket, buf, len, -1, 0);
    }

    int recvmsg(char* buf, int len) {
        if (!is_valid) throw std::runtime_error("Invalid socket");
        return srt_recvmsg(socket, buf, len);
    }

    void getStats(SRT_TRACEBSTATS* stats) const {
        if (!is_valid) throw std::runtime_error("Invalid socket");
        if (srt_bstats(socket, stats, 0) == SRT_ERROR) {
            throw std::runtime_error("Failed to get stats");
        }
    }

    ~SrtSocket() {
        if (is_valid) {
            srt_close(socket);
        }
    }

    SRTSOCKET get() const { return socket; }
};

// Implementation of SRTDynamic
class SRTDynamicImpl : public SRTDynamic {
private:
    SrtSocket socket;
    std::string address;
    int port;
    srtdynamic::StreamConfig config;
    bool is_sender;
    std::atomic<bool> running{false};
    std::thread worker_thread;
    std::queue<std::vector<char>> send_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    PacketCallback packet_callback;
    StatsCallback stats_callback;

    void worker() {
        if (is_sender) {
            senderWorker();
        } else {
            receiverWorker();
        }
    }

    void senderWorker() {
        while (running) {
            std::vector<char> data;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [this] { return !running || !send_queue.empty(); });
                if (!running) break;
                data = std::move(send_queue.front());
                send_queue.pop();
            }

            if (socket.sendmsg(data.data(), data.size()) == SRT_ERROR) {
                std::cerr << "Failed to send data" << std::endl;
            }
        }
    }

    void receiverWorker() {
        std::vector<char> buffer(config.payload_size);
        while (running) {
            int bytes = socket.recvmsg(buffer.data(), buffer.size());
            if (bytes <= 0) continue;

            if (packet_callback) {
                uint64_t timestamp;
                uint64_t number;
                memcpy(&timestamp, buffer.data(), sizeof(uint64_t));
                memcpy(&number, buffer.data() + sizeof(uint64_t), sizeof(uint64_t));
                std::vector<char> data(buffer.begin() + 2 * sizeof(uint64_t), buffer.begin() + bytes);
                packet_callback(data, timestamp, number);
            }
        }
    }

public:
    SRTDynamicImpl(const std::string& addr, int p, const srtdynamic::StreamConfig& cfg, bool sender)
        : address(addr), port(p), config(cfg), is_sender(sender) {}

    bool start() override {
        if (running) return true;

        try {
            socket.create();
            socket.configure(config);

            if (is_sender) {
                socket.connect(address.c_str(), port);
            } else {
                socket.bind(address.c_str(), port);
                socket.listen();
                SRTSOCKET accepted = socket.accept();
                if (accepted == SRT_INVALID_SOCK) {
                    return false;
                }
            }

            running = true;
            worker_thread = std::thread(&SRTDynamicImpl::worker, this);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Failed to start: " << e.what() << std::endl;
            return false;
        }
    }

    void stop() override {
        if (!running) return;
        running = false;
        queue_cv.notify_one();
        if (worker_thread.joinable()) {
            worker_thread.join();
        }
    }

    bool isRunning() const override {
        return running;
    }

    bool send(const std::vector<char>& data, uint64_t timestamp, uint64_t number) override {
        if (!is_sender || !running) return false;

        std::vector<char> packet;
        packet.resize(2 * sizeof(uint64_t) + data.size());
        memcpy(packet.data(), &timestamp, sizeof(uint64_t));
        memcpy(packet.data() + sizeof(uint64_t), &number, sizeof(uint64_t));
        memcpy(packet.data() + 2 * sizeof(uint64_t), data.data(), data.size());

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            send_queue.push(std::move(packet));
        }
        queue_cv.notify_one();
        return true;
    }

    void setPacketCallback(PacketCallback callback) override {
        packet_callback = std::move(callback);
    }

    void setStatsCallback(StatsCallback callback) override {
        stats_callback = std::move(callback);
    }

    SRTStats getStats() const override {
        SRT_TRACEBSTATS stats;
        socket.getStats(&stats);

        SRTStats result;
        result.mbpsSendRate = stats.mbpsSendRate;
        result.mbpsRecvRate = stats.mbpsRecvRate;
        result.mbpsBandwidth = stats.mbpsBandwidth;
        result.pktSent = stats.pktSent;
        result.pktRecv = stats.pktRecv;
        result.pktRetrans = stats.pktRetrans;
        result.pktSndLoss = stats.pktSndLoss;
        result.pktRcvLoss = stats.pktRcvLoss;
        result.msRTT = stats.msRTT;
        result.byteSent = stats.byteSent;
        result.byteRecv = stats.byteRecv;
        result.byteRcvLoss = stats.byteRcvLoss;
        
        return result;
    }
};

// Factory methods implementation
std::unique_ptr<SRTDynamic> SRTDynamic::createSender(const std::string& address, int port, const StreamConfig& config) {
    return std::make_unique<SRTDynamicImpl>(address, port, config, true);
}

std::unique_ptr<SRTDynamic> SRTDynamic::createReceiver(const std::string& address, int port, const StreamConfig& config) {
    return std::make_unique<SRTDynamicImpl>(address, port, config, false);
}

} // namespace srtdynamic
