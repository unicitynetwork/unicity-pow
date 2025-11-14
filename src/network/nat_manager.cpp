// Copyright (c) 2025 The Unicity Foundation
// NAT traversal manager implementation

#include "network/nat_manager.hpp"
#include "util/logging.hpp"
#include <chrono>
#include <cstring>

#ifndef DISABLE_NAT_SUPPORT
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

// miniupnpc API version compatibility
#if defined(MINIUPNPC_API_VERSION) && MINIUPNPC_API_VERSION >= 18
// Version 2.2.8+ (API >= 18): 7 arguments with wanaddr
#define UPNP_GETVALIDIGD_ARGS(devlist, urls, data, lanaddr) \
    devlist, urls, data, lanaddr, sizeof(lanaddr), nullptr, 0
#else
// Older versions (< API 18): 5 arguments
#define UPNP_GETVALIDIGD_ARGS(devlist, urls, data, lanaddr) \
    devlist, urls, data, lanaddr, sizeof(lanaddr)
#endif
#endif

namespace unicity {
namespace network {

namespace {
constexpr int UPNP_DISCOVER_TIMEOUT_MS = 2000;
constexpr int UPNP_MULTICAST_TTL = 2;                // Limit to local network
constexpr int PORT_MAPPING_DURATION_SECONDS = 3600;  // 1 hour
constexpr int REFRESH_INTERVAL_SECONDS = 1800;       // 30 minutes
}

NATManager::NATManager() = default;

NATManager::~NATManager() noexcept {
    try {
        Stop(true); // Silent mode - logger may be destroyed
    } catch (...) {
        // Swallow all exceptions during destruction
        // Cannot log here as logger may be destroyed
    }
}

bool NATManager::Start(uint16_t internal_port) {
    // Atomically check and set running flag to prevent concurrent Start() calls
    if (running_.exchange(true)) {
        LOG_NET_TRACE("NAT manager already running");
        return false;
    }

    // RAII guard to reset running_ flag on exception or early return
    struct RunningGuard {
        std::atomic<bool>& flag;
        bool active = true;
        RunningGuard(std::atomic<bool>& f) : flag(f) {}
        ~RunningGuard() { if (active) flag.store(false, std::memory_order_release); }
        void dismiss() { active = false; }
    };
    RunningGuard running_guard(running_);

    if (internal_port == 0) {
        LOG_NET_ERROR("invalid internal port: 0");
        return false;
    }

    // Perform discovery and port mapping under lock
    {
        std::lock_guard<std::mutex> guard(mapping_mutex_);
        internal_port_ = internal_port;

        LOG_NET_TRACE("starting NAT traversal for port {}", internal_port);

        // Discover UPnP device (protected by mapping_mutex_)
        DiscoverUPnPDevice();

        if (control_url_.empty() || igd_service_type_.empty() || lanaddr_.empty()) {
            LOG_NET_DEBUG("no UPnP-capable gateway found");
            return false;
        }

        // Map port (already protected by mapping_mutex_ through guard)
        if (!MapPort(internal_port)) {
            LOG_NET_ERROR("failed to map port via UPnP");
            return false;
        }
    } // Release lock before creating thread

    // Start refresh thread after discovery and initial mapping complete
    try {
        refresh_thread_ = std::thread([this]() {
            std::unique_lock<std::mutex> lock(refresh_mutex_);
            while (running_) {
                if (refresh_cv_.wait_for(lock, std::chrono::seconds(REFRESH_INTERVAL_SECONDS), [this]() { return !running_; })) {
                    break; // stop requested
                }
                // Perform mapping refresh outside the lock
                lock.unlock();
                try {
                    RefreshMapping();
                } catch (const std::exception& e) {
                    // Don't let exceptions escape and terminate the program
                    LOG_NET_ERROR("UPnP refresh threw exception: {}; will retry next cycle", e.what());
                    // Continue running - next refresh cycle may succeed
                } catch (...) {
                    LOG_NET_ERROR("UPnP refresh threw unknown exception; will retry next cycle");
                }
                lock.lock();
            }
        });
    } catch (const std::exception& e) {
        LOG_NET_ERROR("Failed to create refresh thread: {}; removing mapping", e.what());
        // Thread creation failed - clean up the active port mapping
        UnmapPort();  // This will acquire the mutex
        throw;  // Re-throw - let running_guard reset running_ flag
    }

    LOG_NET_TRACE("NAT traversal successful - external {}:{}{}",
             external_ip_.empty() ? "unknown" : external_ip_,
             external_port_,
             external_ip_.empty() ? " (IP discovery failed)" : "");

    // Success - don't reset running_ flag
    running_guard.dismiss();
    return true;
}

void NATManager::Stop(bool silent) {
    if (!running_.exchange(false)) {
        return;
    }

    // Stop refresh thread
    // IMPORTANT: We must NOT hold mapping_mutex_ here, as the refresh thread
    // may be blocked in RefreshMapping() waiting for that mutex. Joining while
    // holding the mutex would cause a deadlock.
    //
    // Note: This join could block if the refresh thread is stuck in a UPnP call.
    // However, miniupnpc library calls have built-in timeouts (UPNP_DISCOVER_TIMEOUT_MS),
    // so this should not hang indefinitely. The running_ flag being false will also
    // cause the thread to exit on its next loop iteration.
    refresh_cv_.notify_all();
    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }

