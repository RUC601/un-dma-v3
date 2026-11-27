#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "Game/AimFirePhase.hpp"
#include "Game/BoneSlots.hpp"
#include "Game/IntentEngine.hpp"
#include "Game/TargetSolutionTypes.hpp"
#include "Game/TriggerBoneSelection.hpp"
#include "Utils/Types.hpp"

namespace OW::TargetPipeline {

inline constexpr SkeletonBoneMask kTorsoCoreBoneMask =
    kSkeletonBoneHead | kSkeletonBoneNeck | kSkeletonBoneChest | kSkeletonBonePelvis;

inline constexpr SkeletonBoneMask kLimbBoneMask =
    kSkeletonBoneAllMask & ~kTorsoCoreBoneMask;

// Priority-ordered guidance bone ladder produced by AimPriority (decoupled from HitAcceptance).
struct GuidanceBoneLadder {
    std::array<SkeletonBoneMask, 10> groups{};
    std::size_t count = 0;
    bool nearestMode = false;
    SkeletonBoneMask nearestMask = 0;

    void PushUnique(SkeletonBoneMask group) noexcept
    {
        if (group == 0 || count >= groups.size())
            return;
        for (std::size_t i = 0; i < count; ++i) {
            if (groups[i] == group)
                return;
        }
        groups[count++] = group;
    }

