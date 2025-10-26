#pragma once

#include <string>
#include <cstdint>
#include <optional>

enum SecurityMode: int;
enum AccessPointType: int;

struct WifiInfo {
    std::string ssid;
    std::string key;
    std::string bssid;
    SecurityMode securityMode;
    AccessPointType accessPointType;
    std::string ipAddress;
    int32_t port;
};

enum class ConnectionStrategy {
    DONGLE_MODE = 0,
    PHONE_FIRST = 1,
    USB_FIRST = 2
};

class Config {
public:
    static Config* instance();

    WifiInfo getWifiInfo();
    ConnectionStrategy getConnectionStrategy();

    std::string getUniqueSuffix();

    bool validateConfig();
    bool isValidIPAddress(const std::string& ip);
    bool isValidSSID(const std::string& ssid);
    bool isValidPassword(const std::string& password, SecurityMode mode);

private:
    Config() = default;

    int32_t getenv(std::string name, int32_t defaultValue);
    std::string getenv(std::string name, std::string defaultValue);

    std::string getMacAddress(std::string interface);

    std::optional<ConnectionStrategy> connectionStrategy;
};

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

class Logger {
public:
    static Logger* instance();

    void debug(const char *format, ...);
    void info(const char *format, ...);
    void warn(const char *format, ...);
    void error(const char *format, ...);

    void setLogLevel(LogLevel level);
    LogLevel getLogLevel() const;
private:
    Logger();
    ~Logger();

    void log(int priority, const char *format, va_list args);
    LogLevel m_logLevel;
};