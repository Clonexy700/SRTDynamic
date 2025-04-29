#pragma once

#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <cstdint>

namespace srtdynamic {

// Forward declarations to avoid including SRT headers
struct SRTStats;

// Configuration for SRT streams
struct StreamConfig {
    int payload_size;
    int64_t target_bitrate;
    int overhead_percent;
    int latency;
    bool message_mode;
    int retrans_algo;

    // Constructor with default values
    StreamConfig(
        int payload_size = 1316,
        int64_t target_bitrate = 22000000, // 22 Mbps
        int overhead_percent = 20,
        int latency = 4000, // 4 seconds
        bool message_mode = true,
        int retrans_algo = 1
    ) : payload_size(payload_size),
        target_bitrate(target_bitrate),
        overhead_percent(overhead_percent),
        latency(latency),
        message_mode(message_mode),
        retrans_algo(retrans_algo) {}
};

// Callback types
using PacketCallback = std::function<void(const std::vector<char>& data, uint64_t timestamp, uint64_t number)>;
using StatsCallback = std::function<void(const SRTStats& stats)>;

// Main class for SRT dynamic stream redistribution
class SRTDynamic {
public:
    // Factory methods for creating sender/receiver instances
    static std::unique_ptr<SRTDynamic> createSender(const std::string& address, int port, const StreamConfig& config = StreamConfig());
    static std::unique_ptr<SRTDynamic> createReceiver(const std::string& address, int port, const StreamConfig& config = StreamConfig());

    // Destructor
    virtual ~SRTDynamic() = default;

    // Connection management
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    // Data transfer
    virtual bool send(const std::vector<char>& data, uint64_t timestamp, uint64_t number) = 0;
    virtual void setPacketCallback(PacketCallback callback) = 0;
    virtual void setStatsCallback(StatsCallback callback) = 0;

    // Statistics
    virtual SRTStats getStats() const = 0;
};

// Statistics structure
struct SRTStats {
    double mbpsSendRate;
    double mbpsRecvRate;
    double mbpsBandwidth;
    int64_t pktSent;
    int64_t pktRecv;
    int64_t pktRetrans;
    int64_t pktSndLoss;
    int64_t pktRcvLoss;
    int64_t msRTT;
    int64_t byteSent;
    int64_t byteRecv;
    int64_t byteRcvLoss;
};

} // namespace srtdynamic 