    bool Empty() const noexcept
    {
        return !nearestMode && count == 0;
    }
};

// Maps a render skeleton boneId back to its SkeletonBoneMask group bit (0 if unknown).
inline constexpr SkeletonBoneMask BoneGroupMaskForBoneId(int boneId) noexcept
{
    for (std::size_t slot = 0; slot < Plexies20260609::kSkeletonSlotCount; ++slot) {
        if (Plexies20260609::HitboxBoneIdForRenderSlot(slot) == boneId)
            return kSkeletonBoneGroupForRenderSlot[slot];
    }
    return 0;
}

// Maps an exact render skeleton boneId to its individual 18-slot bitmask (left/right distinct, R2 fix).
inline constexpr uint32_t RenderSlotBitForBoneId(int boneId) noexcept
{
    for (std::size_t slot = 0; slot < Plexies20260609::kSkeletonSlotCount; ++slot) {
        if (Plexies20260609::HitboxBoneIdForRenderSlot(slot) == boneId)
            return 1u << static_cast<uint32_t>(slot);
    }
    return 0u;
}

// Computes the predicted world endpoint for off-guidance acceptance ray lookups and warmup
// so world occlusion validates the exact predicted hitbox evaluated by NativeHitWindowScore.
inline Vector3 ComputeAcceptanceRayEndpoint(const Vector3& rawSlotPoint,
                                            const Vector3& rawGuidancePoint,
                                            const Vector3& predictedGuidancePoint,
                                            const NativeHitWindowInput& nativeHitWindow) noexcept
{
    const Vector3 offset = nativeHitWindow.projectileSpeed > 0.0f
        ? (predictedGuidancePoint - rawGuidancePoint)
        : nativeHitWindow.displacement;
    if (!std::isfinite(offset.X) || !std::isfinite(offset.Y) || !std::isfinite(offset.Z))
        return rawSlotPoint;
    return rawSlotPoint + offset;
}

// Decision #1 (R6 fix): short reaction delay followed by smooth cubic ease-in on target reversal.
// Prevents both instant non-human velocity snap and abrupt jump at latch expiry.
inline float ComputeReversalEaseScale(bool reversalActive,
                                      float elapsedMs,
                                      float delayMs = 35.0f,
                                      float rampMs = 125.0f,
                                      float floorScale = 0.15f) noexcept
{
    if (!reversalActive)
        return 1.0f;
    const float safeElapsed = std::isfinite(elapsedMs) ? (std::max)(0.0f, elapsedMs) : 0.0f;
    const float safeDelay = (std::max)(0.0f, delayMs);
    const float safeRamp = (std::max)(1.0f, rampMs);
    const float safeFloor = std::clamp(floorScale, 0.0f, 1.0f);
    if (safeElapsed <= safeDelay)
        return safeFloor;
    const float t = std::clamp((safeElapsed - safeDelay) / safeRamp, 0.0f, 1.0f);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return safeFloor + (1.0f - safeFloor) * smooth;
}

// Builds the Stage 2 guidance bone ladder strictly according to AimPriority (§7.2, Decision #6).
// HeadThenBody hard-cuts at torso core (Head -> Neck -> Chest -> Pelvis) by default so aiming
// never gets dragged down to wrists/ankles; limbs are appended only when allowLimbExecuteFallback
// is explicitly true (e.g. target in low-HP execute range) or when AimPriority::NearestHitbox is chosen.
inline GuidanceBoneLadder BuildGuidanceBoneLadder(AimPriority priority,
                                                  SkeletonBoneMask rawMask,
                                                  float distance,
                                                  float maxHeadDistance,
                                                  bool allowLimbExecuteFallback = false) noexcept
{
    GuidanceBoneLadder ladder{};
    const SkeletonBoneMask normalizedRaw = NormalizeSkeletonBoneMask(rawMask);
    const SkeletonBoneMask gatedMask = AimMaskForDistance(
        normalizedRaw,
        distance,
        maxHeadDistance,
        priority == AimPriority::HeadOnly);

    switch (priority) {
    case AimPriority::HeadOnly: {
        if (gatedMask != 0 && (gatedMask & kSkeletonBoneHead) != 0)
            ladder.PushUnique(kSkeletonBoneHead);
        return ladder;
    }
    case AimPriority::HeadThenBody: {
        constexpr std::array<SkeletonBoneMask, 4> kCoreOrder{{
            kSkeletonBoneHead, kSkeletonBoneNeck, kSkeletonBoneChest, kSkeletonBonePelvis
        }};
        for (SkeletonBoneMask group : kCoreOrder) {
            if ((gatedMask & group) != 0)
                ladder.PushUnique(group);
        }
        // If the user explicitly selected HeadThenBody while keeping the default Head-only mask,
        // still provide torso core fallbacks (Neck -> Chest -> Pelvis) when Head is blocked/out-of-range.
        if (normalizedRaw == kSkeletonBoneHead) {
            ladder.PushUnique(kSkeletonBoneNeck);
            ladder.PushUnique(kSkeletonBoneChest);
            ladder.PushUnique(kSkeletonBonePelvis);
        } else if (ladder.count == 0) {
            // If rawMask had only non-core bones or head was stripped, fall back to torso core.
            ladder.PushUnique(kSkeletonBoneNeck);
            ladder.PushUnique(kSkeletonBoneChest);
            ladder.PushUnique(kSkeletonBonePelvis);
        }
        if (allowLimbExecuteFallback) {
            constexpr std::array<SkeletonBoneMask, 6> kLimbOrder{{
                kSkeletonBoneShoulders, kSkeletonBoneElbows, kSkeletonBoneKnees,
                kSkeletonBoneShanks, kSkeletonBoneHands, kSkeletonBoneFeet
            }};
            for (SkeletonBoneMask group : kLimbOrder) {
                if ((gatedMask & group) != 0)
                    ladder.PushUnique(group);
            }
        }
        return ladder;
    }
    case AimPriority::NeckSpread: {
        constexpr std::array<SkeletonBoneMask, 4> kSpreadOrder{{
            kSkeletonBoneNeck, kSkeletonBoneChest, kSkeletonBoneHead, kSkeletonBonePelvis
        }};
        for (SkeletonBoneMask group : kSpreadOrder) {
            if ((gatedMask & group) != 0)
                ladder.PushUnique(group);
        }
        if (ladder.count == 0) {
            ladder.PushUnique(kSkeletonBoneNeck);
            ladder.PushUnique(kSkeletonBoneChest);
        }
        return ladder;
    }
    case AimPriority::BodyCenter: {
        constexpr std::array<SkeletonBoneMask, 2> kBodyOrder{{
            kSkeletonBoneChest, kSkeletonBonePelvis
        }};
        for (SkeletonBoneMask group : kBodyOrder) {
            if ((normalizedRaw & group) != 0)
                ladder.PushUnique(group);
        }
        // Always keep Chest -> Pelvis available for BodyCenter even if legacy mask was Head-only.
        ladder.PushUnique(kSkeletonBoneChest);
        ladder.PushUnique(kSkeletonBonePelvis);
        // When Ray leak-shot fallback is enabled, walk outward deterministically from body center
        // so occluded-torso targets still engage cleanly without nearest-bone hesitation.
        if (allowLimbExecuteFallback) {
            constexpr std::array<SkeletonBoneMask, 8> kCenterOutwardFallback{{
                kSkeletonBoneNeck, kSkeletonBoneHead,
                kSkeletonBoneShoulders, kSkeletonBoneKnees,
                kSkeletonBoneElbows, kSkeletonBoneShanks,
                kSkeletonBoneHands, kSkeletonBoneFeet
            }};
            for (SkeletonBoneMask group : kCenterOutwardFallback) {
                if ((gatedMask & group) != 0)
                    ladder.PushUnique(group);
            }
        }
        return ladder;
    }
    case AimPriority::NearestHitbox: {
        ladder.nearestMode = true;
        ladder.nearestMask = gatedMask != 0 ? gatedMask : normalizedRaw;
        return ladder;
    }
    }
    return ladder;
}

// Resolves the Stage 2/3 HitAcceptance mask decoupled from AimPriority.
// Returns 0 for SelectedOnly (meaning TargetHitWindow checks only guidanceBoneId).
inline SkeletonBoneMask ResolveAcceptanceMask(HitAcceptance acceptance,
                                              SkeletonBoneMask rawMask,
                                              float distance,
                                              float maxHeadDistance,
                                              int guidanceBoneId) noexcept
{
    if (acceptance == HitAcceptance::SelectedOnly)
        return 0;

    SkeletonBoneMask mask = NormalizeSkeletonBoneMask(
        AimMaskForDistance(rawMask, distance, maxHeadDistance, false));
    const SkeletonBoneMask guidanceGroup = BoneGroupMaskForBoneId(guidanceBoneId);
    if (guidanceGroup != 0)
        mask |= guidanceGroup;
    return NormalizeSkeletonBoneMask(mask);
}

struct UtilityWeights {
    float primaryWeight = 1.00f;
    float dwellCommitWeight = 0.22f;
    float fireAnchorWeight = 0.45f;
    float forwardIntentWeight = 0.35f;
    float reverseIntentWeight = 0.45f;
    float visibilityWeight = 0.15f;
    // Bounded legacy AimPlan::preferRules bonus (replaces infinite lexical domination, B2 fix).
    float legacyPreferBonus = 0.25f;
    // Mild tie-breaker penalty for predicted-FOV-entry candidates vs already-inside-FOV candidates.
    float predictedEntryPenalty = 0.10f;
};

struct UtilityInput {
    int priorityMode = 0;               // 0=FOV, 1=HP, 2=Distance, 3=Weighted
    float angleWeight = 1.0f;
    float distanceWeight = 0.0f;
    float healthWeight = 0.0f;

