#include <stdio.h>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include "common.h"
#include "bluetoothHandler.h"
#include "bluetoothProfiles.h"

#include <google/protobuf/message_lite.h>
#include "proto/WifiStartRequest.pb.h"
#include "proto/WifiInfoResponse.pb.h"
#include "proto/WifiVersionRequest.pb.h"
#include "proto/WifiVersionResponse.pb.h"
#include "proto/WifiConnectStatus.pb.h"
#include "proto/WifiStartResponse.pb.h"

static constexpr const char* INTERFACE_BLUEZ_PROFILE = "org.bluez.Profile1";


#pragma region BluezProfile
BluezProfile::BluezProfile(DBus::Path path): DBus::Object(path) {
    this->create_method<void(void)>(INTERFACE_BLUEZ_PROFILE, "Release", sigc::mem_fun(*this, &BluezProfile::Release));
    this->create_method<void(DBus::Path, std::shared_ptr<DBus::FileDescriptor>, DBus::Properties)>(INTERFACE_BLUEZ_PROFILE ,"NewConnection", sigc::mem_fun(*this, &BluezProfile::NewConnection));
    this->create_method<void(DBus::Path)>(INTERFACE_BLUEZ_PROFILE, "RequestDisconnection", sigc::mem_fun(*this, &BluezProfile::RequestDisconnection));
}
#pragma endregion BluezProfile


#pragma region AAWirelessLauncher
class AAWirelessLauncher {
public:
    AAWirelessLauncher(int fd): m_fd(fd), m_state(State::INITIAL) {};

    bool launch() {
        // Make fd blocking
        int fd_flags = fcntl(m_fd, F_GETFL);
        fcntl(m_fd, F_SETFL, fd_flags & ~O_NONBLOCK);

        // Set read timeout
        struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
        if (setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            Logger::instance()->error("Failed to set socket timeout: %s\n", strerror(errno));
            return false;
        }

        WifiInfo wifiInfo = Config::instance()->getWifiInfo();

        // State 1: Send WifiStartRequest
        if (!transitionTo(State::SENDING_START_REQUEST)) return false;

        Logger::instance()->info("Sending WifiStartRequest (ip: %s, port: %d)\n",
                                 wifiInfo.ipAddress.c_str(), wifiInfo.port);
        WifiStartRequest wifiStartRequest;
        wifiStartRequest.set_ip_address(wifiInfo.ipAddress);
        wifiStartRequest.set_port(wifiInfo.port);

        if (!SendMessage(MessageId::WifiStartRequest, &wifiStartRequest)) {
            Logger::instance()->error("Failed to send WifiStartRequest\n");
            return false;
        }

        // State 2: Wait for WifiInfoRequest or WifiVersionRequest
        if (!transitionTo(State::WAITING_FOR_REQUEST)) return false;

        MessageId messageId = ReadMessage();

        // Handle version request if phone sends it
        if (messageId == MessageId::WifiVersionRequest) {
            if (!handleVersionRequest()) return false;
            messageId = ReadMessage(); // Read next message
        }

        if (messageId != MessageId::WifiInfoRequest) {
            Logger::instance()->error("Expected WifiInfoRequest, got %s (%d)\n",
                                     MessageName(messageId).c_str(), messageId);
            return false;
        }

        // State 3: Send WifiInfoResponse
        if (!transitionTo(State::SENDING_WIFI_INFO)) return false;

        Logger::instance()->info("Sending WifiInfoResponse (ssid: %s, bssid: %s)\n",
                                 wifiInfo.ssid.c_str(), wifiInfo.bssid.c_str());
        WifiInfoResponse wifiInfoResponse;
        wifiInfoResponse.set_ssid(wifiInfo.ssid);
        wifiInfoResponse.set_key(wifiInfo.key);
        wifiInfoResponse.set_bssid(wifiInfo.bssid);
        wifiInfoResponse.set_security_mode(wifiInfo.securityMode);
        wifiInfoResponse.set_access_point_type(wifiInfo.accessPointType);

        if (!SendMessage(MessageId::WifiInfoResponse, &wifiInfoResponse)) {
            Logger::instance()->error("Failed to send WifiInfoResponse\n");
            return false;
        }

        // State 4: Wait for optional responses
        if (!transitionTo(State::WAITING_FOR_COMPLETION)) return false;

        // Phone may send WifiConnectStatus and/or WifiStartResponse
        int max_additional_messages = 3;
        for (int i = 0; i < max_additional_messages; i++) {
            messageId = ReadMessage();

            if (messageId == MessageId::Invalid) {
                // Timeout or connection closed - this is okay
                break;
            }

            if (messageId == MessageId::WifiConnectStatus) {
                Logger::instance()->info("Received WifiConnectStatus\n");
            } else if (messageId == MessageId::WifiStartResponse) {
                Logger::instance()->info("Received WifiStartResponse\n");
            } else {
                Logger::instance()->warn("Unexpected message: %s\n", MessageName(messageId).c_str());
            }
        }

        if (!transitionTo(State::COMPLETED)) return false;
        Logger::instance()->info("Bluetooth handshake completed successfully\n");
        return true;
    }

private:
    enum class State {
        INITIAL,
        SENDING_START_REQUEST,
        WAITING_FOR_REQUEST,
        SENDING_WIFI_INFO,
        WAITING_FOR_COMPLETION,
        COMPLETED,
        ERROR
    };