    // Remove port mapping
    UnmapPort(silent);
}

void NATManager::DiscoverUPnPDevice() {
#ifdef DISABLE_NAT_SUPPORT
    LOG_NET_TRACE("NAT support disabled at compile time");
    return;
#else
    // Clear stale state before attempting discovery
    control_url_.clear();
    igd_service_type_.clear();
    lanaddr_.clear();
    external_ip_.clear();

    int error = 0;
    UPNPDev* devlist = upnpDiscover(
        UPNP_DISCOVER_TIMEOUT_MS,
        nullptr,  // multicast interface
        nullptr,  // minissdpd socket path
        0,        // sameport
        0,        // ipv6
        UPNP_MULTICAST_TTL,
        &error
    );

    if (!devlist) {
        LOG_NET_DEBUG("UPnP discovery failed: error code {}", error);
        return;
    }

    // Get first valid IGD (Internet Gateway Device)
    UPNPUrls urls{};
    IGDdatas data{};
    char lanaddr[64] = {0};

    int result = UPNP_GetValidIGD(UPNP_GETVALIDIGD_ARGS(devlist, &urls, &data, lanaddr));

    freeUPNPDevlist(devlist);

    if (result != 1) {
        LOG_NET_DEBUG("no valid IGD found (result: {})", result);
        FreeUPNPUrls(&urls);
        return;
    }

    // Store gateway info (copy to our cached strings)
    control_url_ = urls.controlURL ? urls.controlURL : "";
    igd_service_type_ = data.first.servicetype; // fixed: servicetype is an array, always non-null
    lanaddr_ = lanaddr;

    // Get external IP
    char ext_ip[40] = {0};
    if (!control_url_.empty() && !igd_service_type_.empty() &&
        UPNP_GetExternalIPAddress(
            control_url_.c_str(),
            igd_service_type_.c_str(),
            ext_ip) == UPNPCOMMAND_SUCCESS) {
        external_ip_ = ext_ip;
        LOG_NET_TRACE("gateway found (LAN: {}, WAN: {})", lanaddr_, external_ip_);
    }

    FreeUPNPUrls(&urls);
#endif // DISABLE_NAT_SUPPORT
}

bool NATManager::MapPort(uint16_t internal_port) {
#ifdef DISABLE_NAT_SUPPORT
    LOG_NET_TRACE("NAT support disabled, skipping port mapping");
    return false;
#else
    // Caller must hold mapping_mutex_
    if (control_url_.empty() || igd_service_type_.empty() || lanaddr_.empty()) {
        return false;
    }

    // Try to map the same port externally
    external_port_ = internal_port;

    const std::string internal_port_str = std::to_string(internal_port);
    const std::string external_port_str = std::to_string(external_port_);
    const std::string duration_str = std::to_string(PORT_MAPPING_DURATION_SECONDS);

    int ret = UPNP_AddPortMapping(
        control_url_.c_str(),
        igd_service_type_.c_str(),
        external_port_str.c_str(),  // external port
        internal_port_str.c_str(),  // internal port
        lanaddr_.c_str(),           // internal client
        "Unicity P2P",       // description
        "TCP",                     // protocol
        nullptr,                    // remote host (any)
        duration_str.c_str()        // lease duration
    );

    if (ret != UPNPCOMMAND_SUCCESS) {
        LOG_NET_ERROR("UPnP port mapping failed: error code {}", ret);
        return false;
    }

    port_mapped_ = true;
    LOG_NET_TRACE("UPnP port mapping created/refreshed: {} -> {}", external_port_, internal_port);
    return true;
#endif // DISABLE_NAT_SUPPORT
}

void NATManager::UnmapPort(bool silent) {
#ifdef DISABLE_NAT_SUPPORT
    return;
#else
    std::lock_guard<std::mutex> guard(mapping_mutex_);

    if (!port_mapped_) {
        return;
    }

    // If gateway info is missing, we can't unmap but the mapping may still exist
    // Just return without changing port_mapped_ state
    if (control_url_.empty() || igd_service_type_.empty()) {
        return;
    }

    const std::string external_port_str = std::to_string(external_port_);

    int ret = UPNP_DeletePortMapping(
        control_url_.c_str(),
        igd_service_type_.c_str(),
        external_port_str.c_str(),
        "TCP",
        nullptr
    );
    (void)ret; // ignore ret; best-effort cleanup

    port_mapped_ = false;
    if (!silent) {
        LOG_NET_TRACE("UPnP port mapping removed");
    }
#endif // DISABLE_NAT_SUPPORT
}

void NATManager::RefreshMapping() {
    LOG_NET_TRACE("refreshing UPnP port mapping");

    // Early exit optimization: port_mapped_ is atomic, so this unlocked read is safe.
    // We double-check under the lock below (double-checked locking pattern).
    if (!port_mapped_) return;

#ifndef DISABLE_NAT_SUPPORT
    // Re-issue the same AddPortMapping call to refresh/extend lease without tearing down
    std::lock_guard<std::mutex> guard(mapping_mutex_);

    // Double-check under lock (port could have been unmapped)
    if (!port_mapped_) return;

    const std::string internal_port_str = std::to_string(internal_port_);
    const std::string external_port_str = std::to_string(external_port_);
    const std::string duration_str = std::to_string(PORT_MAPPING_DURATION_SECONDS);

    int ret = UPNP_AddPortMapping(
        control_url_.c_str(),
        igd_service_type_.c_str(),
        external_port_str.c_str(),
        internal_port_str.c_str(),
        lanaddr_.c_str(),
        "Unicity P2P",
        "TCP",
        nullptr,
        duration_str.c_str());

    if (ret != UPNPCOMMAND_SUCCESS) {
        LOG_NET_ERROR("UPnP refresh failed (error {}), attempting re-discovery", ret);

        // Don't set port_mapped_ = false yet - if re-discovery succeeds, we want to keep trying
        // Gateway may have rebooted or lease expired - re-discover
        DiscoverUPnPDevice();

        bool restored = false;
        if (!control_url_.empty() && !igd_service_type_.empty() && !lanaddr_.empty()) {
            // Re-attempt mapping with new gateway info
            if (MapPort(internal_port_)) {
                LOG_NET_DEBUG("UPnP mapping restored after re-discovery");
                restored = true;
                // MapPort() already set port_mapped_ = true
            } else {
                LOG_NET_ERROR("Failed to restore UPnP mapping after re-discovery");
            }
        } else {
            LOG_NET_ERROR("Gateway no longer available");
        }

        // Only mark as unmapped if we completely failed to restore
        // This way, next refresh cycle will try again
        if (!restored) {
            LOG_NET_ERROR("UPnP mapping lost; will retry on next refresh cycle");
            port_mapped_ = false;
        }
        return;
    }

    // Success - try to refresh external IP as well (it may change)
    char ext_ip[40] = {0};
    if (!control_url_.empty() && !igd_service_type_.empty() &&
        UPNP_GetExternalIPAddress(control_url_.c_str(), igd_service_type_.c_str(), ext_ip) == UPNPCOMMAND_SUCCESS &&
        ext_ip[0] != '\0') {
        external_ip_ = ext_ip;
    }
#endif
}

std::string NATManager::GetExternalIP() const {
    std::lock_guard<std::mutex> guard(mapping_mutex_);
    return external_ip_;
}

uint16_t NATManager::GetExternalPort() const {
    std::lock_guard<std::mutex> guard(mapping_mutex_);
    return external_port_;
}

} // namespace network
} // namespace unicity
