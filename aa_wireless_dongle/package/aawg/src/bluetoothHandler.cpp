#include <stdio.h>

#include "common.h"
#include "bluetoothHandler.h"
#include "bluetoothProfiles.h"
#include "bluetoothAdvertisement.h"
#include "deviceManager.h"

static constexpr const char* ADAPTER_ALIAS_PREFIX = "WirelessAADongle-";
static constexpr const char* ADAPTER_ALIAS_DONGLE_PREFIX = "AndroidAuto-Dongle-";

static constexpr const char* BLUEZ_BUS_NAME = "org.bluez";
static constexpr const char* BLUEZ_ROOT_OBJECT_PATH = "/";
static constexpr const char* BLUEZ_OBJECT_PATH = "/org/bluez";

static constexpr const char* INTERFACE_BLUEZ_ADAPTER = "org.bluez.Adapter1";
static constexpr const char* INTERFACE_BLUEZ_LE_ADVERTISING_MANAGER = "org.bluez.LEAdvertisingManager1";

static constexpr const char* INTERFACE_BLUEZ_DEVICE = "org.bluez.Device1";
static constexpr const char* INTERFACE_BLUEZ_PROFILE_MANAGER = "org.bluez.ProfileManager1";

static constexpr const char* LE_ADVERTISEMENT_OBJECT_PATH = "/com/aawgd/bluetooth/advertisement";

static constexpr const char* AAWG_PROFILE_OBJECT_PATH = "/com/aawgd/bluetooth/aawg";
static constexpr const char* AAWG_PROFILE_UUID = "4de17a00-52cb-11e6-bdf4-0800200c9a66";

static constexpr const char* HSP_HS_PROFILE_OBJECT_PATH = "/com/aawgd/bluetooth/hsp";
static constexpr const char* HSP_AG_UUID = "00001112-0000-1000-8000-00805f9b34fb";
static constexpr const char* HSP_HS_UUID = "00001108-0000-1000-8000-00805f9b34fb";


class BluezAdapterProxy: private DBus::ObjectProxy {
    BluezAdapterProxy(std::shared_ptr<DBus::Connection> conn, DBus::Path path): DBus::ObjectProxy(conn, BLUEZ_BUS_NAME, path) {
        alias = this->create_property<std::string>(INTERFACE_BLUEZ_ADAPTER, "Alias");
        powered = this->create_property<bool>(INTERFACE_BLUEZ_ADAPTER, "Powered");
        discoverable = this->create_property<bool>(INTERFACE_BLUEZ_ADAPTER, "Discoverable");
        pairable = this->create_property<bool>(INTERFACE_BLUEZ_ADAPTER, "Pairable");

        registerAdvertisement = this->create_method<void(DBus::Path, DBus::Properties)>(INTERFACE_BLUEZ_LE_ADVERTISING_MANAGER, "RegisterAdvertisement");
        unregisterAdvertisement = this->create_method<void(DBus::Path)>(INTERFACE_BLUEZ_LE_ADVERTISING_MANAGER, "UnregisterAdvertisement");
    }

public:
    static std::shared_ptr<BluezAdapterProxy> create(std::shared_ptr<DBus::Connection> conn, DBus::Path path)
    {
      return std::shared_ptr<BluezAdapterProxy>(new BluezAdapterProxy(conn, path));
    }

    std::shared_ptr<DBus::PropertyProxy<std::string>> alias;
    std::shared_ptr<DBus::PropertyProxy<bool>> powered;
    std::shared_ptr<DBus::PropertyProxy<bool>> discoverable;
    std::shared_ptr<DBus::PropertyProxy<bool>> pairable;

    std::shared_ptr<DBus::MethodProxy<void(DBus::Path, DBus::Properties)>> registerAdvertisement;
    std::shared_ptr<DBus::MethodProxy<void(DBus::Path)>> unregisterAdvertisement;
};


BluetoothHandler& BluetoothHandler::instance() {
    static BluetoothHandler instance;
    return instance;
}

DBus::ManagedObjects BluetoothHandler::getBluezObjects() {
    std::shared_ptr<DBus::ObjectProxy> m_bluezRootObject = m_connection->create_object_proxy(BLUEZ_BUS_NAME, BLUEZ_ROOT_OBJECT_PATH);
    DBus::MethodProxy getManagedObjects = *(m_bluezRootObject->create_method<DBus::ManagedObjects(void)>("org.freedesktop.DBus.ObjectManager", "GetManagedObjects"));

    return getManagedObjects();
}

