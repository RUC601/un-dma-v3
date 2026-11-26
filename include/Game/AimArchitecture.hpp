#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <Windows.h>

#include "Game/Entity.hpp"
#include "Game/PredictionAlgorithm.hpp"
#include "Game/PredictionProfile.hpp"
#include "Game/AimBehaviorPolicy.hpp"
#include "Game/AimPlan.hpp"
#include "Game/ContourTargeting.hpp"
#include "Game/TargetVisibility.hpp"
#include "Game/TargetSolutionTypes.hpp"
#include "Game/TriggerBoneSelection.hpp"
#include "Game/RayDecision.hpp"
#include "Utils/Types.hpp"

namespace OW {

enum class AimClass : int {
    HitscanSingle = 0,
    HitscanAuto,
    HitscanBurst,
    ProjectileSingle,
    ProjectileAuto,
    ProjectileExplosive,
    Shotgun,
    Beam,
    Targeted,
    Melee,
    Movement,
    Unknown,
};

enum class PredictionOverrideMode : int {
    Auto = 0,
    ForceOn = 1,
    ForceOff = 2,
};

// Both prediction paths share projectile data. Target motion is sampled from
// the target only. Local velocity never becomes part of projectile muzzle
// velocity or target lead input.
enum class RuntimeVariantRequirement : int {
    None = 0,
    PerkOn,
};

enum class TraceMode : int {
    Strict = 0,
    Relaxed = 1,
    Off = 2,
};

enum class UnlockMode : int {
    Anytime = 0,
    OnRelease = 1,
    NeverUntilTimeout = 2,
};

enum class SmoothingControllerType : int {
    Linear = 0,
    PID = 1,
    Bezier = 2,
    PiecewiseCurve = 3,
    AccelLimited = 4,
    Constant = 5,
    HybridPID = 6,
    Shuwu = 7,
    ErrorResponseCurve = 8,
    KoiProportional = 9,
};

enum class ExecutionSource : int {
    GlobalAim = 0,
    Trigger = 1,
    HeroSkill = 2,
    SequenceInternal = 3,
};

struct FirePolicy {
    FirePolicyType type = FirePolicyType::ManualOnly;
    float releaseDelayMs = 0.0f;
    float burstDurationMs = 0.0f;
    float minCharge = 0.0f;
    float maxCharge = 100.0f;
};

inline constexpr int kWeaponControlUseActionButton = -2;
inline constexpr int kWeaponControlNoButton = -1;

struct WeaponControlSpec {
    int generatedFireButton = kWeaponControlUseActionButton;
    int trackingHoldButton = kWeaponControlUseActionButton;
    int stanceHoldButton = kWeaponControlNoButton;
};

struct AimBehavior {
    AimBehaviorType type = AimBehaviorType::Tracking;
    float behaviorTolerance = 0.0f;
    float maxAimTimeMs = 0.0f;
    float minDistance = 0.0f;
    float maxDistance = 0.0f;
    float headshotDistanceGate = 0.0f;
};

struct TargetLockPolicy {
    TraceMode traceMode = TraceMode::Strict;
    UnlockMode unlockMode = UnlockMode::Anytime;
    float minLockMs = 0.0f;
    float maxLockMs = 0.0f;
    float retargetHysteresis = 0.0f;
};

struct SmoothingControllerProfile {
    SmoothingControllerType type = SmoothingControllerType::Linear;
    float speed = 0.0f;
    float acceleration = 0.0f;
    float pitchScale = 1.0f;
};

struct WeaponSpec {
    uint64_t heroId = 0;
    std::string_view heroName{};
    std::string_view weaponId{};
    std::string_view weaponName{};
    int action = 0;
    int order = 0;
    AimClass aimClass = AimClass::Unknown;
    ProjectileSpec projectile{};
    FirePolicy firePolicy{};
    AimBehaviorType defaultBehavior = AimBehaviorType::Tracking;
    RuntimeVariantRequirement variantRequirement = RuntimeVariantRequirement::None;
    std::string_view variantId{};
    std::string_view replacesWeaponId{};
    WeaponControlSpec control{};
    std::string_view sourceUrl{};
    std::string_view sourceNote{};
    float confidence = 0.0f;
};

struct BoneHitboxSpec {
    int boneId = 0;
    std::string_view boneName{};
    float radiusWorld = 0.0f;
    Vector3 centerOffsetLocal{};
    std::string_view shape{"sphere"};
    std::string_view sourceUrl{};
    std::string_view sourceNote{};
    float confidence = 0.0f;
};

struct HeroGeometrySpec {
    uint64_t heroId = 0;
    std::string_view heroName{};
    const BoneHitboxSpec* bones = nullptr;
    int boneCount = 0;
};

struct EntityMotionState {
    enum class Kind : int {
        Unknown,
        Grounded,
        AirborneRising,
        AirborneApex,
        AirborneFalling,
        Strafing,
        SuddenStop,
        TeleportOrInvalid,
    };