    float angleDeg = 0.0f;
    float effectiveFovDeg = 15.0f;
    float distance = 0.0f;
    float maxDistance = 100.0f;
    float hp = 200.0f;
    float maxHp = 200.0f;

    bool isCurrentLocked = false;
    float dwellCommitment = 0.0f;       // [0, 1]
    float firePhaseCommitment = 0.0f;   // [0, 1]
    float retargetHysteresis = 100.0f;  // 0..100 legacy stickiness slider

    bool intentActiveDrag = false;
    float intentAlignment = 0.0f;       // [-1, 1]
    float intentConfidence = 0.0f;      // [0, 1]
    float reverseAlignmentThreshold = -0.35f;

    float visibilityPrior = 0.4f;       // Clear=1.0, EntityVis=0.7, Unknown=0.4, PreAim=0.3
    bool planPreferred = false;         // Legacy AimPlan::preferRules match -> bounded bonus
    float roleUtility = 0.0f;           // Extensible hero/role utility hook (Phase 3 Smart Ana, etc.)
    bool predictedFovEntry = false;
};

struct UtilityBreakdown {
    float primaryTerm = 0.0f;
    float commitTerm = 0.0f;
    float intentTerm = 0.0f;
    float visibilityTerm = 0.0f;
    float roleTerm = 0.0f;
    float total = -(std::numeric_limits<float>::infinity)();
    float effectiveCommitment = 0.0f;   // [0, 1]
    bool hardBlockedByIntent = false;
};

inline float ComputeDwellCommitment(float dwellMs, float fullCommitMs = 260.0f) noexcept
{
    if (!std::isfinite(dwellMs) || dwellMs <= 0.0f)
        return 0.0f;
    const float denom = (std::max)(1.0f, std::isfinite(fullCommitMs) ? fullCommitMs : 260.0f);
    return std::clamp(dwellMs / denom, 0.0f, 1.0f);
}

inline float ComputeFirePhaseCommitment(const AimFirePhase::Phase& phase,
                                        bool phaseActive) noexcept
{
    if (!phaseActive)
        return 0.0f;
    if (phase.preFireBoost || phase.paused)
        return 1.0f;
    if (phase.recovering)
        return 0.75f;
    return 0.0f;
}

inline UtilityBreakdown ComputeCandidateUtility(const UtilityInput& in,
                                                const UtilityWeights& w = {}) noexcept
{
    UtilityBreakdown out{};
    if (!std::isfinite(in.angleDeg) || !std::isfinite(in.distance) || !std::isfinite(in.hp))
        return out;

    const float safeFov = (std::max)(0.001f, std::isfinite(in.effectiveFovDeg) ? in.effectiveFovDeg : 15.0f);
    const float safeDist = (std::max)(0.001f, std::isfinite(in.maxDistance) && in.maxDistance > 0.0f ? in.maxDistance : 100.0f);
    const float uFov = 1.0f - std::clamp(in.angleDeg / safeFov, 0.0f, 1.0f);
    const float uDist = 1.0f - std::clamp(in.distance / safeDist, 0.0f, 1.0f);
    const float hpFrac = (std::isfinite(in.maxHp) && in.maxHp > 0.0f)
        ? std::clamp(in.hp / in.maxHp, 0.0f, 1.0f)
        : 1.0f;
    const float uHp = 1.0f - hpFrac;

    float uPrimary = uFov;
    if (in.priorityMode == 1) {
        uPrimary = 0.70f * uHp + 0.30f * uFov;
    } else if (in.priorityMode == 2) {
        uPrimary = 0.70f * uDist + 0.30f * uFov;
    } else if (in.priorityMode == 3) {
        const float aw = std::clamp(std::isfinite(in.angleWeight) ? in.angleWeight : 1.0f, 0.0f, 100.0f);
        const float dw = std::clamp(std::isfinite(in.distanceWeight) ? in.distanceWeight : 0.0f, 0.0f, 100.0f);
        const float hw = std::clamp(std::isfinite(in.healthWeight) ? in.healthWeight : 0.0f, 0.0f, 100.0f);
        const float sum = aw + dw + hw;
        uPrimary = sum > 0.0f ? (aw * uFov + dw * uDist + hw * uHp) / sum : uFov;
    }

    const float stickiness = std::clamp(
        std::isfinite(in.retargetHysteresis) ? in.retargetHysteresis : 0.0f,
        0.0f, 100.0f) * 0.01f;
    const float dwellC = in.isCurrentLocked
        ? std::clamp(std::isfinite(in.dwellCommitment) ? in.dwellCommitment : 0.0f, 0.0f, 1.0f)
        : 0.0f;
    const float fireC = in.isCurrentLocked
        ? std::clamp(std::isfinite(in.firePhaseCommitment) ? in.firePhaseCommitment : 0.0f, 0.0f, 1.0f)
        : 0.0f;

    out.effectiveCommitment = (std::max)(dwellC * stickiness, fireC);
    const float dwellBonus = w.dwellCommitWeight * stickiness * dwellC;
    const float fireBonus = w.fireAnchorWeight * fireC;
    out.commitTerm = (std::max)(dwellBonus, fireBonus);

    const float conf = std::clamp(
        std::isfinite(in.intentConfidence) ? in.intentConfidence : 0.0f, 0.0f, 1.0f);
    const float align = std::clamp(
        std::isfinite(in.intentAlignment) ? in.intentAlignment : 0.0f, -1.0f, 1.0f);
    if (in.intentActiveDrag && conf > 0.0f) {
        const float intentWeight = align < 0.0f ? w.reverseIntentWeight : w.forwardIntentWeight;
        out.intentTerm = intentWeight * align * conf;
        if (!in.isCurrentLocked && conf >= 0.5f && align < in.reverseAlignmentThreshold)
            out.hardBlockedByIntent = true;
    }

    const float visPrior = std::clamp(
        std::isfinite(in.visibilityPrior) ? in.visibilityPrior : 0.4f, 0.0f, 1.0f);
    out.primaryTerm = w.primaryWeight * uPrimary;
    out.visibilityTerm = w.visibilityWeight * visPrior;
    out.roleTerm = (in.planPreferred ? w.legacyPreferBonus : 0.0f) +
                   (std::isfinite(in.roleUtility) ? in.roleUtility : 0.0f);

    const float entryAdjust = in.predictedFovEntry ? -w.predictedEntryPenalty : 0.0f;
    out.total = out.primaryTerm + out.commitTerm + out.intentTerm +
                out.visibilityTerm + out.roleTerm + entryAdjust;
    return out;
}

// Deterministic higher-utility-wins comparator with stable entityKey tie-break.
inline bool UtilityBetter(float lhsScore, uint64_t lhsKey,
                          float rhsScore, uint64_t rhsKey) noexcept
{
    if (!std::isfinite(lhsScore))
        return false;
    if (!std::isfinite(rhsScore))
        return true;
    if (lhsScore != rhsScore)
        return lhsScore > rhsScore;
    return lhsKey < rhsKey;
}

// Detects target horizontal velocity reversal or sharp sudden stop (Decision #1).
inline bool DetectTargetVelocityReversal(const Vector3& previousVelocity,
                                         const Vector3& currentVelocity,
                                         float minPrevSpeedMps = 1.8f) noexcept
{
    if (!std::isfinite(previousVelocity.X) || !std::isfinite(previousVelocity.Z) ||
        !std::isfinite(currentVelocity.X) || !std::isfinite(currentVelocity.Z)) {
        return false;
    }
    const float prevSpeed = std::hypot(previousVelocity.X, previousVelocity.Z);
    const float currSpeed = std::hypot(currentVelocity.X, currentVelocity.Z);
    if (prevSpeed < minPrevSpeedMps)
        return false;
    // Sudden stop: speed drops by > 65% in a sample window.
    if (currSpeed < prevSpeed * 0.35f)
        return true;
    const float dot = (previousVelocity.X * currentVelocity.X +
                       previousVelocity.Z * currentVelocity.Z) / (prevSpeed * currSpeed);
    return dot < -0.35f;
}

// Projects a world velocity vector into approximate local angular velocity (yawDeg/s, pitchDeg/s)
// around the camera-to-target vector, used to check if human pull aligns with target's new velocity.
inline void ProjectWorldVelocityToAngularRates(const Vector3& cameraPos,
                                               const Vector3& targetPos,
                                               const Vector3& targetVelocity,
                                               float& outYawDegPerSec,
                                               float& outPitchDegPerSec) noexcept
{
    outYawDegPerSec = 0.0f;
    outPitchDegPerSec = 0.0f;
    const Vector3 rel = targetPos - cameraPos;
    const float dist = rel.Size();
    if (!std::isfinite(dist) || dist <= 0.25f)
        return;

    const float horizSq = rel.X * rel.X + rel.Z * rel.Z;
    const float horiz = std::sqrt(horizSq);
    if (horiz <= 0.05f)
        return;

    // yaw = atan2(rel.X, rel.Z) -> dyaw/dt = (rel.Z * vx - rel.X * vz) / (rel.X^2 + rel.Z^2)
    const float yawRadPerSec = (rel.Z * targetVelocity.X - rel.X * targetVelocity.Z) / horizSq;
    // pitch = -atan2(rel.Y, horiz)
    const float dHorizDt = (rel.X * targetVelocity.X + rel.Z * targetVelocity.Z) / horiz;
    const float pitchRadPerSec = -(horiz * targetVelocity.Y - rel.Y * dHorizDt) / (dist * dist);
    if (std::isfinite(yawRadPerSec) && std::isfinite(pitchRadPerSec)) {
        outYawDegPerSec = RAD2DEG(yawRadPerSec);
        outPitchDegPerSec = RAD2DEG(pitchRadPerSec);
    }
}

struct AcquisitionContext {
    AimFirePhase::Phase firePhase{};
    bool firePhaseActive = false;
    bool useIntentEngine = true;
    std::size_t stage1TopK = 3;
};

inline constexpr uint32_t kFastTargetRayMaxAgeMs = 60u;
inline constexpr float kFastTargetSpeedThresholdMps = 4.5f;

// Shared ray-cache freshness threshold (§6.2): fast-moving targets (> 4.5 m/s) tighten
// maxAgeMs from 100 ms to 60 ms across BOTH acceptance lookup and off-guidance warmup.
inline constexpr uint32_t ResolveCandidateRayMaxAgeMs(float targetSpeedMps) noexcept
{
    return (targetSpeedMps > kFastTargetSpeedThresholdMps)
        ? kFastTargetRayMaxAgeMs
        : 100u;
}

// Shared Stage 0 protection-state hard filter seam (§1.2, §4.1):
// ONLY rejects enemy targets (isEnemy == true) with stale health samples (>500ms),
// imort == true, or barrprot == true. NEVER rejects allies receiving healing.
inline constexpr bool ProtectionFilterPasses(bool isEnemy,
                                             bool requireTargetable,
                                             bool imort,
                                             bool barrprot,
                                             uint32_t healthSampleTickMs,
                                             uint32_t nowMs,
                                             bool* outStaleHealthSample = nullptr) noexcept
{
    if (outStaleHealthSample) *outStaleHealthSample = false;
    if (!requireTargetable || !isEnemy)
        return true;
    const bool protectionFresh = healthSampleTickMs != 0 &&
        static_cast<uint32_t>(nowMs - healthSampleTickMs) <= 500u;
    if (!protectionFresh) {
        if (outStaleHealthSample) *outStaleHealthSample = true;
        return false;
    }
    return !imort && !barrprot;
}

} // namespace OW::TargetPipeline
