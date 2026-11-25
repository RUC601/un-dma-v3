#pragma once

#include <cstdint>
#include <chrono>

namespace OW {

// A decision is deliberately separate from the low-level V3Raycast status.
// The aim and trigger paths consume these states and never infer visibility
// from a missing backend result.
enum class RayDecisionStatus : uint8_t {
    Unknown = 0,
    Hit,
    Miss,
    Clear,
    Occluded,
    Stale,
    Unavailable,
    Timeout,
    Invalid,
};

enum class RayDecisionPolicy : uint8_t {
    NativeOnly = 0,
    Hybrid,
};

struct RayDecision {
    RayDecisionStatus actor = RayDecisionStatus::Unknown;
    RayDecisionStatus world = RayDecisionStatus::Unknown;
    uint64_t frame = 0;
    uint64_t connectionEpoch = 0;
    uint64_t sceneEpoch = 0;
    uint64_t entityKey = 0;
    uint64_t actorGeneration = 0;
    uint32_t sampleTick = 0;
    uint32_t ageMs = 0;
    uint32_t latencyUs = 0;
    uint64_t completedAtUs = 0;

    static constexpr uint32_t kDefaultMaxAgeMs = 100;

    bool ActorHit() const noexcept { return actor == RayDecisionStatus::Hit; }
    bool WorldClear() const noexcept { return world == RayDecisionStatus::Clear; }
    bool Fresh(uint32_t maxAgeMs = kDefaultMaxAgeMs) const noexcept {
        return world != RayDecisionStatus::Stale && ageMs <= maxAgeMs;
    }
    bool AllowsNativeOnly() const noexcept { return ActorHit(); }
    bool AllowsHybrid(uint32_t maxAgeMs = kDefaultMaxAgeMs) const noexcept {
        return ActorHit() && WorldClear() && Fresh(maxAgeMs);
    }
};

inline bool RayDecisionAllows(const RayDecision& decision, int backend,
                              uint32_t maxAgeMs = RayDecision::kDefaultMaxAgeMs) noexcept {
    return backend == static_cast<int>(RayDecisionPolicy::Hybrid)
        ? decision.AllowsHybrid(maxAgeMs)
        : decision.AllowsNativeOnly();
}

inline bool RayDecisionAllowsSelection(const RayDecision& decision, bool contourFallback,
                                       int backend,
                                       uint32_t maxAgeMs = RayDecision::kDefaultMaxAgeMs) noexcept {
    if (contourFallback) return true;
    const bool actorAcceptable = decision.actor == RayDecisionStatus::Hit ||
                                 decision.actor == RayDecisionStatus::Unknown;
    if (!actorAcceptable) return false;
    if (backend == static_cast<int>(RayDecisionPolicy::Hybrid))
        return decision.WorldClear() && decision.Fresh(maxAgeMs);
    return true;
}

inline const char* RayDecisionPolicyName(int backend) noexcept {
    return backend == static_cast<int>(RayDecisionPolicy::Hybrid) ? "hybrid" : "native-only";
}

// Hybrid needs BOTH a bound UniCore service and an established ray scene. Either one missing
// means world occlusion can never be obtained, so RayDecisionAllowsSelection() rejects every
// non-contour target. The settings page surfaces this state instead of leaving the user with
// unexplained misses and a Diagnostics::Warn every 5s.
inline bool RayPolicyNeedsServiceNotice(int backend, bool serviceBound,
                                        uint64_t sceneEpoch) noexcept {
    return backend == static_cast<int>(RayDecisionPolicy::Hybrid) &&
           (!serviceBound || sceneEpoch == 0);
}

inline uint64_t RayDecisionNowUs() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline const char* RayDecisionStatusName(RayDecisionStatus status) noexcept {
    switch (status) {
        case RayDecisionStatus::Hit: return "hit";
        case RayDecisionStatus::Miss: return "miss";
        case RayDecisionStatus::Clear: return "clear";
        case RayDecisionStatus::Occluded: return "occluded";
        case RayDecisionStatus::Stale: return "stale";
        case RayDecisionStatus::Unavailable: return "unavailable";
        case RayDecisionStatus::Timeout: return "timeout";
        case RayDecisionStatus::Invalid: return "invalid";
        case RayDecisionStatus::Unknown: break;
    }
    return "unknown";
}

}  // namespace OW