    Kind kind = Kind::Unknown;
    Vector3 worldVelocity{};
    Vector2 screenVelocity{};
    float verticalVelocity = 0.0f;
    float confidence = 0.0f;
};

struct NativeHitWindowInput {
    Vector3 displacement{};
    Vector3 targetVelocity{};
    float launchUpBias = 0.0f;
    float projectileRadius = -1.0f;
    float hitboxScalePercent = 100.0f;
    float projectileSpeed = 0.0f;
    float gravityAcceleration = 0.0f;
    float maxFlightSeconds = 0.0f;
    bool gravity = false;
};

struct TargetCandidate {
    Contour::Observation contour{};
    AimPlan::Effective plan{};
    bool planPreferred = false;
    bool valid = false;
    VisibilityTier tier = VisibilityTier::Ignored;
    PreAimPhase preAimPhase = PreAimPhase::None;
    bool preAimParkPointValid = false;
    TransitionKind transition = TransitionKind::None;
    float reactionRemainingMs = 0.0f;
    bool targetReversalActive = false;
    float targetReversalElapsedMs = 0.0f;

    int entityIndex = -1;
    uint64_t entityKey = 0;
    uint64_t actorGeneration = 0;
    c_entity entitySnapshot{};

    AimPriority aimPriority = AimPriority::HeadThenBody;
    HitAcceptance hitAcceptance = HitAcceptance::SelectedOnly;
    int boneId = 0;
    bool requireExplicitCritical = false;
    SkeletonBoneMask acceptanceMask = 0;
    uint32_t worldClearSlotMask = 0;
    Vector3 rawAimPoint{};
    Vector3 predictedAimPoint{};
    Vector3 aimPoint{};
    Vector2 screenPoint{};
    float distance = 0.0f;
    float fovScore = (std::numeric_limits<float>::max)();
    float effectiveFovDeg = 0.0f;
    float peekGateDeg = 0.0f;
    float utilityScore = 0.0f;
    float commitment = 0.0f;
    float intentAlignment = 0.0f;
    float intentGain = 1.0f;
    InputStance stance = InputStance::ActiveDrag;
    int dynamicFovPresetId = -1;
    bool dynamicFov = false;
    bool predictedFovEntry = false;
    float fovEntryHorizonMs = 0.0f;
    float currentFovScore = (std::numeric_limits<float>::max)();
    float projectedFovScore = (std::numeric_limits<float>::max)();
    float effectiveHitWindow = 0.0f;
    NativeHitWindowInput nativeHitWindow{};
    EntityMotionState motion{};
    TargetLockPolicy lockPolicy{};
    const WeaponSpec* weaponSpec = nullptr;
    RayDecision ray{};
    TraceHitKind hitKind = TraceHitKind::Unknown;
    uint8_t trajectorySegments = 1;
};

// Stage 2 -> Stage 3 authoritative target solution contract (§8.1).
struct TargetSolution {
    bool valid = false;
    VisibilityTier tier = VisibilityTier::Ignored;
    PreAimPhase preAimPhase = PreAimPhase::None;
    bool preAimParkPointValid = false;
    TransitionKind transition = TransitionKind::None;
    float reactionRemainingMs = 0.0f;
    bool targetReversalActive = false;
    float targetReversalElapsedMs = 0.0f;

    // Entity identity and generation guards
    int entityIndex = -1;
    uint64_t entityKey = 0;
    uint64_t actorGeneration = 0;
    c_entity entitySnapshot{};

    // Decoupled guidance (where to pull) vs hit acceptance (where to fire)
    AimPriority aimPriority = AimPriority::HeadThenBody;
    HitAcceptance hitAcceptance = HitAcceptance::SelectedOnly;
    int guidanceBoneId = -1;
    bool requireExplicitCritical = false;
    Vector3 rawGuidancePoint{};
    Vector3 predictedGuidancePoint{};
    Vector3 guidanceAimPoint{};
    Vector2 guidanceScreenPoint{};
    SkeletonBoneMask acceptanceMask = 0;
    uint32_t worldClearSlotMask = 0;
    float effectiveHitWindow = 0.0f;
    NativeHitWindowInput nativeHitWindow{};

    // Utility scoring, commitment, and intent evidence snapshot
    float utilityScore = 0.0f;
    float commitment = 0.0f;
    float intentAlignment = 0.0f;
    float intentGain = 1.0f;
    InputStance stance = InputStance::ActiveDrag;
    float distance = 0.0f;
    float angularErrorDeg = 0.0f;
    float effectiveFovDeg = 0.0f;
    float peekGateDeg = 0.0f;

