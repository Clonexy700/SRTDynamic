// srtdynamic.cpp : Этот файл содержит функцию "main". Здесь начинается и заканчивается выполнение программы.
//

#include <iostream>
#include <srt/srt.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <csignal>
#include <memory>
#include <stdexcept>

// Configuration constants
constexpr int NUM_STREAMS = 2;
constexpr int PACKET_SIZE = 1316;  // Standard SRT payload size
constexpr int BATCH_SIZE = 7;
constexpr int STATS_INTERVAL_MS = 100;
constexpr int TARGET_BITRATE_KBPS = 10000;  // 10 Mbps

// RAII wrapper for SRT socket
class SrtSocket {
public:
    explicit SrtSocket() {
        socket_ = srt_create_socket();
        if (socket_ == SRT_INVALID_SOCK) {
            throw std::runtime_error("Failed to create SRT socket");
        }
    }

    ~SrtSocket() {
        if (socket_ != SRT_INVALID_SOCK) {
            srt_close(socket_);
        }
    }

    SRTSOCKET get() const { return socket_; }

private:
    SRTSOCKET socket_;
};

// RAII wrapper for file stream
class LogFile {
public:
    explicit LogFile(const std::string& filename) : file_(filename) {
        if (!file_.is_open()) {
            throw std::runtime_error("Failed to open log file");
        }
        // Write CSV header
        file_ << "Timepoint,StreamID,pktSent,pktRecv,pktLoss,sendRate,groupID,minLatency,avgLatency,maxLatency\n";
    }

    ~LogFile() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    void write(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_ << line << std::endl;
    }

private:
    std::ofstream file_;
    std::mutex mutex_;
};

// Global state
struct PacketBatch {
    uint64_t group_id;
    std::vector<std::vector<uint8_t>> packets;
};

std::queue<PacketBatch> packet_queue;
std::mutex queue_mutex;
std::condition_variable queue_cv;
std::atomic<bool> running{true};
std::unique_ptr<LogFile> log_file;

// Time utilities
std::string generateLogFilename() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    std::stringstream ss;
    ss << "log_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".csv";
    return ss.str();
}

std::string getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()) % 1000000;
    std::tm tm;
    localtime_s(&tm, &time);
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << "." 
       << std::setfill('0') << std::setw(6) << ms.count();
    return ss.str();
}

// Thread functions
void receiverThread(int stream_id) {
    try {
        SrtSocket socket;
        // Configure socket for live streaming
        int optval = 1;
        srt_setsockflag(socket.get(), SRTO_SENDER, &optval, sizeof(optval));
        srt_setsockflag(socket.get(), SRTO_PAYLOADSIZE, &PACKET_SIZE, sizeof(PACKET_SIZE));
        
        // Connect to external interference generator
        // Implementation depends on your specific setup
        
        std::vector<std::vector<uint8_t>> batch_buffer;
        uint64_t current_group_id = 0;
        
        while (running) {
            std::vector<uint8_t> packet(PACKET_SIZE);
            int bytes_received = srt_recvmsg(socket.get(), 
                reinterpret_cast<char*>(packet.data()), PACKET_SIZE);
            
            if (bytes_received > 0) {
                batch_buffer.push_back(std::move(packet));
                
                if (batch_buffer.size() == BATCH_SIZE) {
                    PacketBatch batch;
                    batch.group_id = current_group_id++;
                    batch.packets = std::move(batch_buffer);
                    batch_buffer.clear();
                    
                    {
                        std::lock_guard<std::mutex> lock(queue_mutex);
                        packet_queue.push(std::move(batch));
                    }
                    queue_cv.notify_one();
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Receiver thread " << stream_id << " error: " << e.what() << std::endl;
    }
}

void senderThread(int stream_id) {
    try {
        SrtSocket socket;
        // Configure socket for live streaming
        int optval = 0;
        srt_setsockflag(socket.get(), SRTO_SENDER, &optval, sizeof(optval));
        srt_setsockflag(socket.get(), SRTO_PAYLOADSIZE, &PACKET_SIZE, sizeof(PACKET_SIZE));
        
        const auto packet_interval = std::chrono::microseconds(
            (PACKET_SIZE * 8 * 1000) / TARGET_BITRATE_KBPS);
        
        while (running) {
            PacketBatch batch;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [&] { return !packet_queue.empty() || !running; });
                if (!running) break;
                batch = std::move(packet_queue.front());
                packet_queue.pop();
            }
            
            for (const auto& packet : batch.packets) {
                auto start_time = std::chrono::steady_clock::now();
                srt_sendmsg(socket.get(), 
                    reinterpret_cast<const char*>(packet.data()), 
                    packet.size(), -1, false);
                
                auto elapsed = std::chrono::steady_clock::now() - start_time;
                if (elapsed < packet_interval) {
                    std::this_thread::sleep_for(packet_interval - elapsed);
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Sender thread " << stream_id << " error: " << e.what() << std::endl;
    }
}

void statsThread() {
    try {
        std::vector<SrtSocket> sender_sockets(NUM_STREAMS);
        std::vector<SRT_TRACEBSTATS> stats(NUM_STREAMS);
        
        while (running) {
            auto start_time = std::chrono::steady_clock::now();
            
            for (int i = 0; i < NUM_STREAMS; ++i) {
                if (srt_bstats(sender_sockets[i].get(), &stats[i], 0) == SRT_ERROR) {
                    continue;
                }
                
                std::stringstream ss;
                ss << getCurrentTime() << ","
                   << i << ","
                   << stats[i].pktSent << ","
                   << stats[i].pktRecv << ","
                   << stats[i].pktLoss << ","
                   << stats[i].mbpsSendRate << ","
                   << "0,0,0,0";  // Placeholder for group ID and latencies
                
                log_file->write(ss.str());
            }
            
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            auto sleep_time = std::chrono::milliseconds(STATS_INTERVAL_MS) - 
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
            if (sleep_time > std::chrono::milliseconds(0)) {
                std::this_thread::sleep_for(sleep_time);
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
        
        // Set up signal handler
        signal(SIGINT, signalHandler);
        
        // Create log file
        log_file = std::make_unique<LogFile>(generateLogFilename());
        
        // Create and start threads
        std::vector<std::thread> threads;
        
        for (int i = 0; i < NUM_STREAMS; ++i) {
            threads.emplace_back(receiverThread, i);
            threads.emplace_back(senderThread, i);
        }
        threads.emplace_back(statsThread);
        
        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }
        
        // Cleanup
        srt_cleanup();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}

// Запуск программы: CTRL+F5 или меню "Отладка" > "Запуск без отладки"
// Отладка программы: F5 или меню "Отладка" > "Запустить отладку"

// Советы по началу работы 
//   1. В окне обозревателя решений можно добавлять файлы и управлять ими.
//   2. В окне Team Explorer можно подключиться к системе управления версиями.
//   3. В окне "Выходные данные" можно просматривать выходные данные сборки и другие сообщения.
//   4. В окне "Список ошибок" можно просматривать ошибки.
//   5. Последовательно выберите пункты меню "Проект" > "Добавить новый элемент", чтобы создать файлы кода, или "Проект" > "Добавить существующий элемент", чтобы добавить в проект существующие файлы кода.
//   6. Чтобы снова открыть этот проект позже, выберите пункты меню "Файл" > "Открыть" > "Проект" и выберите SLN-файл.
