#include "deviceManager.h"
#include "common.h"
#include <fstream>
#include <algorithm>
#include <ctime>

/*static*/ DeviceManager* DeviceManager::instance() {
    static DeviceManager s_instance;
    return &s_instance;
}

DeviceManager::DeviceManager() {
    m_persistence_file = "/var/lib/aawgd/paired_devices.conf";
    loadFromFile(m_persistence_file);
}

void DeviceManager::recordConnection(const std::string& mac_address, const std::string& name) {
    auto& device = m_devices[mac_address];
    device.mac_address = mac_address;
    device.name = name;
    device.last_connected = std::chrono::system_clock::now();
    device.connection_count++;

    Logger::instance()->info("Device connected: %s (%s), total connections: %u\n",
                             name.c_str(), mac_address.c_str(), device.connection_count);

    saveToFile(m_persistence_file);
}

void DeviceManager::recordDisconnection(const std::string& mac_address) {
    if (m_devices.find(mac_address) == m_devices.end()) {
        return;
    }

    Logger::instance()->info("Device disconnected: %s\n", mac_address.c_str());
}

void DeviceManager::recordConnectionFailure(const std::string& mac_address) {
    if (m_devices.find(mac_address) == m_devices.end()) {
        return;
    }

    auto& device = m_devices[mac_address];
    device.failure_count++;

    Logger::instance()->warn("Device connection failure: %s, total failures: %u\n",
                             mac_address.c_str(), device.failure_count);

    saveToFile(m_persistence_file);
}

std::vector<std::string> DeviceManager::getDevicesByPriority() {
    std::vector<std::pair<std::string, PairedDevice>> device_list;

    for (const auto& pair : m_devices) {
        device_list.push_back(pair);
    }

    // Sort by priority:
    // 1. Trusted devices first
    // 2. Most recently connected
    // 3. Highest connection count
    // 4. Lowest failure count
    std::sort(device_list.begin(), device_list.end(),
        [](const auto& a, const auto& b) {
            // Trusted devices first
            if (a.second.is_trusted != b.second.is_trusted) {
                return a.second.is_trusted > b.second.is_trusted;
            }

            // Then by most recent connection
            if (a.second.last_connected != b.second.last_connected) {
                return a.second.last_connected > b.second.last_connected;
            }

            // Then by connection count
            if (a.second.connection_count != b.second.connection_count) {
                return a.second.connection_count > b.second.connection_count;
            }

            // Finally by lowest failure count
            return a.second.failure_count < b.second.failure_count;
        });

    std::vector<std::string> result;
    for (const auto& pair : device_list) {
        result.push_back(pair.first);
    }

    return result;
}

bool DeviceManager::isDeviceTrusted(const std::string& mac_address) {
    auto it = m_devices.find(mac_address);
    return (it != m_devices.end()) && it->second.is_trusted;
}

void DeviceManager::setDeviceTrusted(const std::string& mac_address, bool trusted) {
    auto& device = m_devices[mac_address];
    device.mac_address = mac_address;
    device.is_trusted = trusted;

    Logger::instance()->info("Device %s trust status: %s\n",
                             mac_address.c_str(), trusted ? "TRUSTED" : "UNTRUSTED");

    saveToFile(m_persistence_file);
}

bool DeviceManager::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::instance()->debug("No paired devices file found at %s\n", path.c_str());
        return false;
    }

    m_devices.clear();

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        PairedDevice device;
        unsigned long long timestamp;
        int trusted;

        if (sscanf(line.c_str(), "%*[^=]=%[^,],%[^,],%llu,%u,%u,%d",
                   device.mac_address.data(), device.name.data(),
                   &timestamp, &device.connection_count,
                   &device.failure_count, &trusted) >= 3) {

            device.last_connected = std::chrono::system_clock::time_point(
                std::chrono::seconds(timestamp));
            device.is_trusted = (trusted != 0);

            m_devices[device.mac_address] = device;
        }
    }

    Logger::instance()->info("Loaded %zu paired devices from %s\n", m_devices.size(), path.c_str());
    return true;
}

bool DeviceManager::saveToFile(const std::string& path) {
    std::ofstream file(path);
    if (!file.is_open()) {
        Logger::instance()->warn("Failed to save paired devices to %s\n", path.c_str());
        return false;
    }

    file << "# AA Wireless Dongle Paired Devices\n";
    file << "# Format: device=MAC,NAME,TIMESTAMP,CONNECTIONS,FAILURES,TRUSTED\n";

    for (const auto& pair : m_devices) {
        const auto& device = pair.second;
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            device.last_connected.time_since_epoch()).count();

        file << "device=" << device.mac_address << ","
             << device.name << ","
             << timestamp << ","
             << device.connection_count << ","
             << device.failure_count << ","
             << (device.is_trusted ? 1 : 0) << "\n";
    }

    return true;
}

void DeviceManager::logDevices() {
    Logger::instance()->info("=== Paired Devices (%zu) ===\n", m_devices.size());

    auto sorted = getDevicesByPriority();
    for (const auto& mac : sorted) {
        const auto& device = m_devices[mac];
        Logger::instance()->info("  %s (%s): %u connections, %u failures, %s\n",
                                 device.name.c_str(),
                                 device.mac_address.c_str(),
                                 device.connection_count,
                                 device.failure_count,
                                 device.is_trusted ? "TRUSTED" : "untrusted");
    }
    Logger::instance()->info("========================\n");
}