void BluetoothHandler::initAdapter() {
    DBus::ManagedObjects objects = getBluezObjects();

    std::string adapter_path;
    for (auto const& [path, interfaces]: objects) {
        for (auto const& [interface, properties]: interfaces) {
            if (interface == INTERFACE_BLUEZ_ADAPTER) {
                adapter_path = path;
                Logger::instance()->info("Using bluetooth adapter at path: %s\n", path.c_str());
                break;
            }
        }
        if (!adapter_path.empty()) {
            break;
        }
    }

    if (adapter_path.empty()) {
        Logger::instance()->info("Did not find any bluetooth adapters\n");
    }
    else {
        m_adapter = BluezAdapterProxy::create(m_connection, adapter_path);
        m_adapter->alias->set_value(m_adapterAlias);
        Logger::instance()->info("Bluetooth adapter alias: %s\n", m_adapterAlias.c_str());
    }
}

void BluetoothHandler::setPower(bool on) {
    if (!m_adapter) {
        return;
    }

    m_adapter->powered->set_value(on);
    Logger::instance()->info("Bluetooth adapter was powered %s\n", on ? "on" : "off");
}

void BluetoothHandler::setPairable(bool pairable) {
    if (!m_adapter) {
        return;
    }

    m_adapter->discoverable->set_value(pairable);
    m_adapter->pairable->set_value(pairable);
    Logger::instance()->info("Bluetooth adapter is now discoverable and pairable\n");
}

void BluetoothHandler::exportProfiles() {
    std::shared_ptr<DBus::ObjectProxy> bluezObject = m_connection->create_object_proxy(BLUEZ_BUS_NAME, BLUEZ_OBJECT_PATH);
    DBus::MethodProxy registerProfile = *(bluezObject->create_method<void(DBus::Path, std::string, DBus::Properties)>(INTERFACE_BLUEZ_PROFILE_MANAGER, "RegisterProfile"));

    // Register AA Wireless Profile
    m_aawProfile = AAWirelessProfile::create(AAWG_PROFILE_OBJECT_PATH);
    if (m_connection->register_object(m_aawProfile, DBus::ThreadForCalling::DispatcherThread) != DBus::RegistrationStatus::Success) {
        Logger::instance()->info("Failed to register AA Wireless profile\n");
    }

    registerProfile(AAWG_PROFILE_OBJECT_PATH, AAWG_PROFILE_UUID, {
        {"Name", DBus::Variant("AA Wireless")},
        {"Role", DBus::Variant("server")},
        {"Channel", DBus::Variant(uint16_t(8))},
    });
    Logger::instance()->info("Bluetooth AA Wireless profile active\n");

    if (Config::instance()->getConnectionStrategy() != ConnectionStrategy::DONGLE_MODE) {
        // Register HSP Handset profile
        m_hspProfile = HSPHSProfile::create(HSP_HS_PROFILE_OBJECT_PATH);
        if (m_connection->register_object(m_hspProfile, DBus::ThreadForCalling::DispatcherThread) != DBus::RegistrationStatus::Success) {
            Logger::instance()->info("Failed to register HSP Handset profile\n");
        }
        registerProfile(HSP_HS_PROFILE_OBJECT_PATH, HSP_HS_UUID, {
            {"Name", DBus::Variant("HSP HS")},
        });
        Logger::instance()->info("HSP Handset profile active\n");
    }
}

void BluetoothHandler::startAdvertising() {
    if (!m_adapter) {
        return;
    }

    // Register Advertisement Object
    m_leAdvertisement = BLEAdvertisement::create(LE_ADVERTISEMENT_OBJECT_PATH);

    m_leAdvertisement->type->set_value("peripheral");
    m_leAdvertisement->serviceUUIDs->set_value(std::vector<std::string>{AAWG_PROFILE_UUID});
    m_leAdvertisement->localName->set_value(m_adapterAlias);

    if (m_connection->register_object(m_leAdvertisement, DBus::ThreadForCalling::DispatcherThread) != DBus::RegistrationStatus::Success) {
        Logger::instance()->info("Failed to register BLE Advertisement\n");
    }

    (*m_adapter->registerAdvertisement)(LE_ADVERTISEMENT_OBJECT_PATH, {});
    Logger::instance()->info("BLE Advertisement started\n");
}

