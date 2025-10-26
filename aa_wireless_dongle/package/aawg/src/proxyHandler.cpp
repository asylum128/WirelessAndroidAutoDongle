#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <errno.h>
#include <thread>
#include <optional>
#include <atomic>
#include <string>

#include "common.h"
#include "usb.h"
#include "bluetoothHandler.h"
#include "proxyHandler.h"

// ======== CircularBuffer Implementation ========

CircularBuffer::CircularBuffer(size_t capacity)
    : m_capacity(capacity), m_head(0), m_tail(0), m_size(0) {
    m_buffer = new unsigned char[capacity];
}

CircularBuffer::~CircularBuffer() {
    delete[] m_buffer;
}

size_t CircularBuffer::write(const unsigned char* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_mutex);

    size_t available_space = m_capacity - m_size;
    size_t to_write = (len < available_space) ? len : available_space;

    for (size_t i = 0; i < to_write; i++) {
        m_buffer[m_tail] = data[i];
        m_tail = (m_tail + 1) % m_capacity;
    }

    m_size += to_write;
    return to_write;
}

size_t CircularBuffer::read(unsigned char* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_mutex);

    size_t to_read = (len < m_size) ? len : m_size;

    for (size_t i = 0; i < to_read; i++) {
        data[i] = m_buffer[m_head];
        m_head = (m_head + 1) % m_capacity;
    }

    m_size -= to_read;
    return to_read;
}

size_t CircularBuffer::available() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_size;
}

size_t CircularBuffer::space() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_capacity - m_size;
}

void CircularBuffer::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_head = 0;
    m_tail = 0;
    m_size = 0;
}

// ======== AAWProxy Implementation ========

AAWProxy::AAWProxy() {
    // 256KB buffers for each direction
    m_tcp_to_usb_buffer = new CircularBuffer(262144);
    m_usb_to_tcp_buffer = new CircularBuffer(262144);
}

AAWProxy::~AAWProxy() {
    delete m_tcp_to_usb_buffer;
    delete m_usb_to_tcp_buffer;
}

bool AAWProxy::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        Logger::instance()->error("fcntl F_GETFL failed: %s\n", strerror(errno));
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        Logger::instance()->error("fcntl F_SETFL O_NONBLOCK failed: %s\n", strerror(errno));
        return false;
    }

    return true;
}

ssize_t AAWProxy::readMessage(int fd, unsigned char *buffer, size_t buffer_len) {
    // Read header (4 bytes minimum)
    unsigned char header[8];
    ssize_t header_len = read(fd, header, 4);

    if (header_len <= 0) {
        return header_len;
    }

    if (header_len < 4) {
        errno = EAGAIN;
        return -1;
    }

    size_t message_length = (header[2] << 8) + header[3];

    constexpr char FRAME_TYPE_FIRST = 1 << 0;
    constexpr char FRAME_TYPE_LAST = 1 << 1;
    constexpr char FRAME_TYPE_MASK = FRAME_TYPE_FIRST | FRAME_TYPE_LAST;

    size_t total_header_len = 4;
    if ((header[1] & FRAME_TYPE_MASK) == FRAME_TYPE_FIRST) {
        // Need to read 4 more bytes
        ssize_t extra = read(fd, header + 4, 4);
        if (extra < 4) {
            errno = EAGAIN;
            return -1;
        }
        message_length += 4;
        total_header_len = 8;
    }

    if ((total_header_len + message_length) > buffer_len) {
        errno = EMSGSIZE;
        return -1;
    }

    // Copy header to buffer
    memcpy(buffer, header, total_header_len);

    // Read message body
    size_t total_read = total_header_len;
    while (total_read < (total_header_len + message_length)) {
        ssize_t n = read(fd, buffer + total_read, (total_header_len + message_length) - total_read);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Need more data
                errno = EAGAIN;
                return -1;
            }
            return n;
        }
        if (n == 0) {
            return 0;
        }
        total_read += n;
    }

    return total_read;
}

