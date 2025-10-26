#pragma once

#include <atomic>
#include <optional>
#include <thread>
#include <deque>
#include <vector>
#include <mutex>

// Circular buffer for efficient data transfer
class CircularBuffer {
public:
    CircularBuffer(size_t capacity);
    ~CircularBuffer();

    size_t write(const unsigned char* data, size_t len);
    size_t read(unsigned char* data, size_t len);
    size_t available() const;
    size_t space() const;
    void clear();

private:
    unsigned char* m_buffer;
    size_t m_capacity;
    size_t m_head;
    size_t m_tail;
    size_t m_size;
    mutable std::mutex m_mutex;
};

class AAWProxy {
public:
    AAWProxy();
    ~AAWProxy();

    std::optional<std::thread> startServer(int32_t port);

private:
    void handleClient(int server_fd);
    void forwardAsync(std::atomic<bool>& should_exit);
    void stopForwarding(std::atomic<bool>& should_exit);

    bool setNonBlocking(int fd);
    ssize_t readMessage(int fd, unsigned char *buf, size_t buffer_len);

    int m_usb_fd = -1;
    int m_tcp_fd = -1;

    std::optional<std::thread> m_forward_thread = std::nullopt;

    std::atomic<bool> m_log_communication = false;

    // Circular buffers for async I/O
    CircularBuffer* m_tcp_to_usb_buffer;
    CircularBuffer* m_usb_to_tcp_buffer;

    // Statistics
    std::atomic<uint64_t> m_bytes_tcp_to_usb{0};
    std::atomic<uint64_t> m_bytes_usb_to_tcp{0};
};