void BluetoothHandler::stopAdvertising() {
    if (!m_adapter) {
        return;
    }

    (*m_adapter->unregisterAdvertisement)(LE_ADVERTISEMENT_OBJECT_PATH);
    Logger::instance()->info("BLE Advertisement stopped\n");
}

void BluetoothHandler::connectDevice() {
    DBus::ManagedObjects objects = getBluezObjects();

    // Build map of device paths to their properties
    std::map<std::string, std::map<std::string, DBus::Properties>> device_map;
    for (auto const& [path, interfaces]: objects) {
        for (auto const& [interface, properties]: interfaces) {
            if (interface == INTERFACE_BLUEZ_DEVICE) {
                device_map[path] = interfaces;
            }
        }
    }

    if (device_map.empty()) {
        Logger::instance()->info("Did not find any bluetooth devices\n");
        return;
    }

    const bool isDongleMode = (Config::instance()->getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE);

    Logger::instance()->info("Found %zu bluetooth devices\n", device_map.size());

    // Get prioritized list of known devices
    std::vector<std::string> prioritized_macs = DeviceManager::instance()->getDevicesByPriority();

    // Try known devices first, in priority order
    for (const std::string& mac_address : prioritized_macs) {
        Logger::instance()->info("Trying prioritized device: %s\n", mac_address.c_str());

        // Find matching device path
        for (const auto& [path, interfaces] : device_map) {
            std::shared_ptr<DBus::ObjectProxy> bluezDevice = m_connection->create_object_proxy(BLUEZ_BUS_NAME, path);
            std::shared_ptr<DBus::PropertyProxy<std::string>> deviceAddress =
                bluezDevice->create_property<std::string>(INTERFACE_BLUEZ_DEVICE, "Address");

            if (deviceAddress && *deviceAddress == mac_address) {
                if (tryConnectToDevice(path, isDongleMode)) {
                    return;
                }
            }
        }
    }

    // Try remaining devices that aren't in our known list
    for (const auto& [path, interfaces] : device_map) {
        std::shared_ptr<DBus::ObjectProxy> bluezDevice = m_connection->create_object_proxy(BLUEZ_BUS_NAME, path);
        std::shared_ptr<DBus::PropertyProxy<std::string>> deviceAddress =
            bluezDevice->create_property<std::string>(INTERFACE_BLUEZ_DEVICE, "Address");

        std::string mac_address = deviceAddress ? *deviceAddress : "";

        // Skip if already tried
        if (std::find(prioritized_macs.begin(), prioritized_macs.end(), mac_address) != prioritized_macs.end()) {
            continue;
        }

        Logger::instance()->info("Trying new device at path: %s\n", path.c_str());

        if (tryConnectToDevice(path, isDongleMode)) {
            return;
        }
    }

    if (!isDongleMode) {
        Logger::instance()->info("Failed to connect to any known bluetooth device\n");
    }
}

