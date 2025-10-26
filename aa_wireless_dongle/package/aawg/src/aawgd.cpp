#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <atomic>

#include "common.h"
#include "bluetoothHandler.h"
#include "proxyHandler.h"
#include "uevent.h"
#include "usb.h"

// Global flag for graceful shutdown
static std::atomic<bool> g_shutdown_requested(false);

void signal_handler(int signum) {
    const char* signame = (signum == SIGTERM) ? "SIGTERM" : (signum == SIGINT) ? "SIGINT" : "SIGNAL";
    Logger::instance()->info("Received %s, initiating graceful shutdown\n", signame);
    g_shutdown_requested = true;
}

void setup_signal_handlers() {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        Logger::instance()->error("Failed to setup SIGTERM handler\n");
    }
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        Logger::instance()->error("Failed to setup SIGINT handler\n");
    }

    // Ignore SIGPIPE to handle broken pipe errors gracefully
    signal(SIGPIPE, SIG_IGN);

    Logger::instance()->info("Signal handlers installed\n");
}

int main(void) {
    Logger::instance()->info("AA Wireless Dongle starting\n");

    setup_signal_handlers();

    // Validate configuration before starting
    if (!Config::instance()->validateConfig()) {
        Logger::instance()->error("Configuration validation failed, please check /etc/aawgd.conf\n");
        return 1;
    }

    // Global init
    std::optional<std::thread> ueventThread =  UeventMonitor::instance().start();
    UsbManager::instance().init();
    BluetoothHandler::instance().init();

    ConnectionStrategy connectionStrategy = Config::instance()->getConnectionStrategy();
    if (connectionStrategy == ConnectionStrategy::DONGLE_MODE) {
        BluetoothHandler::instance().powerOn();
    }

    int connection_cycle = 0;
    int consecutive_failures = 0;
    const int max_consecutive_failures = 5;

    while (!g_shutdown_requested) {
        connection_cycle++;
        Logger::instance()->info("=== Connection cycle #%d ===\n", connection_cycle);
        Logger::instance()->info("Connection Strategy: %d\n", connectionStrategy);

        bool cycle_successful = false;

        // Per connection setup and processing
        if (connectionStrategy == ConnectionStrategy::USB_FIRST) {
            Logger::instance()->info("Waiting for the accessory to connect first\n");
            if (g_shutdown_requested) break;
            UsbManager::instance().enableDefaultAndWaitForAccessory();
        }

        if (g_shutdown_requested) break;

        AAWProxy proxy;
        std::optional<std::thread> proxyThread = proxy.startServer(Config::instance()->getWifiInfo().port);

        if (!proxyThread) {
            Logger::instance()->error("Failed to start proxy server\n");
            consecutive_failures++;

            if (consecutive_failures >= max_consecutive_failures) {
                Logger::instance()->error("Too many consecutive failures (%d), exiting\n", consecutive_failures);
                g_shutdown_requested = true;
                break;
            }

            // Exponential backoff for server failures
            int backoff_delay = std::min(2 << consecutive_failures, 60);
            Logger::instance()->warn("Waiting %d seconds before retry\n", backoff_delay);
            for (int i = 0; i < backoff_delay && !g_shutdown_requested; i++) {
                sleep(1);
            }
            continue;
        }

        if (connectionStrategy != ConnectionStrategy::DONGLE_MODE) {
            BluetoothHandler::instance().powerOn();
        }

        std::optional<std::thread> btConnectionThread = BluetoothHandler::instance().connectWithRetry();

        proxyThread->join();
        cycle_successful = true; // If we got here, at least proxy ran

        if (btConnectionThread) {
            BluetoothHandler::instance().stopConnectWithRetry();
            btConnectionThread->join();
        }

        UsbManager::instance().disableGadget();

        // Reset failure counter on successful cycle
        if (cycle_successful) {
            if (consecutive_failures > 0) {
                Logger::instance()->info("Connection cycle successful, resetting failure counter\n");
            }
            consecutive_failures = 0;
        }

        if (g_shutdown_requested) break;

        if (connectionStrategy != ConnectionStrategy::DONGLE_MODE) {
            // Normal retry delay
            Logger::instance()->info("Waiting before next connection cycle\n");
            for (int i = 0; i < 20 && !g_shutdown_requested; i++) {
                usleep(100000); // 100ms chunks to allow faster shutdown
            }
        }
    }

    // Graceful cleanup
    Logger::instance()->info("Shutting down gracefully\n");

    UsbManager::instance().disableGadget();
    BluetoothHandler::instance().stopConnectWithRetry();
    BluetoothHandler::instance().powerOff();

    if (ueventThread && ueventThread->joinable()) {
        ueventThread->join();
    }

    Logger::instance()->info("AA Wireless Dongle stopped\n");

    return 0;
}
