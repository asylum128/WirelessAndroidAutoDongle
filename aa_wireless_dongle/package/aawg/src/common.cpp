#include <cstdlib>
#include <cstdarg>
#include <sstream>
#include <fstream>
#include <syslog.h>

#include "common.h"
#include "proto/WifiInfoResponse.pb.h"

#pragma region Config
/*static*/ Config* Config::instance() {
    static Config s_instance;
    return &s_instance;
}

int32_t Config::getenv(std::string name, int32_t defaultValue) {
    char* envValue = std::getenv(name.c_str());
    try {
        return envValue != nullptr ? std::stoi(envValue) : defaultValue;
    }
    catch(...) {
        return defaultValue;
    }
}

std::string Config::getenv(std::string name, std::string defaultValue) {
    char* envValue = std::getenv(name.c_str());
    return envValue != nullptr ? envValue : defaultValue;
}

std::string Config::getMacAddress(std::string interface) {
    std::ifstream addressFile("/sys/class/net/" + interface + "/address");

    std::string macAddress;
    getline(addressFile, macAddress);

    return macAddress;
}

std::string Config::getUniqueSuffix() {
    std::string uniqueSuffix = getenv("AAWG_UNIQUE_NAME_SUFFIX", "");
    if (!uniqueSuffix.empty()) {
        return uniqueSuffix;
    }

    std::ifstream serialNumberFile("/sys/firmware/devicetree/base/serial-number");

    std::string serialNumber;
    getline(serialNumberFile, serialNumber);

    // Removing trailing null from serialNumber, pad at the beginning
    serialNumber = std::string("00000000") + serialNumber.c_str();

    return serialNumber.substr(serialNumber.size() - 6);
}

WifiInfo Config::getWifiInfo() {
    return {
        getenv("AAWG_WIFI_SSID", "AAWirelessDongle"),
        getenv("AAWG_WIFI_PASSWORD", "ConnectAAWirelessDongle"),
        getenv("AAWG_WIFI_BSSID", getMacAddress("wlan0")),
        SecurityMode::WPA2_PERSONAL,
        AccessPointType::DYNAMIC,
        getenv("AAWG_PROXY_IP_ADDRESS", "10.0.0.1"),
        getenv("AAWG_PROXY_PORT", 5288),
    };
}

ConnectionStrategy Config::getConnectionStrategy() {
    if (!connectionStrategy.has_value()) {
        const int32_t connectionStrategyEnv = getenv("AAWG_CONNECTION_STRATEGY", 1);

        switch (connectionStrategyEnv) {
            case 0:
                connectionStrategy = ConnectionStrategy::DONGLE_MODE;
                break;
            case 1:
                connectionStrategy = ConnectionStrategy::PHONE_FIRST;
                break;
            case 2:
                connectionStrategy = ConnectionStrategy::USB_FIRST;
                break;
            default:
                connectionStrategy = ConnectionStrategy::PHONE_FIRST;
                break;
        }
    }

    return connectionStrategy.value();
}

bool Config::isValidIPAddress(const std::string& ip) {
    int a, b, c, d;
    if (sscanf(ip.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        return false;
    }

    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) {
        return false;
    }

    return true;
}

bool Config::isValidSSID(const std::string& ssid) {
    if (ssid.empty()) {
        return false;
    }

    if (ssid.length() > 32) {
        return false;
    }

    return true;
}

bool Config::isValidPassword(const std::string& password, SecurityMode mode) {
    if (mode == SecurityMode::OPEN) {
        return password.empty();
    }

    if (mode == SecurityMode::WPA2_PERSONAL || mode == SecurityMode::WPA_PERSONAL) {
        if (password.length() < 8 || password.length() > 63) {
            return false;
        }
    }

    if (mode == SecurityMode::WEP_64) {
        if (password.length() != 5 && password.length() != 10) {
            return false;
        }
    }

    if (mode == SecurityMode::WEP_128) {
        if (password.length() != 13 && password.length() != 26) {
            return false;
        }
    }

    return true;
}

