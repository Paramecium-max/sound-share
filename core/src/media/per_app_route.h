#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace miplay::media {

struct SilentRouteStatus {
    bool supported = false;      // Whether a secondary active eRender endpoint exists besides default speaker
    bool active = false;         // Whether target process tree is currently routed to secondary silent endpoint
    std::string endpoint_name;   // Friendly name of the selected secondary endpoint
    std::string hint;            // User-facing hint when unavailable or degraded
};

struct RecoveryRouteEntry {
    uint32_t pid = 0;
    uint64_t creation_time = 0;
    std::string prior_token;
};

class PerAppSilentRouter {
public:
    PerAppSilentRouter() = default;
    ~PerAppSilentRouter();

    PerAppSilentRouter(const PerAppSilentRouter&) = delete;
    PerAppSilentRouter& operator=(const PerAppSilentRouter&) = delete;

    // Crash recovery on startup: restores any PID routes left over from an abnormal exit.
    static void recover_stale_routes();

    // Checks whether at least one secondary active eRender endpoint exists besides default speaker.
    static bool has_secondary_render_endpoint();

    // Cross-platform helpers for crash recovery file serialization & parsing (unit-tested on all platforms).
    static std::string serialize_recovery_entries(const std::vector<RecoveryRouteEntry>& entries);
    static std::vector<RecoveryRouteEntry> parse_recovery_entries(const std::string& content);

    // Routes root_pid and all its descendant processes to a secondary silent eRender endpoint,
    // plus starts a silent keepalive render stream on that endpoint so its audio clock never stalls.
    bool apply_for_process_tree(uint32_t root_pid, SilentRouteStatus* out_status = nullptr);

    // Restores all routed PIDs back to their prior persisted endpoint (or system default) and stops keepalive.
    void restore();

    SilentRouteStatus status() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace miplay::media