void AAWProxy::forwardAsync(std::atomic<bool>& should_exit) {
    Logger::instance()->info("Starting async I/O forwarding\n");

    unsigned char read_buffer[131072]; // 128KB read buffer

    struct pollfd fds[2];
    fds[0].fd = m_tcp_fd;
    fds[1].fd = m_usb_fd;

    while (!should_exit) {
        // Determine what we want to poll for
        fds[0].events = 0;
        fds[1].events = 0;

        // We want to read from TCP if buffer has space
        if (m_tcp_to_usb_buffer->space() > 0) {
            fds[0].events |= POLLIN;
        }

        // We want to write to TCP if buffer has data
        if (m_usb_to_tcp_buffer->available() > 0) {
            fds[0].events |= POLLOUT;
        }

        // We want to read from USB if buffer has space
        if (m_usb_to_tcp_buffer->space() > 0) {
            fds[1].events |= POLLIN;
        }

        // We want to write to USB if buffer has data
        if (m_tcp_to_usb_buffer->available() > 0) {
            fds[1].events |= POLLOUT;
        }

        // Poll with 1 second timeout
        int poll_result = poll(fds, 2, 1000);

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue; // Signal interrupted, check should_exit
            }
            Logger::instance()->error("poll() failed: %s\n", strerror(errno));
            break;
        }

        if (poll_result == 0) {
            // Timeout - check for shutdown
            continue;
        }

        // Check for errors
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            Logger::instance()->warn("TCP connection error/hangup\n");
            break;
        }
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            Logger::instance()->warn("USB connection error/hangup\n");
            break;
        }

        // Read from TCP
        if (fds[0].revents & POLLIN) {
            ssize_t n = readMessage(m_tcp_fd, read_buffer, sizeof(read_buffer));
            if (n > 0) {
                size_t written = m_tcp_to_usb_buffer->write(read_buffer, n);
                m_bytes_tcp_to_usb += written;
                if (m_log_communication) {
                    Logger::instance()->debug("Read %zd bytes from TCP, buffered %zu\n", n, written);
                }
            } else if (n == 0) {
                Logger::instance()->info("TCP connection closed\n");
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                Logger::instance()->error("Read from TCP failed: %s\n", strerror(errno));
                break;
            }
        }

        // Write to TCP
        if (fds[0].revents & POLLOUT) {
            size_t available = m_usb_to_tcp_buffer->available();
            if (available > 0) {
                size_t to_write = (available < sizeof(read_buffer)) ? available : sizeof(read_buffer);
                size_t read_count = m_usb_to_tcp_buffer->read(read_buffer, to_write);

                ssize_t n = write(m_tcp_fd, read_buffer, read_count);
                if (n > 0) {
                    if ((size_t)n < read_count) {
                        // Partial write - put back unwritten data
                        m_usb_to_tcp_buffer->write(read_buffer + n, read_count - n);
                    }
                    if (m_log_communication) {
                        Logger::instance()->debug("Wrote %zd bytes to TCP\n", n);
                    }
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    Logger::instance()->error("Write to TCP failed: %s\n", strerror(errno));
                    // Put data back
                    m_usb_to_tcp_buffer->write(read_buffer, read_count);
                    break;
                } else {
                    // EAGAIN - put data back
                    m_usb_to_tcp_buffer->write(read_buffer, read_count);
                }
            }
        }

        // Read from USB
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(m_usb_fd, read_buffer, sizeof(read_buffer));
            if (n > 0) {
                size_t written = m_usb_to_tcp_buffer->write(read_buffer, n);
                m_bytes_usb_to_tcp += written;
                if (m_log_communication) {
                    Logger::instance()->debug("Read %zd bytes from USB, buffered %zu\n", n, written);
                }
            } else if (n == 0) {
                Logger::instance()->info("USB connection closed\n");
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                Logger::instance()->error("Read from USB failed: %s\n", strerror(errno));
                break;
            }
        }

        // Write to USB
        if (fds[1].revents & POLLOUT) {
            size_t available = m_tcp_to_usb_buffer->available();
            if (available > 0) {
                size_t to_write = (available < sizeof(read_buffer)) ? available : sizeof(read_buffer);
                size_t read_count = m_tcp_to_usb_buffer->read(read_buffer, to_write);

                ssize_t n = write(m_usb_fd, read_buffer, read_count);
                if (n > 0) {
                    if ((size_t)n < read_count) {
                        // Partial write - put back unwritten data
                        m_tcp_to_usb_buffer->write(read_buffer + n, read_count - n);
                    }
                    if (m_log_communication) {
                        Logger::instance()->debug("Wrote %zd bytes to USB\n", n);
                    }
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    Logger::instance()->error("Write to USB failed: %s\n", strerror(errno));
                    // Put data back
                    m_tcp_to_usb_buffer->write(read_buffer, read_count);
                    break;
                } else {
                    // EAGAIN - put data back
                    m_tcp_to_usb_buffer->write(read_buffer, read_count);
                }
            }
        }
    }

    Logger::instance()->info("Async I/O forwarding stopped. TCP->USB: %lu bytes, USB->TCP: %lu bytes\n",
                             m_bytes_tcp_to_usb.load(), m_bytes_usb_to_tcp.load());
}

void AAWProxy::stopForwarding(std::atomic<bool>& should_exit) {
    Logger::instance()->info("Stopping forwarding\n");
    should_exit = true;

    if (m_forward_thread && m_forward_thread->joinable()) {
        m_forward_thread->join();
        m_forward_thread = std::nullopt;
    }
}

