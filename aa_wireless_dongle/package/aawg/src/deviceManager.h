#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <map>

struct PairedDevice {
    std::string mac_address;
    std::string name;
    std::chrono::system_clock::time_point last_connected;
    uint32_t connection_count;
    uint32_t failure_count;
    bool is_trusted;

    PairedDevice()
        : connection_count(0), failure_count(0), is_trusted(false) {}
};

class DeviceManager {
public:
    static DeviceManager* instance();

    void recordConnection(const std::string& mac_address, const std::string& name);
    void recordDisconnection(const std::string& mac_address);
    void recordConnectionFailure(const std::string& mac_address);

    std::vector<std::string> getDevicesByPriority();
    bool isDeviceTrusted(const std::string& mac_address);
    void setDeviceTrusted(const std::string& mac_address, bool trusted);

    bool loadFromFile(const std::string& path);
    bool saveToFile(const std::string& path);

    void logDevices();

private:
    DeviceManager();

    std::map<std::string, PairedDevice> m_devices;
    std::string m_persistence_file;
};