    // Path and occlusion evidence
    RayDecision ray{};
    TraceHitKind hitKind = TraceHitKind::Unknown;
    uint8_t trajectorySegments = 1;
    Contour::Observation contour{};

    // Policy and motion context
    AimPlan::Effective plan{};
    bool planPreferred = false;
    int dynamicFovPresetId = -1;
    bool dynamicFov = false;
    bool predictedFovEntry = false;
    float fovEntryHorizonMs = 0.0f;
    float currentFovScore = (std::numeric_limits<float>::max)();
    float projectedFovScore = (std::numeric_limits<float>::max)();
    TargetLockPolicy lockPolicy{};
    EntityMotionState motion{};
    const WeaponSpec* weaponSpec = nullptr;

    TargetCandidate ToCandidate() const
    {
        TargetCandidate c{};
        c.contour = contour;
        c.plan = plan;
        c.planPreferred = planPreferred;
        c.valid = valid;
        c.tier = tier;
        c.preAimPhase = preAimPhase;
        c.preAimParkPointValid = preAimParkPointValid;
        c.transition = transition;
        c.reactionRemainingMs = reactionRemainingMs;
        c.targetReversalActive = targetReversalActive;
        c.targetReversalElapsedMs = targetReversalElapsedMs;
        c.entityIndex = entityIndex;
        c.entityKey = entityKey;
        c.actorGeneration = actorGeneration;
        c.entitySnapshot = entitySnapshot;
        c.aimPriority = aimPriority;
        c.hitAcceptance = hitAcceptance;
        c.boneId = guidanceBoneId;
        c.requireExplicitCritical = requireExplicitCritical;
        c.acceptanceMask = acceptanceMask;
        c.worldClearSlotMask = worldClearSlotMask;
        c.rawAimPoint = rawGuidancePoint;
        c.predictedAimPoint = predictedGuidancePoint;
        c.aimPoint = guidanceAimPoint;
        c.screenPoint = guidanceScreenPoint;
        c.distance = distance;
        c.fovScore = angularErrorDeg;
        c.effectiveFovDeg = effectiveFovDeg;
        c.peekGateDeg = peekGateDeg;
        c.utilityScore = utilityScore;
        c.commitment = commitment;
        c.intentAlignment = intentAlignment;
        c.intentGain = intentGain;
        c.stance = stance;
        c.dynamicFovPresetId = dynamicFovPresetId;
        c.dynamicFov = dynamicFov;
        c.predictedFovEntry = predictedFovEntry;
        c.fovEntryHorizonMs = fovEntryHorizonMs;
        c.currentFovScore = currentFovScore;
        c.projectedFovScore = projectedFovScore;
        c.effectiveHitWindow = effectiveHitWindow;
        c.nativeHitWindow = nativeHitWindow;
        c.motion = motion;
        c.lockPolicy = lockPolicy;
        c.weaponSpec = weaponSpec;
        c.ray = ray;
        c.hitKind = hitKind;
        c.trajectorySegments = trajectorySegments;
        return c;
    }

