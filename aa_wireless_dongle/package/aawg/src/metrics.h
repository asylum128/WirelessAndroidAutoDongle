#pragma once

#include <atomic>
#include <chrono>
#include <string>

class ConnectionMetrics {
public:
    static ConnectionMetrics* instance();

    void recordConnectionStart();
    void recordConnectionEnd();
    void recordBytesTransferred(uint64_t tcp_to_usb, uint64_t usb_to_tcp);
    void recordError(const std::string& error_type);

    void logStats();
    void resetStats();

    // Getters for health checks
    uint64_t getTotalConnections() const { return m_total_connections.load(); }
    uint64_t getActiveConnections() const { return m_active_connections.load(); }
    uint64_t getTotalErrors() const { return m_total_errors.load(); }
    uint64_t getTotalBytesTcpToUsb() const { return m_total_bytes_tcp_to_usb.load(); }
    uint64_t getTotalBytesUsbToTcp() const { return m_total_bytes_usb_to_tcp.load(); }
    std::chrono::seconds getUptime() const;

private:
    ConnectionMetrics();

    std::atomic<uint64_t> m_total_connections{0};
    std::atomic<uint64_t> m_active_connections{0};
    std::atomic<uint64_t> m_total_errors{0};
    std::atomic<uint64_t> m_connection_failures{0};

    std::atomic<uint64_t> m_total_bytes_tcp_to_usb{0};
    std::atomic<uint64_t> m_total_bytes_usb_to_tcp{0};

    std::chrono::steady_clock::time_point m_start_time;
    std::chrono::steady_clock::time_point m_last_connection_time;
};