    enum class MessageId {
        Invalid = -1,
        WifiStartRequest = 1,
        WifiInfoRequest = 2,
        WifiInfoResponse = 3,
        WifiVersionRequest = 4,
        WifiVersionResponse = 5,
        WifiConnectStatus = 6,
        WifiStartResponse = 7,
    };

    bool transitionTo(State newState) {
        Logger::instance()->debug("State transition: %d -> %d\n", static_cast<int>(m_state), static_cast<int>(newState));
        m_state = newState;
        return true;
    }

    bool handleVersionRequest() {
        Logger::instance()->info("Handling WifiVersionRequest\n");

        WifiVersionResponse versionResp;
        versionResp.set_version_major(1);
        versionResp.set_version_minor(0);
        versionResp.set_version_patch(0);

        if (!SendMessage(MessageId::WifiVersionResponse, &versionResp)) {
            Logger::instance()->error("Failed to send WifiVersionResponse\n");
            return false;
        }

        return true;
    }

    std::string MessageName(MessageId messageId) {
        switch (messageId) {
            case MessageId::WifiStartRequest:
                return "WifiStartRequest";
            case MessageId::WifiInfoRequest:
                return "WifiInfoRequest";
            case MessageId::WifiInfoResponse:
                return "WifiInfoResponse";
            case MessageId::WifiVersionRequest:
                return "WifiVersionRequest";
            case MessageId::WifiVersionResponse:
                return "WifiVersionResponse";
            case MessageId::WifiConnectStatus:
                return "WifiConnectStatus";
            case MessageId::WifiStartResponse:
                return "WifiStartResponse";
            default:
                return "UNKNOWN";
        }
    }

    bool SendMessage(MessageId messageId, google::protobuf::MessageLite* message) {
        uint16_t messageSize = (uint16_t)message->ByteSizeLong();
        uint16_t length = messageSize + 4;

        if (length > 65535) {
            Logger::instance()->error("Message too large: %d bytes\n", length);
            return false;
        }

        unsigned char* buffer = new unsigned char[length];

        uint16_t networkShort = 0;
        networkShort = htons(messageSize);
        memcpy(buffer, &networkShort, sizeof(networkShort));

        networkShort = htons(static_cast<uint16_t>(messageId));
        memcpy(buffer + 2, &networkShort, sizeof(networkShort));

        if (!message->SerializeToArray(buffer + 4, messageSize)) {
            Logger::instance()->error("Failed to serialize %s\n", MessageName(messageId).c_str());
            delete[] buffer;
            return false;
        }

        ssize_t wrote = write(m_fd, buffer, length);
        delete[] buffer;

        if (wrote < 0) {
            Logger::instance()->error("Error sending %s: %s\n", MessageName(messageId).c_str(), strerror(errno));
            return false;
        }
        else if ((size_t)wrote != length) {
            Logger::instance()->error("Partial write for %s: %zd/%d bytes\n", MessageName(messageId).c_str(), wrote, length);
            return false;
        }

        Logger::instance()->info("Sent %s (%d bytes)\n", MessageName(messageId).c_str(), wrote);
        return true;
    }