void AAWProxy::handleClient(int server_sock) {
    struct sockaddr client_address;
    socklen_t client_addresslen = sizeof(client_address);
    if ((m_tcp_fd = accept(server_sock, &client_address, &client_addresslen)) < 0) {
        close(server_sock);
        Logger::instance()->error("accept failed: %s\n", strerror(errno));
        return;
    }

    close(server_sock);

    Logger::instance()->info("TCP server accepted connection\n");

    // Phone connected via TCP, we can stop retrying bluetooth connection
    BluetoothHandler::instance().stopConnectWithRetry();

    if (Config::instance()->getConnectionStrategy() != ConnectionStrategy::USB_FIRST) {
        if (!UsbManager::instance().enableDefaultAndWaitForAccessory(std::chrono::seconds(30))) {
            close(m_tcp_fd);
            m_tcp_fd = -1;
            return;
        }
    }

    Logger::instance()->info("Opening usb accessory\n");
    if ((m_usb_fd = open("/dev/usb_accessory", O_RDWR)) < 0) {
        Logger::instance()->error("error opening /dev/usb_accessory: %s\n", strerror(errno));
        close(m_tcp_fd);
        m_tcp_fd = -1;
        return;
    }

    // Set both file descriptors to non-blocking mode
    if (!setNonBlocking(m_tcp_fd)) {
        Logger::instance()->error("Failed to set TCP socket to non-blocking\n");
        close(m_usb_fd);
        close(m_tcp_fd);
        m_usb_fd = -1;
        m_tcp_fd = -1;
        return;
    }

    if (!setNonBlocking(m_usb_fd)) {
        Logger::instance()->error("Failed to set USB FD to non-blocking\n");
        close(m_usb_fd);
        close(m_tcp_fd);
        m_usb_fd = -1;
        m_tcp_fd = -1;
        return;
    }

    // Increase TCP socket buffer sizes for better streaming performance
    int tcp_buffer_size = 262144; // 256KB
    if (setsockopt(m_tcp_fd, SOL_SOCKET, SO_RCVBUF, &tcp_buffer_size, sizeof(tcp_buffer_size))) {
        Logger::instance()->warn("setsockopt SO_RCVBUF failed: %s\n", strerror(errno));
    }
    if (setsockopt(m_tcp_fd, SOL_SOCKET, SO_SNDBUF, &tcp_buffer_size, sizeof(tcp_buffer_size))) {
        Logger::instance()->warn("setsockopt SO_SNDBUF failed: %s\n", strerror(errno));
    }

    // Enable TCP keepalive
    int keepalive = 1;
    if (setsockopt(m_tcp_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive))) {
        Logger::instance()->warn("setsockopt SO_KEEPALIVE failed: %s\n", strerror(errno));
    }

    // Enable TCP_NODELAY (disable Nagle's algorithm) for lower latency
    // This is crucial for real-time audio/video streaming
    int nodelay = 1;
    if (setsockopt(m_tcp_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay))) {
        Logger::instance()->warn("setsockopt TCP_NODELAY failed: %s\n", strerror(errno));
    } else {
        Logger::instance()->info("TCP_NODELAY enabled for low-latency streaming\n");
    }

    // Set high priority for this socket in the kernel
    // Priority 6 is for interactive traffic (audio/video)
    int priority = 6;
    if (setsockopt(m_tcp_fd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority))) {
        Logger::instance()->warn("setsockopt SO_PRIORITY failed: %s\n", strerror(errno));
    } else {
        Logger::instance()->info("Socket priority set to %d for better QoS\n", priority);
    }

    Logger::instance()->info("Starting async I/O between TCP and USB\n");

    // Clear buffers
    m_tcp_to_usb_buffer->clear();
    m_usb_to_tcp_buffer->clear();
    m_bytes_tcp_to_usb = 0;
    m_bytes_usb_to_tcp = 0;

    std::atomic<bool> should_exit = false;
    m_forward_thread = std::thread(&AAWProxy::forwardAsync, this, std::ref(should_exit));

    if (m_forward_thread && m_forward_thread->joinable()) {
        m_forward_thread->join();
        m_forward_thread = std::nullopt;
    }

    close(m_usb_fd);
    m_usb_fd = -1;

    close(m_tcp_fd);
    m_tcp_fd = -1;

    Logger::instance()->info("Client handling completed\n");
}

std::optional<std::thread> AAWProxy::startServer(int32_t port) {
    Logger::instance()->info("Starting tcp server on port %d\n", port);
    int server_sock;
    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        Logger::instance()->error("creating socket failed: %s\n", strerror(errno));
        return std::nullopt;
    }

    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        Logger::instance()->error("setsockopt failed: %s\n", strerror(errno));
        close(server_sock);
        return std::nullopt;
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_sock, (struct sockaddr*)&address, sizeof(address)) < 0) {
        Logger::instance()->error("bind failed: %s\n", strerror(errno));
        close(server_sock);
        return std::nullopt;
    }

    if (listen(server_sock, 3) < 0) {
        Logger::instance()->error("listen failed: %s\n", strerror(errno));
        close(server_sock);
        return std::nullopt;
    }

    Logger::instance()->info("TCP server listening on port %d\n", port);

    return std::thread(&AAWProxy::handleClient, this, server_sock);
}
