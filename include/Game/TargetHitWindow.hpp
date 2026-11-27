#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

#include "Game/AimArchitecture.hpp"
#include "Game/BoneSlots.hpp"
#include "Game/HeroGeometrySpec.hpp"
#include "Game/TriggerBoneSelection.hpp"
#include "config/UiPreferences.hpp"

namespace OW {

inline bool HitWindowAngleInside(const Vector3& viewAngle,
                                 const Vector3& targetAngle,
                                 const Vector3& camera,
                                 const Vector3& aimPoint,
                                 float radius)
{
    const float distance = camera.DistTo(aimPoint);
    if (!std::isfinite(distance) || distance <= 0.0001f || !std::isfinite(radius) || radius <= 0.0f)
        return false;
    return viewAngle.DistTo(targetAngle) <= radius / distance;
}

inline Vector3 HitWindowDirectionFromEuler(const Vector3& viewAngle)
{
    const float horizontal = std::cos(viewAngle.X);
    return Vector3(
        horizontal * std::sin(viewAngle.Y),
        -std::sin(viewAngle.X),
        horizontal * std::cos(viewAngle.Y));
}

inline Vector3 HitWindowEulerFromDirection(const Vector3& unitDirection)
{
    return Vector3(
        -std::asin(std::clamp(unitDirection.Y, -1.0f, 1.0f)),
        std::atan2(unitDirection.X, unitDirection.Z),
        0.0f);
}

inline std::optional<float> SelectedAimNativeHitScore(const TargetCandidate& candidate,
    const Vector3& viewAngle, const Vector3& camera, const Vector3& aimPoint,
    const Vector3& sampleDisplacement = {})
{
    if (!candidate.valid || candidate.nativeHitWindow.projectileRadius < 0.0f ||
        (candidate.aimPoint + sampleDisplacement).DistTo(aimPoint) > 0.0001f)
        return std::nullopt;

    const float distance = camera.DistTo(aimPoint);
    if (!std::isfinite(distance) || distance <= 0.0001f)
        return std::nullopt;
    const Vector3 direction = HitWindowDirectionFromEuler(viewAngle) * distance;
    return NativeHitWindowScore(candidate.entitySnapshot, candidate.boneId,
        GetTickCount(), camera, direction, candidate.nativeHitWindow, sampleDisplacement,
        candidate.requireExplicitCritical);
}

inline bool InSelectedAimHitWindow(const TargetCandidate& candidate,
    const Vector3& viewAngle, const Vector3& targetAngle, const Vector3& camera,
    const Vector3& aimPoint)
{
    const bool contourExposed = candidate.contour.evidence.exposed && candidate.contour.evidence.fresh;
    RayDecision effectiveRay = candidate.ray;
    if (candidate.barrierPolicy != BarrierPolicy::WaitBarrierDrop)
        effectiveRay.barrierCheckRequired = false;
    if (!RayDecisionAllowsSelection(effectiveRay, contourExposed,
                                    UI::Preferences::ActiveRayBackend()))
        return false;
    if (NativeHitboxSampleUsable(candidate.entitySnapshot, GetTickCount())) {
        const auto score = SelectedAimNativeHitScore(candidate, viewAngle, camera, aimPoint);
        if (!score)
            return false;
        return *score <= 1.0f;
    }
    // Keep bone fallback contact centered on the true predicted hitbox rather
    // than any reverse-lateral steering bias applied to candidate.aimPoint.
    if (candidate.valid &&
        std::isfinite(candidate.predictedAimPoint.X) &&
        std::isfinite(candidate.predictedAimPoint.Y) &&
        std::isfinite(candidate.predictedAimPoint.Z) &&
        (candidate.predictedAimPoint.X != 0.0f ||
         candidate.predictedAimPoint.Y != 0.0f ||
         candidate.predictedAimPoint.Z != 0.0f) &&
        candidate.aimPoint.DistTo(aimPoint) <= 0.0001f) {
        const Vector3 delta = candidate.predictedAimPoint - camera;
        const float dist = delta.Size();
        if (std::isfinite(dist) && dist > 0.0001f) {
            const Vector3 centerAngle = HitWindowEulerFromDirection(delta / dist);
            return HitWindowAngleInside(viewAngle, centerAngle, camera,
                candidate.predictedAimPoint, candidate.effectiveHitWindow);
        }
    }
    return HitWindowAngleInside(viewAngle, targetAngle, camera, aimPoint,
        candidate.effectiveHitWindow);
}

inline bool InAllowedAimHitWindow(const TargetCandidate& candidate,
    const Vector3& viewAngle, const Vector3& targetAngle, const Vector3& camera,
    const Vector3& aimPoint, SkeletonBoneMask allowedBoneMask,
    const Vector3& sampleDisplacement = {})
{
    if (!candidate.valid)
        return false;
    const bool contourExposed = candidate.contour.evidence.exposed && candidate.contour.evidence.fresh;
    RayDecision effectiveRay = candidate.ray;
    if (candidate.barrierPolicy != BarrierPolicy::WaitBarrierDrop)
        effectiveRay.barrierCheckRequired = false;
    if (!RayDecisionAllowsSelection(effectiveRay, contourExposed,
                                    UI::Preferences::ActiveRayBackend()))
        return false;
    if (allowedBoneMask == 0)
        return InSelectedAimHitWindow(candidate, viewAngle, targetAngle, camera, aimPoint);

    const uint32_t now = GetTickCount();
    const bool requirePerBoneWorldClear =
        !contourExposed &&
        UI::Preferences::ActiveRayBackend() == static_cast<int>(RayDecisionPolicy::Hybrid);
    const bool requirePerBoneBarrierClear =
        requirePerBoneWorldClear &&
        candidate.barrierPolicy == BarrierPolicy::WaitBarrierDrop &&
        candidate.ray.barrierCheckRequired;
    if (NativeHitboxSampleUsable(candidate.entitySnapshot, now) &&
        candidate.nativeHitWindow.projectileRadius >= 0.0f &&
        (candidate.aimPoint + sampleDisplacement).DistTo(aimPoint) <= 0.0001f) {
        const float distance = camera.DistTo(aimPoint);
        if (!std::isfinite(distance) || distance <= 0.0001f)
            return false;
        const Vector3 direction = HitWindowDirectionFromEuler(viewAngle) * distance;
        bool checkedSelectedBone = false;
        for (std::size_t slot = 0; slot < Plexies20260609::kSkeletonSlotCount; ++slot) {
            if (!SkeletonBoneMaskIncludesRenderSlot(allowedBoneMask, slot))
                continue;
            const int boneId = Plexies20260609::HitboxBoneIdForRenderSlot(slot);
            if (boneId == Plexies20260609::kUnusedRenderSkeletonBone)
                continue;
            if (boneId == candidate.boneId) {
                checkedSelectedBone = true;
            } else if (requirePerBoneWorldClear) {
                // Under Hybrid world/barrier ray occlusion, guidance-bone Clear cannot authorize
                // firing at a different render slot unless THAT exact slot (left/right distinct)
                // also has fresh WorldClear (and fresh BarrierClear under WaitBarrierDrop).
                const uint32_t slotBit = slot < 32 ? (1u << static_cast<uint32_t>(slot)) : 0u;
                if ((candidate.worldClearSlotMask & slotBit) == 0)
                    continue;
                if (requirePerBoneBarrierClear && (candidate.barrierClearSlotMask & slotBit) == 0)
                    continue;
            }
            const bool slotCriticalOnly =
                (boneId == candidate.boneId) && candidate.requireExplicitCritical &&
                candidate.hitAcceptance == HitAcceptance::SelectedOnly;
            const auto score = NativeHitWindowScore(candidate.entitySnapshot, boneId,
                now, camera, direction, candidate.nativeHitWindow, sampleDisplacement,
                slotCriticalOnly);
            if (score && *score <= 1.0f)
                return true;
        }
        if (!checkedSelectedBone) {
            const auto score = NativeHitWindowScore(candidate.entitySnapshot, candidate.boneId,
                now, camera, direction, candidate.nativeHitWindow, sampleDisplacement,
                candidate.requireExplicitCritical && candidate.hitAcceptance == HitAcceptance::SelectedOnly);
            if (score && *score <= 1.0f)
                return true;
        }
        return false;
    }

    return InSelectedAimHitWindow(candidate, viewAngle, targetAngle, camera, aimPoint);
}

// Unified Phase 2/3 program-generated auto-fire gate (§5.4, §5.5, §8.2, Decision #4, #7):
// enforces valid Engageable tier, expired PeekEmerge ReactionDelayGate, policy-gated Barrier block
// (WaitBarrierDrop blocks Barrier; IgnoreBarrier honors §8.2 IgnoreBarrier x Barrier cell),
// and Fresh Clear (world + active Slot 3 barrier) under Hybrid ray policy.
inline bool CandidateAutoFireGateAllowed(const TargetCandidate& candidate,
                                         int activeRayBackend) noexcept
{
    if (!candidate.valid ||
        candidate.tier != VisibilityTier::Engageable ||
        candidate.reactionRemainingMs > 0.0f) {
        return false;
    }
    const bool waitBarrierDrop = candidate.barrierPolicy == BarrierPolicy::WaitBarrierDrop;
    if (waitBarrierDrop && candidate.hitKind == TraceHitKind::Barrier) {
        return false;
    }
    const bool contourExposed =
        candidate.contour.evidence.exposed && candidate.contour.evidence.fresh;
    RayDecision effectiveRay = candidate.ray;
    if (!waitBarrierDrop) {
        effectiveRay.barrierCheckRequired = false;
    }
    return RayDecisionAllowsAutoFire(effectiveRay, contourExposed, activeRayBackend);
}

// Shared Stage 3 tracking output & fire-eligibility decision seam (§5.4, §5.5, §8.2, F9):
// consumed directly by BehaviorController::RunTracking and exercised by offline selftests.
enum class TrackingStageDecision : uint8_t {
    NoOutput = 0,       // Ignored, PreAim Parked, Barrier PreAim, PreAim pending EdgeFan, or ReactionDelayGate active
    PreAimParkMove,     // PreAim Positioning with valid P_park (auto-fire strictly forbidden)
    EngagePullOnly,     // Engageable + reaction expired, auto-fire blocked (e.g. Hybrid vacuum grace)
    EngagePullAndFire,  // Engageable + reaction expired + auto-fire gate allowed
};

inline TrackingStageDecision EvaluateTrackingStageDecision(const TargetCandidate& candidate,
                                                           int activeRayBackend) noexcept
{
    if (!candidate.valid)
        return TrackingStageDecision::NoOutput;
    if (candidate.tier == VisibilityTier::PreAim) {
        if (candidate.barrierPolicy == BarrierPolicy::WaitBarrierDrop &&
            candidate.hitKind == TraceHitKind::Barrier) {
            return TrackingStageDecision::NoOutput;
        }
        return (candidate.preAimPhase == PreAimPhase::Positioning && candidate.preAimParkPointValid)
            ? TrackingStageDecision::PreAimParkMove
            : TrackingStageDecision::NoOutput;
    }
    if (candidate.tier != VisibilityTier::Engageable || candidate.reactionRemainingMs > 0.0f)
        return TrackingStageDecision::NoOutput;
    return CandidateAutoFireGateAllowed(candidate, activeRayBackend)
        ? TrackingStageDecision::EngagePullAndFire
        : TrackingStageDecision::EngagePullOnly;
}

// Decoupled HitAcceptance consumer for TargetCandidate (§7.2, §8.2).
// Enforces VisibilityTier::Engageable (hard-forbids PreAim/Ignored fire), expired ReactionDelayGate,
// and dispatches SelectedOnly vs AnyValidHitbox using candidate.acceptanceMask.
inline bool InCandidateHitWindow(const TargetCandidate& candidate,
    const Vector3& viewAngle, const Vector3& targetAngle, const Vector3& camera,
    const Vector3& aimPoint, SkeletonBoneMask fallbackAllowedMask = 0,
    const Vector3& sampleDisplacement = {})
{
    if (!CandidateAutoFireGateAllowed(candidate, UI::Preferences::ActiveRayBackend()))
        return false;
    if (candidate.hitAcceptance == HitAcceptance::SelectedOnly) {
        if (NativeHitboxSampleUsable(candidate.entitySnapshot, GetTickCount())) {
            const bool contourExposed = candidate.contour.evidence.exposed && candidate.contour.evidence.fresh;
            RayDecision effectiveRay = candidate.ray;
            if (candidate.barrierPolicy != BarrierPolicy::WaitBarrierDrop)
                effectiveRay.barrierCheckRequired = false;
            if (!RayDecisionAllowsSelection(effectiveRay, contourExposed,
                                            UI::Preferences::ActiveRayBackend()))
                return false;
            const auto score = SelectedAimNativeHitScore(
                candidate, viewAngle, camera, aimPoint, sampleDisplacement);
            return score && *score <= 1.0f;
        }
        return InSelectedAimHitWindow(candidate, viewAngle, targetAngle, camera, aimPoint);
    }

    const SkeletonBoneMask mask = candidate.acceptanceMask != 0
        ? candidate.acceptanceMask
        : fallbackAllowedMask;
    return InAllowedAimHitWindow(
        candidate, viewAngle, targetAngle, camera, aimPoint, mask, sampleDisplacement);
}

// Authoritative Stage 3 HitAcceptance consumer for TargetSolution (§8.1, §8.2).
inline bool InSolutionHitWindow(const TargetSolution& solution,
    const Vector3& viewAngle, const Vector3& targetAngle, const Vector3& camera,
    const Vector3& aimPoint, const Vector3& sampleDisplacement = {})
{
    if (!solution.valid || solution.tier != VisibilityTier::Engageable ||
        solution.reactionRemainingMs > 0.0f)
        return false;
    const TargetCandidate candidate = solution.ToCandidate();
    return InCandidateHitWindow(
        candidate, viewAngle, targetAngle, camera, aimPoint,
        solution.acceptanceMask, sampleDisplacement);
}

} // namespace OW