    MessageId ReadMessage() {
        uint16_t networkShort = 0;
        ssize_t readBytes;

        // Read length
        readBytes = read(m_fd, &networkShort, 2);
        if (readBytes == 0) {
            Logger::instance()->info("Connection closed by peer\n");
            return MessageId::Invalid;
        }
        if (readBytes != 2) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                Logger::instance()->warn("Read timeout\n");
            } else {
                Logger::instance()->error("Error reading length: %s\n", strerror(errno));
            }
            return MessageId::Invalid;
        }
        uint16_t length = ntohs(networkShort);

        if (length > 32768) {  // Sanity check - messages shouldn't be > 32KB
            Logger::instance()->error("Invalid message length: %d bytes\n", length);
            return MessageId::Invalid;
        }

        // Read message ID
        readBytes = read(m_fd, &networkShort, 2);
        if (readBytes != 2) {
            Logger::instance()->error("Error reading message ID: %s\n", strerror(errno));
            return MessageId::Invalid;
        }
        MessageId messageId = static_cast<MessageId>(ntohs(networkShort));

        // Validate message ID
        if (messageId < MessageId::WifiStartRequest || messageId > MessageId::WifiStartResponse) {
            Logger::instance()->error("Invalid message ID: %d\n", static_cast<int>(messageId));
            return MessageId::Invalid;
        }

        Logger::instance()->info("Read %s (length: %d bytes)\n", MessageName(messageId).c_str(), length);

        // Read payload (even if we don't parse it, we must consume it)
        if (length > 0) {
            unsigned char* buffer = new unsigned char[length];
            ssize_t total_read = 0;

            while (total_read < length) {
                readBytes = read(m_fd, buffer + total_read, length - total_read);
                if (readBytes <= 0) {
                    Logger::instance()->error("Error reading payload: %s\n", strerror(errno));
                    delete[] buffer;
                    return MessageId::Invalid;
                }
                total_read += readBytes;
            }

            delete[] buffer;
        }

        return messageId;
    }

    int m_fd;
    State m_state;
};
#pragma endregion AAWirelessLauncher

#pragma region AAWirelessProfile
void AAWirelessProfile::Release() {
    Logger::instance()->info("AA Wireless Release\n");
}

void AAWirelessProfile::NewConnection(DBus::Path path, std::shared_ptr<DBus::FileDescriptor> fd, DBus::Properties fdProperties) {
    Logger::instance()->info("AA Wireless NewConnection\n");
    Logger::instance()->info("Path: %s, fd: %d\n", path.c_str(), fd->descriptor());

    if (!AAWirelessLauncher(fd->descriptor()).launch()) {
        Logger::instance()->error("Bluetooth launch sequence failed\n");
        return;
    }

    Logger::instance()->info("Bluetooth launch sequence completed successfully\n");
}

void AAWirelessProfile::RequestDisconnection(DBus::Path path) {
    Logger::instance()->info("AA Wireless RequestDisconnection\n");
    Logger::instance()->info("Path: %s\n", path.c_str());
}

AAWirelessProfile::AAWirelessProfile(DBus::Path path): BluezProfile(path) {};

/* static */ std::shared_ptr<AAWirelessProfile> AAWirelessProfile::create(DBus::Path path) {
    return std::shared_ptr<AAWirelessProfile>(new AAWirelessProfile(path));
}
#pragma endregion AAWirelessProfile

#pragma region HSPHSProfile
void HSPHSProfile::Release() {
    Logger::instance()->info("HSP HS Release\n");
}

void HSPHSProfile::NewConnection(DBus::Path path, std::shared_ptr<DBus::FileDescriptor> fd, DBus::Properties fdProperties) {
    Logger::instance()->info("HSP HS NewConnection\n");
    Logger::instance()->info("Path: %s, fd: %d\n", path.c_str(), fd->descriptor());
}

void HSPHSProfile::RequestDisconnection(DBus::Path path) {
    Logger::instance()->info("HSP HS RequestDisconnection\n");
    Logger::instance()->info("Path: %s\n", path.c_str());
}

HSPHSProfile::HSPHSProfile(DBus::Path path): BluezProfile(path) {};

/* static */ std::shared_ptr<HSPHSProfile> HSPHSProfile::create(DBus::Path path) {
    return std::shared_ptr<HSPHSProfile>(new HSPHSProfile(path));
}
#pragma endregion HSPHSProfile
