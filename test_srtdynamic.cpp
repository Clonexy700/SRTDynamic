#include "srtdynamic.h"
#include <srt.h>
#include <iostream>
#include <thread>
#include <chrono>

using namespace srtdynamic;

void printStats(const SRTStats& stats) {
    std::cout << "Stats: "
              << "Send Rate: " << stats.mbpsSendRate << " Mbps, "
              << "Recv Rate: " << stats.mbpsRecvRate << " Mbps, "
              << "RTT: " << stats.msRTT << " ms\n";
}

void receiverCallback(const std::vector<char>& data, uint64_t timestamp, uint64_t number) {
    std::cout << "Received packet " << number << " at " << timestamp << " with size " << data.size() << " bytes\n";
}

int main() {
    try {
        // Initialize SRT
        if (srt_startup() == SRT_ERROR) {
            throw std::runtime_error("Failed to initialize SRT");
        }

        // Create receiver first
        StreamConfig config;
        config.payload_size = 1316;
        config.target_bitrate = 22000000; // 22 Mbps
        config.latency = 4000; // 4 seconds

        auto receiver = SRTDynamic::createReceiver("127.0.0.1", 12345, config);
        receiver->setPacketCallback(receiverCallback);
        receiver->setStatsCallback(printStats);

        if (!receiver->start()) {
            throw std::runtime_error("Failed to start receiver");
        }

        // Create sender
        auto sender = SRTDynamic::createSender("127.0.0.1", 12345, config);
        sender->setStatsCallback(printStats);

        if (!sender->start()) {
            throw std::runtime_error("Failed to start sender");
        }

        // Send some test data
        std::vector<char> test_data(1000, 'A');
        uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        for (uint64_t i = 0; i < 10; ++i) {
            sender->send(test_data, timestamp, i);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Wait a bit for all data to be processed
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Cleanup
        sender->stop();
        receiver->stop();
        srt_cleanup();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
} 