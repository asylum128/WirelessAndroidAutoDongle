#include "metrics.h"
#include "common.h"

/*static*/ ConnectionMetrics* ConnectionMetrics::instance() {
    static ConnectionMetrics s_instance;
    return &s_instance;
}

ConnectionMetrics::ConnectionMetrics() {
    m_start_time = std::chrono::steady_clock::now();
    m_last_connection_time = m_start_time;
}

void ConnectionMetrics::recordConnectionStart() {
    m_total_connections++;
    m_active_connections++;
    m_last_connection_time = std::chrono::steady_clock::now();
    Logger::instance()->info("Connection started (total: %lu, active: %lu)\n",
                             m_total_connections.load(), m_active_connections.load());
}

void ConnectionMetrics::recordConnectionEnd() {
    if (m_active_connections > 0) {
        m_active_connections--;
    }
    Logger::instance()->info("Connection ended (active: %lu)\n", m_active_connections.load());
}

void ConnectionMetrics::recordBytesTransferred(uint64_t tcp_to_usb, uint64_t usb_to_tcp) {
    m_total_bytes_tcp_to_usb += tcp_to_usb;
    m_total_bytes_usb_to_tcp += usb_to_tcp;
}

void ConnectionMetrics::recordError(const std::string& error_type) {
    m_total_errors++;
    Logger::instance()->warn("Error recorded: %s (total errors: %lu)\n",
                             error_type.c_str(), m_total_errors.load());
}

std::chrono::seconds ConnectionMetrics::getUptime() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - m_start_time);
}

void ConnectionMetrics::logStats() {
    auto uptime = getUptime();
    uint64_t total_bytes = m_total_bytes_tcp_to_usb.load() + m_total_bytes_usb_to_tcp.load();

    Logger::instance()->info("=== Connection Statistics ===\n");
    Logger::instance()->info("Uptime: %ld seconds\n", uptime.count());
    Logger::instance()->info("Total connections: %lu\n", m_total_connections.load());
    Logger::instance()->info("Active connections: %lu\n", m_active_connections.load());
    Logger::instance()->info("Total errors: %lu\n", m_total_errors.load());
    Logger::instance()->info("Bytes TCP->USB: %lu (%.2f MB)\n",
                             m_total_bytes_tcp_to_usb.load(),
                             m_total_bytes_tcp_to_usb.load() / (1024.0 * 1024.0));
    Logger::instance()->info("Bytes USB->TCP: %lu (%.2f MB)\n",
                             m_total_bytes_usb_to_tcp.load(),
                             m_total_bytes_usb_to_tcp.load() / (1024.0 * 1024.0));
    Logger::instance()->info("Total bytes: %lu (%.2f MB)\n",
                             total_bytes, total_bytes / (1024.0 * 1024.0));

    if (uptime.count() > 0) {
        uint64_t throughput = total_bytes / uptime.count();
        Logger::instance()->info("Average throughput: %lu bytes/sec (%.2f KB/s)\n",
                                 throughput, throughput / 1024.0);
    }
    Logger::instance()->info("============================\n");
}

void ConnectionMetrics::resetStats() {
    m_total_connections = 0;
    m_active_connections = 0;
    m_total_errors = 0;
    m_connection_failures = 0;
    m_total_bytes_tcp_to_usb = 0;
    m_total_bytes_usb_to_tcp = 0;
    m_start_time = std::chrono::steady_clock::now();
    Logger::instance()->info("Connection metrics reset\n");
}
