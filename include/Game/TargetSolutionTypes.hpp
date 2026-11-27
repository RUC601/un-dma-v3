#pragma once

#include <algorithm>
#include <cstdint>

namespace OW {

// 1. Guidance target selection policy: determines where guidanceAimPoint pulls.
enum class AimPriority : uint8_t {
    HeadOnly = 0,       // Strict head (or explicit native critical shape)
    HeadThenBody,       // Head -> Neck -> Chest -> Pelvis (hard cutoff at torso core unless execute fallback)
    NeckSpread,         // Neck / upper chest center for shotguns / recoil climb weapons
    BodyCenter,         // Always torso center (Chest / Pelvis), never dragged to hands/feet
    NearestHitbox,      // Closest valid hitbox within allowed mask (local peek / execute finish)
};

// 2. Hit acceptance policy: determines when Trigger / Flick / MagneticTrigger may fire.
enum class HitAcceptance : uint8_t {
    SelectedOnly = 0,   // Fire only when crosshair hits the selected guidance part (or critical shape)
    AnyValidHitbox,     // Fire when crosshair hits any valid hitbox in acceptanceMask
};

enum class VisibilityTier : uint8_t {
    Ignored = 0,
    PreAim,             // Behind wall/barrier pre-aim hold: one-shot park then hands-off, fire forbidden
    Engageable,         // Confirmed hittable: full tracking / flick / fire after reaction gate
};

enum class PreAimPhase : uint8_t {
    None = 0,
    Positioning,        // Probing edge fan and executing one-shot park to wall edge
    Parked,             // Parked at wall edge (or no edge found): 100% hands-off
};

enum class BarrierPolicy : uint8_t {
    IgnoreBarrier = 0,  // Ordinary/shield-break slots: do not query barrier mask
    WaitBarrierDrop,    // Sniper/peek slots: barrier occlusion enters PreAim until shield drops
};

enum class TransitionKind : uint8_t {
    None = 0,
    Hold,               // Same entity and same guidance bone as previous frame
    BoneFallback,       // Same entity, guidance bone switched (e.g. Head -> Chest)
    PeekEmerge,         // Same entity transitioned from PreAim/Ignored to Engageable
    TargetSwitch,       // Switched to a different entity
};

enum class TraceHitKind : uint8_t {
    Unknown = 0,
    None,               // Clear path
    World,              // Blocked by static world geometry / terrain
    Barrier,            // Blocked by dynamic barrier (shield_blocks_fire)
    Actor,              // Blocked by another actor body
};

enum class InputStance : uint8_t {
    ActiveDrag = 0,     // Active human camera residual present (or committed target active)
    IdleHold,           // Physically stationary 150-250ms with no locked target: FOV shrinks
};

// Stage 2 consolidated visibility verification policy (§2, §6.3).
enum class VisibilityPolicy : uint8_t {
    Lite = 0,               // Entity Vis + local native Hitbox only (zero ray, zero capture)
    Vision,                 // Entity Vis + ContourService exposure-first fallback when Vis==false
    RayWorld,               // Async TraceCache + 6-engine VisionProbe (world geometry mask)
    RayWorldAndBarrier,     // Async TraceCache + 6-engine VisionProbe (world + Slot 3 barrier mask)
};

// Pluggable Stage 1/2 role utility mode (§4.2, Phase 3).
enum class RoleUtilityMode : uint8_t {
    Auto = 0,               // Activate built-in hero plugin matching local hero (e.g. Smart Ana)
    Off,                    // Disable role utility plugins (roleUtility = 0)
    SmartAna,               // Explicitly enable Smart Ana (ally low-HP crisis vs enemy execute)
};

inline constexpr int kAimPriorityCount = 5;
inline constexpr int kHitAcceptanceCount = 2;
inline constexpr int kBarrierPolicyCount = 2;
inline constexpr int kVisibilityPolicyCount = 4;
inline constexpr int kRoleUtilityModeCount = 3;

inline constexpr AimPriority ClampAimPriority(int value) noexcept
{
    return static_cast<AimPriority>(std::clamp(value, 0, kAimPriorityCount - 1));
}

inline constexpr HitAcceptance ClampHitAcceptance(int value) noexcept
{
    return static_cast<HitAcceptance>(std::clamp(value, 0, kHitAcceptanceCount - 1));
}

inline constexpr BarrierPolicy ClampBarrierPolicy(int value) noexcept
{
    return static_cast<BarrierPolicy>(std::clamp(value, 0, kBarrierPolicyCount - 1));
}

inline constexpr VisibilityPolicy ClampVisibilityPolicy(int value) noexcept
{
    return static_cast<VisibilityPolicy>(std::clamp(value, 0, kVisibilityPolicyCount - 1));
}

inline constexpr RoleUtilityMode ClampRoleUtilityMode(int value) noexcept
{
    return static_cast<RoleUtilityMode>(std::clamp(value, 0, kRoleUtilityModeCount - 1));
}

// Consolidates runtime ray policy, contour vision fallback, per-slot BarrierPolicy, and
// profile barrier-mask availability into the single authoritative VisibilityPolicy (§2, §6.3).
// rayDecisionPolicy: 0=NativeOnly, 1=Hybrid.
inline constexpr VisibilityPolicy ResolveVisibilityPolicy(int rayDecisionPolicy,
                                                          bool contourEnabled,
                                                          BarrierPolicy barrierPolicy,
                                                          uint64_t barrierMask) noexcept
{
    if (rayDecisionPolicy == 1) {
        if (barrierPolicy == BarrierPolicy::WaitBarrierDrop && barrierMask != 0)
            return VisibilityPolicy::RayWorldAndBarrier;
        return VisibilityPolicy::RayWorld;
    }
    return contourEnabled ? VisibilityPolicy::Vision : VisibilityPolicy::Lite;
}

inline constexpr const char* AimPriorityName(AimPriority priority) noexcept
{
    switch (priority) {
    case AimPriority::HeadOnly: return "HeadOnly";
    case AimPriority::HeadThenBody: return "HeadThenBody";
    case AimPriority::NeckSpread: return "NeckSpread";
    case AimPriority::BodyCenter: return "BodyCenter";
    case AimPriority::NearestHitbox: return "NearestHitbox";
    }
    return "HeadThenBody";
}

inline constexpr const char* HitAcceptanceName(HitAcceptance acceptance) noexcept
{
    switch (acceptance) {
    case HitAcceptance::SelectedOnly: return "SelectedOnly";
    case HitAcceptance::AnyValidHitbox: return "AnyValidHitbox";
    }
    return "SelectedOnly";
}

inline constexpr const char* VisibilityTierName(VisibilityTier tier) noexcept
{
    switch (tier) {
    case VisibilityTier::Ignored: return "Ignored";
    case VisibilityTier::PreAim: return "PreAim";
    case VisibilityTier::Engageable: return "Engageable";
    }
    return "Ignored";
}

inline constexpr const char* TransitionKindName(TransitionKind kind) noexcept
{
    switch (kind) {
    case TransitionKind::None: return "None";
    case TransitionKind::Hold: return "Hold";
    case TransitionKind::BoneFallback: return "BoneFallback";
    case TransitionKind::PeekEmerge: return "PeekEmerge";
    case TransitionKind::TargetSwitch: return "TargetSwitch";
    }
    return "None";
}

inline constexpr const char* TraceHitKindName(TraceHitKind kind) noexcept
{
    switch (kind) {
    case TraceHitKind::Unknown: return "Unknown";
    case TraceHitKind::None: return "None";
    case TraceHitKind::World: return "World";
    case TraceHitKind::Barrier: return "Barrier";
    case TraceHitKind::Actor: return "Actor";
    }
    return "Unknown";
}

inline constexpr const char* InputStanceName(InputStance stance) noexcept
{
    switch (stance) {
    case InputStance::ActiveDrag: return "ActiveDrag";
    case InputStance::IdleHold: return "IdleHold";
    }
    return "ActiveDrag";
}

inline constexpr const char* BarrierPolicyName(BarrierPolicy policy) noexcept
{
    switch (policy) {
    case BarrierPolicy::IgnoreBarrier: return "IgnoreBarrier";
    case BarrierPolicy::WaitBarrierDrop: return "WaitBarrierDrop";
    }
    return "IgnoreBarrier";
}

inline constexpr const char* VisibilityPolicyName(VisibilityPolicy policy) noexcept
{
    switch (policy) {
    case VisibilityPolicy::Lite: return "Lite";
    case VisibilityPolicy::Vision: return "Vision";
    case VisibilityPolicy::RayWorld: return "Ray[World]";
    case VisibilityPolicy::RayWorldAndBarrier: return "Ray[World+Barrier]";
    }
    return "Lite";
}

inline constexpr const char* RoleUtilityModeName(RoleUtilityMode mode) noexcept
{
    switch (mode) {
    case RoleUtilityMode::Auto: return "Auto";
    case RoleUtilityMode::Off: return "Off";
    case RoleUtilityMode::SmartAna: return "SmartAna";
    }
    return "Auto";
}

} // namespace OW
