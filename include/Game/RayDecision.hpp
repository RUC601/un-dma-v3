#pragma once

#include "Game/TargetSolutionTypes.hpp"

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

enum class RayStaleReason : uint8_t {
    None = 0,
    AgeExpired,
    SpatialDrift,
    SceneMismatch,
};

enum class RaySegmentKind : uint8_t {
    Direct = 0,
    ParabolicChord,
    GroundClip,
    EdgeFan,
    Barrier,
};

struct RayDecision {
    RayDecisionStatus actor = RayDecisionStatus::Unknown;
    RayDecisionStatus world = RayDecisionStatus::Unknown;
    uint64_t frame = 0;
    uint64_t connectionEpoch = 0;
    uint64_t sceneEpoch = 0;
    uint64_t entityKey = 0;
    uint64_t actorGeneration = 0;
    uint64_t bodyAuthorityRevision = 0;
    uint32_t sampleTick = 0;
    uint32_t ageMs = 0;
    uint32_t latencyUs = 0;
    uint64_t completedAtUs = 0;
    float fraction = 1.0f;
    float hitPosX = 0.0f;
    float hitPosY = 0.0f;
    float hitPosZ = 0.0f;
    bool hasHitPosition = false;
    float hitNormalX = 0.0f;
    float hitNormalY = 0.0f;
    float hitNormalZ = 0.0f;
    bool hasHitNormal = false;
    uint32_t hitFlags = 0;
    uint32_t hitbox = 0xFFFFFFFFu;
    uint32_t hitEntityId = 0xFFFFFFFFu;
    RayStaleReason staleReason = RayStaleReason::None;
    RaySegmentKind segmentKind = RaySegmentKind::Direct;
    uint8_t segmentIndex = 0;
    // Phase 3 Slot 3 dynamic barrier verdict (§6.3, §8.1, §8.2).
    // Only enforced when barrierCheckRequired == true (Ray[World+Barrier] + WaitBarrierDrop).
    RayDecisionStatus barrier = RayDecisionStatus::Unknown;
    uint32_t barrierAgeMs = 0;
    bool barrierCheckRequired = false;

    static constexpr uint32_t kDefaultMaxAgeMs = 100;
    static constexpr uint32_t kDefaultTrackingVacuumGraceMs = 60;

