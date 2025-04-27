#include <srt.h>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstring>
#include <csignal>
#include <cmath>
#include <sstream>
#include <limits>
#include <winsock2.h>
#include <ws2tcpip.h>

// Constants
constexpr int NUM_STREAMS = 2;
constexpr int PACKET_SIZE = 1316;
constexpr int BATCH_SIZE = 7;
constexpr int STATS_INTERVAL_MS = 100;
constexpr int UPDATE_INTERVAL_MS = 1000;

// Port configuration for each stream
const int SENDER_PORTS[NUM_STREAMS] = {12345, 12346};
const int RECEIVER_PORTS[NUM_STREAMS] = {20000, 20001};

// Packet structure
struct Packet {
    int stream_id;
    uint64_t group_id;
    uint64_t timestamp;
    std::vector<char> payload;
};

// SRT socket wrapper with RAII
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

    void configureLiveStream() {
        if (!is_valid) throw std::runtime_error("Invalid socket");
        
        int optval = SRTT_LIVE;
        if (srt_setsockopt(socket, 0, SRTO_TRANSTYPE, &optval, sizeof(optval)) == SRT_ERROR) {
            throw std::runtime_error("Failed to set transport type");
        }

        optval = PACKET_SIZE;
        if (srt_setsockopt(socket, 0, SRTO_PAYLOADSIZE, &optval, sizeof(optval)) == SRT_ERROR) {
            throw std::runtime_error("Failed to set payload size");
        }

        // Configure additional SRT options
        int64_t target_bitrate = 22000 * 1000; // 22 Mbps
        srt_setsockopt(socket, 0, SRTO_INPUTBW, &target_bitrate, sizeof(target_bitrate));

        int overhead_percent = 20;
        srt_setsockopt(socket, 0, SRTO_OHEADBW, &overhead_percent, sizeof(overhead_percent));

        bool message_mode = true;
        srt_setsockopt(socket, 0, SRTO_MESSAGEAPI, &message_mode, sizeof(message_mode));

        int rendezvous = 0;
        srt_setsockopt(socket, 0, SRTO_RENDEZVOUS, &rendezvous, sizeof(rendezvous));

        bool tsbpd = true;
        srt_setsockopt(socket, 0, SRTO_TSBPDMODE, &tsbpd, sizeof(tsbpd));

        int latency = 4000; // 4 seconds
        srt_setsockopt(socket, 0, SRTO_PEERLATENCY, &latency, sizeof(latency));
        srt_setsockopt(socket, 0, SRTO_RCVLATENCY, &latency, sizeof(latency));
        srt_setsockopt(socket, 0, SRTO_SNDDROPDELAY, &latency, sizeof(latency));

        int retransAlgo = 1;
        srt_setsockopt(socket, 0, SRTO_RETRANSMITALGO, &retransAlgo, sizeof(retransAlgo));

        int size_for_buffers = (target_bitrate / 8) * (latency / 1000);
        srt_setsockopt(socket, 0, SRTO_UDP_RCVBUF, &size_for_buffers, sizeof(size_for_buffers));
        srt_setsockopt(socket, 0, SRTO_UDP_SNDBUF, &size_for_buffers, sizeof(size_for_buffers));
        srt_setsockopt(socket, 0, SRTO_SNDBUF, &size_for_buffers, sizeof(size_for_buffers));
        srt_setsockopt(socket, 0, SRTO_RCVBUF, &size_for_buffers, sizeof(size_for_buffers));
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

    void getStats(SRT_TRACEBSTATS* stats) {
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

// Global state
std::mutex queue_mutex;
std::condition_variable queue_cv;
std::queue<Packet> packet_queue;
std::mutex log_mutex;
std::ofstream log_file;
std::atomic<bool> running{true};
std::vector<std::thread> threads;
std::vector<SrtSocket> sender_sockets;
std::vector<SrtSocket> receiver_sockets;

// Utility functions
std::string generateLogFilename() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    std::stringstream ss;
    ss << "stats_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".csv";
    return ss.str();
}

std::string getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::tm tm;
    localtime_s(&tm, &time);
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "." 
       << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

// Thread functions
void receiverThread(int stream_id) {
    try {
        std::vector<Packet> batch;
        uint64_t group_id = 0;
        
        while (running) {
            Packet pkt;
            pkt.stream_id = stream_id;
            pkt.payload.resize(PACKET_SIZE);
            
            int bytes = receiver_sockets[stream_id].recvmsg(pkt.payload.data(), PACKET_SIZE);
            if (bytes <= 0) continue;
            
            // Extract timestamp from first 8 bytes
            uint64_t timestamp;
            memcpy(&timestamp, pkt.payload.data(), sizeof(uint64_t));
            pkt.timestamp = timestamp;
            batch.push_back(pkt);
            
            if (batch.size() == BATCH_SIZE) {
                for (auto& p : batch) {
                    p.group_id = group_id;
                }
                
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    for (const auto& p : batch) {
                        packet_queue.push(p);
                    }
                }
                queue_cv.notify_one();
                
                batch.clear();
                group_id++;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Receiver thread " << stream_id << " error: " << e.what() << std::endl;
    }
}

void senderThread(int stream_id) {
    try {
        while (running) {
            std::vector<Packet> batch;
            
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [&] { return !running || !packet_queue.empty(); });
                
                if (!running) break;
                
                while (!packet_queue.empty() && batch.size() < BATCH_SIZE) {
                    if (packet_queue.front().stream_id == stream_id) {
                        batch.push_back(packet_queue.front());
                        packet_queue.pop();
                    } else {
                        break;
                    }
                }
            }
            
            for (auto& pkt : batch) {
                // Re-embed timestamp and group_id
                char* payload_ptr = const_cast<char*>(pkt.payload.data());
                memcpy((void*)payload_ptr, (const void*)&pkt.timestamp, sizeof(uint64_t));
                memcpy((void*)(payload_ptr + sizeof(uint64_t)), (const void*)&pkt.group_id, sizeof(uint64_t));
                
                // Enforce bitrate with sleep
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                
                sender_sockets[stream_id].sendmsg(pkt.payload.data(), pkt.payload.size());
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Sender thread " << stream_id << " error: " << e.what() << std::endl;
    }
}

void statsThread() {
    try {
        std::vector<SRT_TRACEBSTATS> sender_stats(NUM_STREAMS);
        std::vector<SRT_TRACEBSTATS> receiver_stats(NUM_STREAMS);
        
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(STATS_INTERVAL_MS));
            
            for (int i = 0; i < NUM_STREAMS; ++i) {
                sender_sockets[i].getStats(&sender_stats[i]);
                receiver_sockets[i].getStats(&receiver_stats[i]);
                
                // Log to CSV
                {
                    std::lock_guard<std::mutex> lock(log_mutex);
                    log_file << getCurrentTime() << ","
                            << "Stream " << (i + 1) << ","
                            << "net_delay (G1)," << 100 << ","  // Example value
                            << "delay (G1)," << 4000 << ","     // Example value
                            << "gen_loss (G1)," << 0 << ","     // Example value
                            << "overhead (G2)," << 20 << ","
                            << "pktFlowWindow (G2)," << sender_stats[i].pktFlowWindow << ","
                            << "pktCongestionWindow (G2)," << sender_stats[i].pktCongestionWindow << ","
                            << "pktFlightSize (G2)," << sender_stats[i].pktFlightSize << ","
                            << "byteAvailSndBuf (G2)," << sender_stats[i].byteAvailSndBuf << ","
                            << "byteAvailRcvBuf (G2)," << receiver_stats[i].byteAvailRcvBuf << ","
                            << "msRTT (G3)," << sender_stats[i].msRTT << ","
                            << "mbpsSendRate (G3)," << sender_stats[i].mbpsSendRate << ","
                            << "mbpsRecvRate (G3)," << receiver_stats[i].mbpsRecvRate << ","
                            << "mbpsBandwidth (G4)," << sender_stats[i].mbpsBandwidth << ","
                            << "pktSent (G4)," << sender_stats[i].pktSent << ","
                            << "pktRetrans (G4)," << sender_stats[i].pktRetrans << ","
                            << "pktSentTotal (G4)," << sender_stats[i].pktSentTotal << ","
                            << "pktSentUnique (G4)," << sender_stats[i].pktSentUnique << ","
                            << "pktSndLoss (G4)," << sender_stats[i].pktSndLoss << ","
                            << "pktRecv (G4)," << receiver_stats[i].pktRecv << ","
                            << "pktRecvTotal (G4)," << receiver_stats[i].pktRecvTotal << ","
                            << "pktRecvUnique (G4)," << receiver_stats[i].pktRecvUnique << ","
                            << "pktRecvLoss (G4)," << receiver_stats[i].pktRcvLoss << ","
                            << "byteSent (G4)," << sender_stats[i].byteSent << ","
                            << "byteSentUnique (G4)," << sender_stats[i].byteSentUnique << ","
                            << "byteRecv (G4)," << receiver_stats[i].byteRecv << ","
                            << "byteRecvUnique (G4)," << receiver_stats[i].byteRecvUnique << ","
                            << "byteRcvLoss (G4)," << receiver_stats[i].byteRcvLoss << "\n";
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Stats thread error: " << e.what() << std::endl;
    }
}

void signalHandler(int) {
    running = false;
    queue_cv.notify_all();
}

int main() {
    try {
        // Initialize SRT
        if (srt_startup() == SRT_ERROR) {
            throw std::runtime_error("Failed to initialize SRT");
        }

        // Create and configure sockets
        sender_sockets.resize(NUM_STREAMS);
        receiver_sockets.resize(NUM_STREAMS);
        
        // Open log file and write header
        log_file.open(generateLogFilename());
        log_file << "Timepoint,Stream,"
                << "net_delay (G1),delay (G1),gen_loss (G1),"
                << "overhead (G2),pktFlowWindow (G2),pktCongestionWindow (G2),pktFlightSize (G2),"
                << "byteAvailSndBuf (G2),byteAvailRcvBuf (G2),"
                << "msRTT (G3),mbpsSendRate (G3),mbpsRecvRate (G3),"
                << "mbpsBandwidth (G4),pktSent (G4),pktRetrans (G4),"
                << "pktSentTotal (G4),pktSentUnique (G4),pktSndLoss (G4),"
                << "pktRecv (G4),pktRecvTotal (G4),pktRecvUnique (G4),pktRecvLoss (G4),"
                << "byteSent (G4),byteSentUnique (G4),"
                << "byteRecv (G4),byteRecvUnique (G4),byteRcvLoss (G4)\n";
        
        for (int i = 0; i < NUM_STREAMS; ++i) {
            sender_sockets[i].create();
            receiver_sockets[i].create();
            
            sender_sockets[i].configureLiveStream();
            receiver_sockets[i].configureLiveStream();
            
            // Configure sender and receiver addresses with specific ports
            sender_sockets[i].connect("127.0.0.1", SENDER_PORTS[i]);
            receiver_sockets[i].bind("127.0.0.1", RECEIVER_PORTS[i]);
            receiver_sockets[i].listen();
        }

        // Install signal handler
        signal(SIGINT, signalHandler);

        // Launch threads
        for (int i = 0; i < NUM_STREAMS; ++i) {
            threads.emplace_back(receiverThread, i);
            threads.emplace_back(senderThread, i);
        }
        threads.emplace_back(statsThread);

        // Wait for threads
        for (auto& t : threads) {
            t.join();
        }

        // Cleanup
        log_file.close();
        srt_cleanup();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