bool Config::validateConfig() {
    WifiInfo wifi = getWifiInfo();
    bool valid = true;

    // Validate SSID
    if (!isValidSSID(wifi.ssid)) {
        Logger::instance()->error("Invalid SSID: '%s' (must be 1-32 characters)\n", wifi.ssid.c_str());
        valid = false;
    }

    // Validate password for security mode
    if (!isValidPassword(wifi.key, wifi.securityMode)) {
        Logger::instance()->error("Invalid password for security mode %d\n", static_cast<int>(wifi.securityMode));
        if (wifi.securityMode == SecurityMode::WPA2_PERSONAL || wifi.securityMode == SecurityMode::WPA_PERSONAL) {
            Logger::instance()->error("WPA/WPA2 password must be 8-63 characters\n");
        }
        valid = false;
    }

    // Validate IP address
    if (!isValidIPAddress(wifi.ipAddress)) {
        Logger::instance()->error("Invalid IP address: '%s'\n", wifi.ipAddress.c_str());
        valid = false;
    }

    // Validate port
    if (wifi.port < 1 || wifi.port > 65535) {
        Logger::instance()->error("Invalid port: %d (must be 1-65535)\n", wifi.port);
        valid = false;
    }

    // Validate BSSID format (if not empty)
    if (!wifi.bssid.empty()) {
        int mac[6];
        if (sscanf(wifi.bssid.c_str(), "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
            Logger::instance()->error("Invalid BSSID format: '%s' (expected XX:XX:XX:XX:XX:XX)\n", wifi.bssid.c_str());
            valid = false;
        }
    }

    if (valid) {
        Logger::instance()->info("Configuration validation passed\n");
    } else {
        Logger::instance()->error("Configuration validation failed\n");
    }

    return valid;
}
#pragma endregion Config

#pragma region Logger
/*static*/ Logger* Logger::instance() {
    static Logger s_instance;
    return &s_instance;
}

Logger::Logger() : m_logLevel(LogLevel::INFO) {
    openlog("aawgd", LOG_PERROR | LOG_PID | LOG_CONS, LOG_DAEMON);

    // Set log level from environment variable
    const char* logLevelEnv = std::getenv("AAWG_LOG_LEVEL");
    if (logLevelEnv != nullptr) {
        std::string level(logLevelEnv);
        if (level == "DEBUG") m_logLevel = LogLevel::DEBUG;
        else if (level == "INFO") m_logLevel = LogLevel::INFO;
        else if (level == "WARN") m_logLevel = LogLevel::WARN;
        else if (level == "ERROR") m_logLevel = LogLevel::ERROR;
    }
}

Logger::~Logger() {
    closelog();
}

void Logger::log(int priority, const char *format, va_list args) {
    vsyslog(priority, format, args);
}

void Logger::debug(const char *format, ...) {
    if (m_logLevel > LogLevel::DEBUG) return;
    va_list args;
    va_start(args, format);
    log(LOG_DEBUG, format, args);
    va_end(args);
}

void Logger::info(const char *format, ...) {
    if (m_logLevel > LogLevel::INFO) return;
    va_list args;
    va_start(args, format);
    log(LOG_INFO, format, args);
    va_end(args);
}

void Logger::warn(const char *format, ...) {
    if (m_logLevel > LogLevel::WARN) return;
    va_list args;
    va_start(args, format);
    log(LOG_WARNING, format, args);
    va_end(args);
}

void Logger::error(const char *format, ...) {
    if (m_logLevel > LogLevel::ERROR) return;
    va_list args;
    va_start(args, format);
    log(LOG_ERR, format, args);
    va_end(args);
}

void Logger::setLogLevel(LogLevel level) {
    m_logLevel = level;
}

LogLevel Logger::getLogLevel() const {
    return m_logLevel;
}
#pragma endregion Logger