    static TargetSolution FromCandidate(const TargetCandidate& c)
    {
        TargetSolution s{};
        s.valid = c.valid;
        s.tier = c.valid && c.tier == VisibilityTier::Ignored ? VisibilityTier::Engageable : c.tier;
        s.preAimPhase = c.preAimPhase;
        s.preAimParkPointValid = c.preAimParkPointValid;
        s.transition = c.transition;
        s.reactionRemainingMs = c.reactionRemainingMs;
        s.targetReversalActive = c.targetReversalActive;
        s.targetReversalElapsedMs = c.targetReversalElapsedMs;
        s.entityIndex = c.entityIndex;
        s.entityKey = c.entityKey;
        s.actorGeneration = c.actorGeneration != 0 ? c.actorGeneration : c.entitySnapshot.actor_generation;
        s.entitySnapshot = c.entitySnapshot;
        s.aimPriority = c.aimPriority;
        s.hitAcceptance = c.hitAcceptance;
        s.guidanceBoneId = c.boneId;
        s.requireExplicitCritical = c.requireExplicitCritical;
        s.rawGuidancePoint = c.rawAimPoint;
        s.predictedGuidancePoint = c.predictedAimPoint;
        s.guidanceAimPoint = c.aimPoint;
        s.guidanceScreenPoint = c.screenPoint;
        s.acceptanceMask = c.acceptanceMask;
        s.worldClearSlotMask = c.worldClearSlotMask;
        s.effectiveHitWindow = c.effectiveHitWindow;
        s.nativeHitWindow = c.nativeHitWindow;
        s.utilityScore = c.utilityScore;
        s.commitment = c.commitment;
        s.intentAlignment = c.intentAlignment;
        s.intentGain = c.intentGain;
        s.stance = c.stance;
        s.distance = c.distance;
        s.angularErrorDeg = c.fovScore;
        s.effectiveFovDeg = c.effectiveFovDeg;
        s.peekGateDeg = c.peekGateDeg;
        s.ray = c.ray;
        s.hitKind = c.hitKind;
        s.trajectorySegments = c.trajectorySegments;
        s.contour = c.contour;
        s.plan = c.plan;
        s.planPreferred = c.planPreferred;
        s.dynamicFovPresetId = c.dynamicFovPresetId;
        s.dynamicFov = c.dynamicFov;
        s.predictedFovEntry = c.predictedFovEntry;
        s.fovEntryHorizonMs = c.fovEntryHorizonMs;
        s.currentFovScore = c.currentFovScore;
        s.projectedFovScore = c.projectedFovScore;
        s.lockPolicy = c.lockPolicy;
        s.motion = c.motion;
        s.weaponSpec = c.weaponSpec;
        return s;
    }
};

inline PredictionOverrideMode ClampPredictionOverride(int value)
{
    return static_cast<PredictionOverrideMode>(std::clamp(value, 0, 2));
}

inline FirePolicyType ClampFirePolicy(int value)
{
    return static_cast<FirePolicyType>(std::clamp(value, 0, 6));
}

inline TraceMode ClampTraceMode(int value)
{
    return static_cast<TraceMode>(std::clamp(value, 0, 2));
}

inline UnlockMode ClampUnlockMode(int value)
{
    return static_cast<UnlockMode>(std::clamp(value, 0, 2));
}

inline SmoothingControllerType ClampSmoothingController(int value)
{
    return static_cast<SmoothingControllerType>(std::clamp(value, 0, AimPlan::kMotionMethodCount - 1));
}

inline bool ResolvePredictionEnabled(PredictionOverrideMode mode,
                                     const WeaponSpec* weapon,
                                     bool legacyFallback)
{
    // V2 prediction is always part of the target-point pipeline.  Keep the
    // old arguments so archived configs and call sites remain source
    // compatible, but do not let Auto/Force Off become a second runtime
    // truth.  A zero-speed profile still produces the valid pre-fire motion
    // estimate and cast-delay path without inventing projectile flight.
    (void)mode;
    (void)weapon;
    (void)legacyFallback;
    return true;
}

inline int LegacyAimModeFromBehavior(AimBehaviorType behavior)
{
    return behavior == AimBehaviorType::Tracking ? 0 : 1;
}

inline bool AllowsGeneratedAimFire(FirePolicyType policy)
{
    return policy != FirePolicyType::ManualOnly;
}

inline constexpr bool IsKeepFiringPolicy(FirePolicyType policy) noexcept
{
    return policy == FirePolicyType::KeepFiring;
}

inline constexpr bool IsSingleShotPolicy(FirePolicyType policy) noexcept
{
    return policy == FirePolicyType::SingleShot;
}

inline constexpr bool IsTrackingWithHoldPolicy(FirePolicyType policy) noexcept
{
    return policy == FirePolicyType::TrackingWithHold;
}

inline bool AimActivationAllowed(bool activationHotkeyDown,
                                 bool requireActionHeld,
                                 bool actionButtonDown)
{
    return activationHotkeyDown && (!requireActionHeld || actionButtonDown);
}

inline constexpr bool AllowsGeneratedTrackingHold(FirePolicyType policy,
                                                   bool activationIsPrimaryMouseAction,
                                                   bool activationMatchesOutputButton)
{
    // Tracking With Hold owns the selected Action Profile's output button
    // during the tracking session.  The activation key starts that session
    // and is not required to be the same physical mouse button.
    (void)activationIsPrimaryMouseAction;
    (void)activationMatchesOutputButton;
    return policy == FirePolicyType::TrackingWithHold;
}

inline constexpr bool ShouldApplyDisabledHeroTriggerFallback(
    bool hasPresentHeroTriggerSlot,
    bool hasEnabledHeroTriggerFallback) noexcept
{
    return hasPresentHeroTriggerSlot && !hasEnabledHeroTriggerFallback;
}

static_assert(AllowsGeneratedTrackingHold(
    FirePolicyType::HoldWhileTracking, true, false));
static_assert(AllowsGeneratedTrackingHold(
    FirePolicyType::HoldWhileTracking, true, true));
static_assert(AllowsGeneratedTrackingHold(
    FirePolicyType::HoldWhileTracking, false, false));
static_assert(ShouldApplyDisabledHeroTriggerFallback(true, false));

} // namespace OW