    bool ActorHit() const noexcept { return actor == RayDecisionStatus::Hit; }
    bool WorldClear() const noexcept { return world == RayDecisionStatus::Clear; }
    bool BarrierClear(uint32_t maxAgeMs = kDefaultMaxAgeMs) const noexcept {
        return !barrierCheckRequired ||
               (barrier == RayDecisionStatus::Clear && barrierAgeMs <= maxAgeMs);
    }
    bool BarrierOccluded() const noexcept {
        return barrierCheckRequired && barrier == RayDecisionStatus::Occluded;
    }
    bool IsObservationVacuum() const noexcept {
        return world == RayDecisionStatus::Unknown ||
               world == RayDecisionStatus::Stale ||
               world == RayDecisionStatus::Unavailable ||
               world == RayDecisionStatus::Timeout;
    }
    bool IsBarrierObservationVacuum() const noexcept {
        return barrierCheckRequired && (
            barrier == RayDecisionStatus::Unknown ||
            barrier == RayDecisionStatus::Stale ||
            barrier == RayDecisionStatus::Unavailable ||
            barrier == RayDecisionStatus::Timeout);
    }
    bool Fresh(uint32_t maxAgeMs = kDefaultMaxAgeMs) const noexcept {
        return world != RayDecisionStatus::Stale && ageMs <= maxAgeMs;
    }
    bool AllowsNativeOnly() const noexcept { return ActorHit(); }
    bool AllowsHybrid(uint32_t maxAgeMs = kDefaultMaxAgeMs) const noexcept {
        return ActorHit() && WorldClear() && Fresh(maxAgeMs) && BarrierClear(maxAgeMs);
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
        return decision.WorldClear() && decision.Fresh(maxAgeMs) && !decision.BarrierOccluded();
    return true;
}

// Strict gate for program-generated automatic fire (Trigger / Flick / MagneticTrigger / SingleShot):
// under Hybrid policy, observation vacuum (Stale/Unknown/Unavailable/Timeout) NEVER authorizes fire
// (Decision #7, §6.2, §8.2). When Slot 3 WaitBarrierDrop is active (barrierCheckRequired == true),
// barrier occlusion or barrier vacuum also strictly blocks auto-fire (§8.2, Decision #4).
inline bool RayDecisionAllowsAutoFire(const RayDecision& decision, bool contourFallback,
                                      int backend,
                                      uint32_t maxAgeMs = RayDecision::kDefaultMaxAgeMs) noexcept {
    if (!RayDecisionAllowsSelection(decision, contourFallback, backend, maxAgeMs))
        return false;
    if (!contourFallback && backend == static_cast<int>(RayDecisionPolicy::Hybrid) &&
        decision.barrierCheckRequired) {
        return decision.BarrierClear(maxAgeMs);
    }
    return true;
}

// Tracking continuity grace gate during a brief Hybrid ray observation vacuum (5-15ms after blink
// or sudden motion, Decision #7): if native entity Vis == true, allow <= 60ms smooth tracking hold
// while keeping ray.world in its true vacuum status so auto-fire stays strictly blocked. If native
// entity Vis == false, immediately exit Engageable without waiting for ray completion.
inline bool RayDecisionAllowsTrackingGrace(
    const RayDecision& decision,
    bool entityVis,
    bool contourFallback,
    int backend,
    uint32_t vacuumElapsedMs,
    uint32_t maxVacuumGraceMs = RayDecision::kDefaultTrackingVacuumGraceMs,
    uint32_t maxAgeMs = RayDecision::kDefaultMaxAgeMs) noexcept {
    if (RayDecisionAllowsSelection(decision, contourFallback, backend, maxAgeMs))
        return true;
    if (backend != static_cast<int>(RayDecisionPolicy::Hybrid))
        return false;
    const bool actorAcceptable = decision.actor == RayDecisionStatus::Hit ||
                                 decision.actor == RayDecisionStatus::Unknown;
    if (!actorAcceptable)
        return false;
    return entityVis && decision.IsObservationVacuum() && vacuumElapsedMs <= maxVacuumGraceMs;
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

// WaitBarrierDrop requires BOTH Hybrid ray policy and a live-verified profile barrier mask.
// Surfaces an honest notice whenever a slot requests WaitBarrierDrop on an unsupported build.
inline bool RayPolicyNeedsBarrierNotice(int backend, BarrierPolicy barrierPolicy,
                                        uint64_t barrierMask) noexcept {
    return barrierPolicy == BarrierPolicy::WaitBarrierDrop &&
           (backend != static_cast<int>(RayDecisionPolicy::Hybrid) || barrierMask == 0);
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

inline const char* RayStaleReasonName(RayStaleReason reason) noexcept {
    switch (reason) {
        case RayStaleReason::None: return "none";
        case RayStaleReason::AgeExpired: return "age_expired";
        case RayStaleReason::SpatialDrift: return "spatial_drift";
        case RayStaleReason::SceneMismatch: return "scene_mismatch";
    }
    return "none";
}

inline const char* RaySegmentKindName(RaySegmentKind kind) noexcept {
    switch (kind) {
        case RaySegmentKind::Direct: return "direct";
        case RaySegmentKind::ParabolicChord: return "parabolic_chord";
        case RaySegmentKind::GroundClip: return "ground_clip";
        case RaySegmentKind::EdgeFan: return "edge_fan";
        case RaySegmentKind::Barrier: return "barrier";
    }
    return "direct";
}

}  // namespace OW