bool BluetoothHandler::tryConnectToDevice(const std::string& device_path, bool isDongleMode) {
    try {
        std::shared_ptr<DBus::ObjectProxy> bluezDevice = m_connection->create_object_proxy(BLUEZ_BUS_NAME, device_path);
        DBus::MethodProxy connectProfile = *(bluezDevice->create_method<void(std::string)>(INTERFACE_BLUEZ_DEVICE, "ConnectProfile"));
        DBus::MethodProxy disconnect = *(bluezDevice->create_method<void()>(INTERFACE_BLUEZ_DEVICE, "Disconnect"));

        std::shared_ptr<DBus::PropertyProxy<bool>> deviceConnected = bluezDevice->create_property<bool>(INTERFACE_BLUEZ_DEVICE, "Connected");
        std::shared_ptr<DBus::PropertyProxy<std::string>> deviceAddress = bluezDevice->create_property<std::string>(INTERFACE_BLUEZ_DEVICE, "Address");
        std::shared_ptr<DBus::PropertyProxy<std::string>> deviceName = bluezDevice->create_property<std::string>(INTERFACE_BLUEZ_DEVICE, "Name");

        std::string mac_address = deviceAddress ? *deviceAddress : "unknown";
        std::string name = deviceName ? *deviceName : "Unknown Device";

        if (deviceConnected && *deviceConnected) {
            Logger::instance()->info("Device already connected, disconnecting first\n");
            disconnect();
        }

        connectProfile(isDongleMode ? "" : HSP_AG_UUID);
        Logger::instance()->info("Successfully connected to device: %s (%s)\n", name.c_str(), mac_address.c_str());

        // Record successful connection
        DeviceManager::instance()->recordConnection(mac_address, name);

        return true;

    } catch (DBus::Error& e) {
        Logger::instance()->warn("Failed to connect to device at path: %s - %s\n", device_path.c_str(), e.what());

        // Try to get MAC address for failure tracking
        try {
            std::shared_ptr<DBus::ObjectProxy> bluezDevice = m_connection->create_object_proxy(BLUEZ_BUS_NAME, device_path);
            std::shared_ptr<DBus::PropertyProxy<std::string>> deviceAddress = bluezDevice->create_property<std::string>(INTERFACE_BLUEZ_DEVICE, "Address");
            if (deviceAddress) {
                DeviceManager::instance()->recordConnectionFailure(*deviceAddress);
            }
        } catch (...) {
            // Ignore errors getting MAC address
        }

        return false;
    }
}

void BluetoothHandler::retryConnectLoop() {
    bool should_exit = false;
    std::future<void> connectWithRetryFuture = connectWithRetryPromise->get_future();

    int retry_count = 0;
    int retry_delay = 5; // Start with 5 seconds
    const int max_delay = 60; // Cap at 60 seconds
    const int max_retries = 20; // Give up after 20 attempts

    while (!should_exit && retry_count < max_retries) {
        Logger::instance()->info("Bluetooth connection attempt %d/%d\n", retry_count + 1, max_retries);

        connectDevice();

        retry_count++;

        // Wait with exponential backoff
        if (connectWithRetryFuture.wait_for(std::chrono::seconds(retry_delay)) == std::future_status::ready) {
            should_exit = true;
            connectWithRetryPromise = nullptr;
            Logger::instance()->info("Bluetooth connection successful after %d attempts\n", retry_count);
            retry_count = 0; // Reset for next connection cycle
            retry_delay = 5;
        } else {
            // Connection not established, increase delay
            if (retry_count > 1) {
                retry_delay = std::min(retry_delay * 2, max_delay);
                Logger::instance()->info("Connection failed, retrying in %d seconds\n", retry_delay);
            }
        }
    }

    if (retry_count >= max_retries) {
        Logger::instance()->error("Bluetooth connection failed after %d attempts, giving up\n", max_retries);
    }

    if (Config::instance()->getConnectionStrategy() != ConnectionStrategy::DONGLE_MODE) {
        BluetoothHandler::instance().powerOff();
    }
}

void BluetoothHandler::init() {
    // DBus::set_logging_function( DBus::log_std_err );
    // DBus::set_log_level( SL_TRACE );

    m_dispatcher = DBus::StandaloneDispatcher::create();
    m_connection = m_dispatcher->create_connection( DBus::BusType::SYSTEM );

    std::string adapterAliasPrefix = (Config::instance()->getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE) ? ADAPTER_ALIAS_DONGLE_PREFIX : ADAPTER_ALIAS_PREFIX;

    m_adapterAlias = adapterAliasPrefix + Config::instance()->getUniqueSuffix();

    initAdapter();
    exportProfiles();
}

void BluetoothHandler::powerOn() {
    if (!m_adapter) {
        return;
    }

    setPower(true);
    setPairable(true);

    if (Config::instance()->getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE) {
        startAdvertising();
    }
}

std::optional<std::thread> BluetoothHandler::connectWithRetry() {
    if (!m_adapter) {
        return std::nullopt;
    }

    connectWithRetryPromise = std::make_shared<std::promise<void>>();
    return std::thread(&BluetoothHandler::retryConnectLoop, this);
}

void BluetoothHandler::stopConnectWithRetry() {
    if (connectWithRetryPromise) {
        connectWithRetryPromise->set_value();
    }
}

void BluetoothHandler::powerOff() {
    if (!m_adapter) {
        return;
    }

    if (Config::instance()->getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE) {
        stopAdvertising();
    }
    setPower(false);
}