#include "Game/AimArchitecture.hpp"
#include "Game/AimFirePhase.hpp"
#include "Game/AimPlan.hpp"
#include "Game/IntentEngine.hpp"
#include "Game/LeadPrediction.hpp"
#include "Game/PreAimState.hpp"
#include "Game/TargetHitWindow.hpp"
#include "Game/TargetPipeline.hpp"
#include "Game/TargetSolutionTypes.hpp"
#include "Game/TrajectoryRayPlanner.hpp"
#include "config/AimTargetMigration.hpp"

#include <rapidjson/document.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

// Offline test harness stubs so the pure geometry/pipeline test never loads DMA DLLs.
Memory::~Memory() {}

namespace UI::Preferences {
int g_testRayBackend = 0; // 0 = NativeOnly, 1 = Hybrid
int ActiveRayBackend() noexcept { return g_testRayBackend; }
}

namespace OW {
namespace {
int g_fixtureHitBoneId = -1;
bool g_fixtureHitIsCritical = false;
}

bool NativeHitboxSampleUsable(const c_entity&, uint32_t) { return true; }

std::optional<float> NativeHitWindowScore(const c_entity&, int boneId, uint32_t,
                                          const Vector3&, const Vector3&,
                                          const NativeHitWindowInput&,
                                          const Vector3&,
                                          bool requireExplicitCritical)
{
    if (boneId != g_fixtureHitBoneId)
        return std::optional<float>{2.5f};
    if (requireExplicitCritical && !g_fixtureHitIsCritical)
        return std::optional<float>{2.5f};
    return std::optional<float>{0.5f};
}
} // namespace OW

namespace {

bool Near(float a, float b, float eps = 1e-3f)
{
    return std::fabs(a - b) <= eps;
}

int Fail(const char* msg)
{
    std::cerr << "FAIL " << msg << '\n';
    return EXIT_FAILURE;
}

bool TestIntentEngine()
{
    // 1. Yaw wrap-around at +/- 180 deg seam.
    if (!Near(OW::WrapDegrees180(179.5f - (-179.5f)), -1.0f, 1e-4f))
        return false;
    if (!Near(OW::WrapDegrees180(-179.5f - 179.5f), 1.0f, 1e-4f))
        return false;
    if (!Near(OW::WrapDegrees180(-180.0f), 180.0f, 1e-4f))
        return false;
    if (!Near(OW::WrapDegrees180(std::numeric_limits<float>::quiet_NaN()), 0.0f, 1e-4f))
        return false;

    // 2. Pipeline-delay ring buffer cancels program-sent motion without phantom residual.
    OW::IntentEngineConfig cfg{};
    cfg.pipelineDelayUs = 8000;
    cfg.noiseFloorDeg = 0.05f;
    cfg.activeDragThresholdDeg = 0.06f;
    cfg.activeDragMinFrames = 2;
    OW::IntentEngine engine(cfg);

    uint64_t t = 1000000;
    engine.ObserveCamera(t, 0.0f, 0.0f, 1, true);
    engine.RecordProgramSent(t, 2.0f, -0.5f); // Expected to arrive at t + 8000 us.

    t += 4000; // t + 4 ms: command not yet arrived, camera still at 0.
    auto snap = engine.ObserveCamera(t, 0.0f, 0.0f, 1, true);
    if (!snap.residualValid || !Near(snap.residualNormDeg, 0.0f, 1e-4f))
        return false;

    t += 4000; // t + 8 ms: command arrives, camera moves (+2.0, -0.5).
    snap = engine.ObserveCamera(t, 2.0f, -0.5f, 1, true);
    if (!snap.residualValid || !Near(snap.residualYawDeg, 0.0f, 1e-4f) ||
        !Near(snap.residualPitchDeg, 0.0f, 1e-4f)) {
        return false;
    }

    // 3. Teleport / respawn jump (> 15 deg/frame) must be rejected without polluting state.
    t += 5000;
    snap = engine.ObserveCamera(t, 28.0f, -0.5f, 1, true);
    if (snap.residualValid || snap.activeDrag)
        return false;

    // 4. Opposite-direction drag attenuation: fast 12ms decay vs slow 100ms recovery.
    engine.Reset();
    t = 2000000;
    float camYaw = 0.0f;
    engine.ObserveCamera(t, camYaw, 0.0f, 1, true);
    for (int i = 0; i < 6; ++i) {
        t += 5000; // 5 ms per step -> 30 ms total
        camYaw -= 0.6f; // Human pulls left (-yaw) while target error is right (+5 deg)
        engine.ObserveCamera(t, camYaw, 0.0f, 1, true);
        snap = engine.UpdateActiveTargetFeedback(5.0f, 0.0f, true);
    }
    if (!snap.activeDrag || snap.activeTargetAlignment > -0.95f)
        return false;
    // After 30ms with tau=12ms, gain should drop below 0.12.
    if (snap.activeTargetGain >= 0.12f)
        return false;
    // Opposite-direction challenger must be hard-blocked while dragging.
    if (!engine.IsOppositeDragHardBlocked(5.0f, 0.0f, false))
        return false;
    // Currently locked target is NOT hard-blocked by challenger filter (uses reverse break instead).
    if (engine.IsOppositeDragHardBlocked(5.0f, 0.0f, true))
        return false;
    // Aligned challenger on the left (-5 deg) must NOT be blocked.
    if (engine.IsOppositeDragHardBlocked(-5.0f, 0.0f, false))
        return false;

    // Release pull -> 100ms slow recovery (should still be damped after 40ms, mostly recovered after 300ms).
    for (int i = 0; i < 8; ++i) {
        t += 5000; // +40 ms
        engine.ObserveCamera(t, camYaw, 0.0f, 1, true);
        snap = engine.UpdateActiveTargetFeedback(5.0f, 0.0f, true);
    }
    if (snap.activeTargetGain <= 0.20f || snap.activeTargetGain >= 0.65f)
        return false;
    for (int i = 0; i < 52; ++i) {
        t += 5000; // +260 ms -> 300 ms total
        engine.ObserveCamera(t, camYaw, 0.0f, 1, true);
        snap = engine.UpdateActiveTargetFeedback(5.0f, 0.0f, true);
    }
    if (snap.activeTargetGain < 0.90f)
        return false;

    // 5. Reverse lock-break (>1.5 deg over >=45 ms) vs sudden-stop reversal exemption (Decision #1).
    engine.Reset();
    t = 3000000;
    camYaw = 0.0f;
    engine.ObserveCamera(t, camYaw, 0.0f, 1, true);
    for (int i = 0; i < 10; ++i) {
        t += 5000; // 50 ms
        camYaw -= 0.25f; // 2.5 deg cumulative reverse pull
        engine.ObserveCamera(t, camYaw, 0.0f, 1, true);
        // Target suddenly reversed leftward (-30 deg/s), player anticipates leftward: exempt!
        snap = engine.UpdateActiveTargetFeedback(5.0f, 0.0f, true, true, -30.0f, 0.0f);
    }
    if (!snap.reverseExemptByTargetReversal || snap.reverseBreakTriggered)
        return false;
    // Gain still attenuated smoothly even when lock-break is exempted!
    if (snap.activeTargetGain >= 0.15f)
        return false;

    // Without target reversal exemption, sustained opposite pull MUST trigger lock-break!
    for (int i = 0; i < 10; ++i) {
        t += 5000; // 50 ms
        camYaw -= 0.25f; // 2.5 deg cumulative reverse pull
        engine.ObserveCamera(t, camYaw, 0.0f, 1, true);
        snap = engine.UpdateActiveTargetFeedback(5.0f, 0.0f, true, false);
    }
    if (!snap.reverseBreakTriggered)
        return false;

    // 6. IdleHold stance: enters after 200ms with no committed target, shrinks FOV,
    //    relaxes for PreAim/PeekEmerge, and recovers in 1 frame on human drag.
    engine.Reset();
    t = 4000000;
    camYaw = 10.0f;
    engine.ObserveCamera(t, camYaw, 0.0f, 1, false);
    // 150ms idle: still ActiveDrag.
    for (int i = 0; i < 15; ++i) {
        t += 10000;
        snap = engine.ObserveCamera(t, camYaw, 0.0f, 1, false);
    }
    if (snap.stance != OW::InputStance::ActiveDrag)
        return false;
    if (!Near(engine.EffectiveSelectionFovDeg(16.0f), 16.0f, 1e-3f))
        return false;

    // Continue idle to 600ms total -> IdleHold and shrunk near 5.5 deg.
    for (int i = 0; i < 45; ++i) {
        t += 10000;
        snap = engine.ObserveCamera(t, camYaw, 0.0f, 1, false);
    }
    if (snap.stance != OW::InputStance::IdleHold)
        return false;
    const float shrunkFov = engine.EffectiveSelectionFovDeg(16.0f);
    if (shrunkFov >= 6.5f || shrunkFov < 5.4f)
        return false;
    // PreAim/PeekEmerge exemption relaxes FOV to min(activeFov, peekGateDeg).
    const float peekFov = engine.EffectiveSelectionFovDeg(16.0f, true, 9.8f);
    if (!Near(peekFov, 9.8f, 1e-3f))
        return false;

    // Single frame of human drag (> noiseFloorDeg) must restore ActiveDrag & full 16 deg immediately.
    t += 10000;
    camYaw += 0.3f;
    snap = engine.ObserveCamera(t, camYaw, 0.0f, 1, false);
    if (snap.stance != OW::InputStance::ActiveDrag ||
        !Near(engine.EffectiveSelectionFovDeg(16.0f), 16.0f, 1e-3f)) {
        return false;
    }

    // Committed target prevents entering IdleHold even if stationary for 400ms.
    for (int i = 0; i < 40; ++i) {
        t += 10000;
        snap = engine.ObserveCamera(t, camYaw, 0.0f, 1, true);
    }
    if (snap.stance != OW::InputStance::ActiveDrag)
        return false;

    // 7. Camera publishSequence deduplication (Finding #2):
    //    Consumer polls every 2ms while camera publishes every 10ms.
    //    Duplicate polls MUST NOT inject zero-motion frames, MUST NOT break activeDragMinFrames=2,
    //    and MUST NOT prematurely consume delayed commands.
    engine.Reset();
    t = 5000000;
    camYaw = 0.0f;
    engine.ObserveCamera(t, camYaw, 0.0f, 1, true, 100);
    engine.RecordProgramSent(t, 1.5f, 0.0f); // Arrives at t + 8000 us.
    // 3 duplicate consumer polls of seq 100 at +2ms, +4ms, +6ms, +8ms (still old camera frame!):
    for (int dup = 1; dup <= 4; ++dup) {
        auto dupSnap = engine.ObserveCamera(t + static_cast<uint64_t>(dup) * 2000, camYaw, 0.0f, 1, true, 100);
        if (dupSnap.sampleUpdated)
            return false;
    }
    // New camera sample seq 101 arrives at +10ms with +1.5 deg program move + (-0.5 deg) human drag:
    t += 10000;
    camYaw += 1.0f; // +1.5 program - 0.5 human
    snap = engine.ObserveCamera(t, camYaw, 0.0f, 1, true, 101);
    if (!snap.sampleUpdated || !snap.residualValid || !Near(snap.residualYawDeg, -0.5f, 1e-4f) ||
        snap.dragConsecutiveFrames != 1) {
        return false;
    }
    // Intervening duplicate polls of seq 101 must NOT reset dragConsecutiveFrames!
    for (int dup = 1; dup <= 4; ++dup) {
        auto dupSnap = engine.ObserveCamera(t + static_cast<uint64_t>(dup) * 2000, camYaw, 0.0f, 1, true, 101);
        if (dupSnap.sampleUpdated || dupSnap.dragConsecutiveFrames != 1)
            return false;
    }
    // Next distinct camera sample seq 102 with -0.5 deg human drag -> reaches activeDragMinFrames == 2!
    t += 10000;
    camYaw -= 0.5f;
    snap = engine.ObserveCamera(t, camYaw, 0.0f, 1, true, 102);
    if (!snap.sampleUpdated || !snap.activeDrag || snap.dragConsecutiveFrames != 2)
        return false;

    return true;
}

bool TestUtilityAndFirePhaseCommitment()
{
    using namespace OW::TargetPipeline;

    // 1. Bounded legacy AimPlan::preferRules (B2 fix):
    //    Center target at 0.5 deg MUST beat edge preferred target at 14.0 deg.
    UtilityInput centerNonPref{};
    centerNonPref.angleDeg = 0.5f;
    centerNonPref.effectiveFovDeg = 15.0f;
    centerNonPref.visibilityPrior = 0.7f;
    centerNonPref.planPreferred = false;

    UtilityInput edgePref{};
    edgePref.angleDeg = 14.0f;
    edgePref.effectiveFovDeg = 15.0f;
    edgePref.visibilityPrior = 0.7f;
    edgePref.planPreferred = true;

    const auto uCenter = ComputeCandidateUtility(centerNonPref);
    const auto uEdgePref = ComputeCandidateUtility(edgePref);
    if (!(uCenter.total > uEdgePref.total))
        return false;

    // Near-center preferred target (3.0 deg) beats non-preferred target at 4.0 deg.
    UtilityInput nearPref = centerNonPref;
    nearPref.angleDeg = 3.0f;
    nearPref.planPreferred = true;
    UtilityInput midNonPref = centerNonPref;
    midNonPref.angleDeg = 4.0f;
    midNonPref.planPreferred = false;
    if (!(ComputeCandidateUtility(nearPref).total > ComputeCandidateUtility(midNonPref).total))
        return false;

    // 2. AimFirePhase anchoring blocks ordinary crossing passerby,
    //    but allows high-value killable/preferred center target + aligned human pull (Decision #3).
    OW::AimFirePhase::Tuning tuning{};
    tuning.shotIntervalMs = 250.0f;
    tuning.pauseMs = 35.0f;
    tuning.recoveryMs = 90.0f;
    tuning.preFireBoostWindowMs = 45.0f;

    const auto preFire = OW::AimFirePhase::Resolve(220.0f, true, tuning); // 30ms before next shot
    if (!preFire.preFireBoost || !Near(ComputeFirePhaseCommitment(preFire, true), 1.0f))
        return false;
    const auto postFirePause = OW::AimFirePhase::Resolve(15.0f, true, tuning);
    if (!postFirePause.paused || !Near(ComputeFirePhaseCommitment(postFirePause, true), 1.0f))
        return false;
    const auto recovering = OW::AimFirePhase::Resolve(60.0f, true, tuning);
    if (!recovering.recovering || !Near(ComputeFirePhaseCommitment(recovering, true), 0.75f))
        return false;

    UtilityInput lockedTarget{};
    lockedTarget.angleDeg = 2.0f;
    lockedTarget.effectiveFovDeg = 15.0f;
    lockedTarget.isCurrentLocked = true;
    lockedTarget.dwellCommitment = ComputeDwellCommitment(260.0f);
    lockedTarget.firePhaseCommitment = ComputeFirePhaseCommitment(preFire, true);
    lockedTarget.retargetHysteresis = 100.0f;
    lockedTarget.visibilityPrior = 1.0f;

    UtilityInput passerby{};
    passerby.angleDeg = 1.0f; // Closer to crosshair, but ordinary full-HP passerby with no player pull
    passerby.effectiveFovDeg = 15.0f;
    passerby.isCurrentLocked = false;
    passerby.visibilityPrior = 1.0f;

    const auto uLocked = ComputeCandidateUtility(lockedTarget);
    const auto uPasserby = ComputeCandidateUtility(passerby);
    if (!(uLocked.total > uPasserby.total))
        return false;

    // High-value killable/preferred target at center + player pulling toward it breaks through!
    UtilityInput lockedWhilePullingAway = lockedTarget;
    lockedWhilePullingAway.intentActiveDrag = true;
    lockedWhilePullingAway.intentAlignment = -0.5f;
    lockedWhilePullingAway.intentConfidence = 1.0f;

    UtilityInput highValueCenter{};
    highValueCenter.angleDeg = 0.3f;
    highValueCenter.effectiveFovDeg = 15.0f;
    highValueCenter.isCurrentLocked = false;
    highValueCenter.intentActiveDrag = true;
    highValueCenter.intentAlignment = 0.9f;
    highValueCenter.intentConfidence = 1.0f;
    highValueCenter.visibilityPrior = 1.0f;
    highValueCenter.planPreferred = true;
    highValueCenter.roleUtility = 0.25f;

    const auto uLockedAway = ComputeCandidateUtility(lockedWhilePullingAway);
    const auto uHighValue = ComputeCandidateUtility(highValueCenter);
    if (uHighValue.hardBlockedByIntent || !(uHighValue.total > uLockedAway.total))
        return false;

    // Opposite-direction active drag must hard-block challenger.
    UtilityInput oppositeChallenger = passerby;
    oppositeChallenger.intentActiveDrag = true;
    oppositeChallenger.intentAlignment = -0.8f;
    oppositeChallenger.intentConfidence = 1.0f;
    if (!ComputeCandidateUtility(oppositeChallenger).hardBlockedByIntent)
        return false;

    // 3. Velocity reversal detector.
    if (!DetectTargetVelocityReversal(OW::Vector3(5.0f, 0.0f, 0.0f), OW::Vector3(-4.5f, 0.0f, 0.0f)))
        return false;
    if (!DetectTargetVelocityReversal(OW::Vector3(5.0f, 0.0f, 0.0f), OW::Vector3(0.4f, 0.0f, 0.0f)))
        return false;
    if (DetectTargetVelocityReversal(OW::Vector3(5.0f, 0.0f, 0.0f), OW::Vector3(4.8f, 0.0f, 0.4f)))
        return false;
    if (DetectTargetVelocityReversal(OW::Vector3(0.5f, 0.0f, 0.0f), OW::Vector3(-0.5f, 0.0f, 0.0f)))
        return false;

    // 4. Priority mode 3 (weighted HP-only) & predictedFovEntry penalty parity (Finding #15).
    UtilityInput hpLow{};
    hpLow.priorityMode = 3;
    hpLow.angleWeight = 0.0f;
    hpLow.distanceWeight = 0.0f;
    hpLow.healthWeight = 10.0f;
    hpLow.angleDeg = 8.0f;
    hpLow.hp = 30.0f;
    hpLow.maxHp = 200.0f;

    UtilityInput hpFull = hpLow;
    hpFull.angleDeg = 1.0f;
    hpFull.hp = 200.0f;
    if (!(ComputeCandidateUtility(hpLow).total > ComputeCandidateUtility(hpFull).total))
        return false;

    UtilityInput insideFov{};
    insideFov.angleDeg = 3.0f;
    insideFov.predictedFovEntry = false;
    UtilityInput predEntry = insideFov;
    predEntry.predictedFovEntry = true;
    if (!Near(ComputeCandidateUtility(insideFov).total - ComputeCandidateUtility(predEntry).total, 0.10f, 1e-4f))
        return false;

    // 5. Normalized Euler pitch/yaw for elevated displacements (Finding #1):
    //    (0, 5, 20) must yield ~-14.036 deg pitch (not NaN!) and match (0, 50, 200).
    auto computeNormPitchDeg = [](const OW::Vector3& d) {
        const float len = d.Size();
        const OW::Vector3 u = d / len;
        return RAD2DEG(-std::asin(std::clamp(u.Y, -1.0f, 1.0f)));
    };
    const float pitchNear = computeNormPitchDeg(OW::Vector3(0.0f, 5.0f, 20.0f));
    const float pitchFar = computeNormPitchDeg(OW::Vector3(0.0f, 50.0f, 200.0f));
    if (!std::isfinite(pitchNear) || !Near(pitchNear, -14.0362f, 0.01f) || !Near(pitchNear, pitchFar, 1e-4f))
        return false;

    return true;
}

bool TestBoneDecouplingAndStage0()
{
    using namespace OW::TargetPipeline;

    // 1. BodyCenter + AnyValidHitbox + All bones (Ana/Zarya use-case, B3 fix):
    //    Guidance pulls ONLY to Chest/Pelvis (zero limbs, zero Head), while acceptance accepts ALL bones.
    const auto bodyLadder = BuildGuidanceBoneLadder(
        OW::AimPriority::BodyCenter, OW::kSkeletonBoneAllMask, 15.0f, 100.0f, false);
    if (bodyLadder.nearestMode || bodyLadder.count != 2 ||
        bodyLadder.groups[0] != OW::kSkeletonBoneChest ||
        bodyLadder.groups[1] != OW::kSkeletonBonePelvis) {
        return false;
    }
    const auto bodyAccept = ResolveAcceptanceMask(
        OW::HitAcceptance::AnyValidHitbox, OW::kSkeletonBoneAllMask, 15.0f, 100.0f, OW::BONE_CHEST);
    if (bodyAccept != OW::kSkeletonBoneAllMask)
        return false;

    // 2. HeadThenBody hard cutoff at torso core (Decision #6):
    const auto htbCore = BuildGuidanceBoneLadder(
        OW::AimPriority::HeadThenBody, OW::kSkeletonBoneAllMask, 15.0f, 100.0f, false);
    if (htbCore.nearestMode || htbCore.count != 4 ||
        htbCore.groups[0] != OW::kSkeletonBoneHead ||
        htbCore.groups[1] != OW::kSkeletonBoneNeck ||
        htbCore.groups[2] != OW::kSkeletonBoneChest ||
        htbCore.groups[3] != OW::kSkeletonBonePelvis) {
        return false;
    }
    // Execute fallback allows limbs after torso core.
    const auto htbExec = BuildGuidanceBoneLadder(
        OW::AimPriority::HeadThenBody, OW::kSkeletonBoneAllMask, 15.0f, 100.0f, true);
    if (htbExec.count <= 4)
        return false;

    // 3. HeadOnly + SelectedOnly: strict head, empty past maxHeadDistance, acceptance=0 (selected bone only).
    const auto headNear = BuildGuidanceBoneLadder(
        OW::AimPriority::HeadOnly, OW::kSkeletonBoneAllMask, 20.0f, 30.0f, false);
    const auto headFar = BuildGuidanceBoneLadder(
        OW::AimPriority::HeadOnly, OW::kSkeletonBoneAllMask, 45.0f, 30.0f, false);
    if (headNear.count != 1 || headNear.groups[0] != OW::kSkeletonBoneHead || !headFar.Empty())
        return false;
    if (ResolveAcceptanceMask(OW::HitAcceptance::SelectedOnly, OW::kSkeletonBoneAllMask, 20.0f, 30.0f, OW::BONE_HEAD) != 0)
        return false;

    // 4. NeckSpread: Neck -> Chest -> Head -> Pelvis (no limbs).
    const auto spread = BuildGuidanceBoneLadder(
        OW::AimPriority::NeckSpread, OW::kSkeletonBoneAllMask, 10.0f, 100.0f, false);
    if (spread.nearestMode || spread.count != 4 ||
        spread.groups[0] != OW::kSkeletonBoneNeck ||
        spread.groups[1] != OW::kSkeletonBoneChest) {
        return false;
    }

    // 5. NearestHitbox enables nearestMode across allowed mask.
    const auto nearest = BuildGuidanceBoneLadder(
        OW::AimPriority::NearestHitbox, OW::kSkeletonBoneAllMask, 10.0f, 100.0f, false);
    if (!nearest.nearestMode || nearest.nearestMask != OW::kSkeletonBoneAllMask)
        return false;

    // 6. Stage 0 AimPlan rule check (B1 fix): Visible onlyRule must NOT kill in Stage 0,
    //    while Hero/Health onlyRules MUST still filter in Stage 0.
    OW::AimPlan::Selection sel{};
    OW::AimPlan::SelectionRule visRule{};
    visRule.measure = static_cast<int>(OW::AimPlan::RuleMeasure::Visible);
    OW::AimPlan::SelectionRule hpRule{};
    hpRule.measure = static_cast<int>(OW::AimPlan::RuleMeasure::Health);
    hpRule.minValue = 1.0f;
    hpRule.maxValue = 150.0f;
    sel.onlyRules = {visRule, hpRule};
    OW::AimPlan::Effective eff{};

    if (OW::AimPlan::EligiblePreGeometry(sel, eff, 1, 100.0f, 200.0f, false))
        return false;
    if (!OW::AimPlan::EligiblePreGeometryStage0(sel, eff, 1, 100.0f, 200.0f))
        return false;
    if (OW::AimPlan::EligiblePreGeometryStage0(sel, eff, 1, 190.0f, 200.0f))
        return false;

    // 7. TargetSolution <-> TargetCandidate contract round-trip.
    OW::TargetSolution sol{};
    sol.valid = true;
    sol.tier = OW::VisibilityTier::Engageable;
    sol.transition = OW::TransitionKind::BoneFallback;
    sol.aimPriority = OW::AimPriority::BodyCenter;
    sol.hitAcceptance = OW::HitAcceptance::AnyValidHitbox;
    sol.guidanceBoneId = OW::BONE_CHEST;
    sol.acceptanceMask = OW::kSkeletonBoneAllMask;
    sol.utilityScore = 1.42f;
    sol.commitment = 0.85f;
    sol.intentAlignment = 0.6f;
    sol.intentGain = 0.75f;
    sol.stance = OW::InputStance::IdleHold;
    sol.hitKind = OW::TraceHitKind::None;
    sol.entityKey = 0x123456;
    sol.actorGeneration = 9;
    const auto cand = sol.ToCandidate();
    const auto roundTrip = OW::TargetSolution::FromCandidate(cand);
    if (!roundTrip.valid || roundTrip.tier != OW::VisibilityTier::Engageable ||
        roundTrip.transition != OW::TransitionKind::BoneFallback ||
        roundTrip.aimPriority != OW::AimPriority::BodyCenter ||
        roundTrip.hitAcceptance != OW::HitAcceptance::AnyValidHitbox ||
        roundTrip.guidanceBoneId != OW::BONE_CHEST ||
        roundTrip.acceptanceMask != OW::kSkeletonBoneAllMask ||
        !Near(roundTrip.utilityScore, 1.42f) ||
        !Near(roundTrip.commitment, 0.85f) ||
        !Near(roundTrip.intentGain, 0.75f) ||
        roundTrip.stance != OW::InputStance::IdleHold ||
        roundTrip.hitKind != OW::TraceHitKind::None ||
        roundTrip.actorGeneration != 9) {
        return false;
    }

    // 8. Production TargetHitWindow InSolutionHitWindow / InCandidateHitWindow enforcement:
    //    PreAim/Ignored hard-blocked; SelectedOnly checks only guidanceBoneId; AnyValidHitbox checks acceptanceMask.
    {
        OW::TargetSolution hitSol{};
        hitSol.valid = true;
        hitSol.tier = OW::VisibilityTier::Engageable;
        hitSol.aimPriority = OW::AimPriority::BodyCenter;
        hitSol.hitAcceptance = OW::HitAcceptance::SelectedOnly;
        hitSol.guidanceBoneId = OW::BONE_CHEST;
        hitSol.guidanceAimPoint = OW::Vector3(0.0f, 0.0f, 10.0f);
        hitSol.acceptanceMask = OW::kSkeletonBoneAllMask;
        hitSol.nativeHitWindow.projectileRadius = 0.05f;
        hitSol.ray.actor = OW::RayDecisionStatus::Hit;
        hitSol.ray.world = OW::RayDecisionStatus::Clear;

        const OW::Vector3 viewAngle(0.0f, 0.0f, 0.0f);
        const OW::Vector3 camera(0.0f, 0.0f, 0.0f);
        const int footBoneId = OW::Plexies20260609::HitboxBoneIdForRenderSlot(14);

        // Crosshair is on foot bone while guidance is Chest:
        OW::g_fixtureHitBoneId = footBoneId;
        // SelectedOnly MUST reject foot hit!
        if (OW::InSolutionHitWindow(hitSol, viewAngle, viewAngle, camera, hitSol.guidanceAimPoint))
            return false;

        // AnyValidHitbox with All mask MUST accept foot hit while guidance remains Chest!
        hitSol.hitAcceptance = OW::HitAcceptance::AnyValidHitbox;
        if (!OW::InSolutionHitWindow(hitSol, viewAngle, viewAngle, camera, hitSol.guidanceAimPoint))
            return false;

        // PreAim and Ignored MUST be hard-rejected even when crosshair hits the exact guidance bone!
        OW::g_fixtureHitBoneId = OW::BONE_CHEST;
        hitSol.tier = OW::VisibilityTier::PreAim;
        if (OW::InSolutionHitWindow(hitSol, viewAngle, viewAngle, camera, hitSol.guidanceAimPoint))
            return false;
        hitSol.tier = OW::VisibilityTier::Ignored;
        if (OW::InSolutionHitWindow(hitSol, viewAngle, viewAngle, camera, hitSol.guidanceAimPoint))
            return false;

        // Engageable + SelectedOnly hitting Chest MUST pass!
        hitSol.tier = OW::VisibilityTier::Engageable;
        hitSol.hitAcceptance = OW::HitAcceptance::SelectedOnly;
        if (!OW::InSolutionHitWindow(hitSol, viewAngle, viewAngle, camera, hitSol.guidanceAimPoint))
            return false;

        // Finding #16: explicit Critical shape enforcement under HeadOnly + SelectedOnly.
        hitSol.aimPriority = OW::AimPriority::HeadOnly;
        hitSol.hitAcceptance = OW::HitAcceptance::SelectedOnly;
        hitSol.guidanceBoneId = OW::BONE_HEAD;
        hitSol.requireExplicitCritical = true;
        OW::g_fixtureHitBoneId = OW::BONE_HEAD;
        OW::g_fixtureHitIsCritical = false; // Non-critical shape sharing BONE_HEAD MUST be rejected!
        if (OW::InSolutionHitWindow(hitSol, viewAngle, viewAngle, camera, hitSol.guidanceAimPoint))
            return false;
        OW::g_fixtureHitIsCritical = true; // Explicit Critical shape MUST pass!
        if (!OW::InSolutionHitWindow(hitSol, viewAngle, viewAngle, camera, hitSol.guidanceAimPoint))
            return false;

        // R2 fix: Hybrid AnyValidHitbox requires per-render-slot worldClearSlotMask (left/right distinct).
        const int leftFootBoneId = OW::Plexies20260609::HitboxBoneIdForRenderSlot(14);  // left foot (slot 14)
        const int rightFootBoneId = OW::Plexies20260609::HitboxBoneIdForRenderSlot(15); // right foot (slot 15)
        UI::Preferences::g_testRayBackend = 1; // Hybrid
        hitSol.hitAcceptance = OW::HitAcceptance::AnyValidHitbox;
        hitSol.requireExplicitCritical = false;
        hitSol.guidanceBoneId = OW::BONE_HEAD;
        hitSol.acceptanceMask = OW::kSkeletonBoneAllMask;
        // Head + left foot clear; right foot is behind cover (shares kSkeletonBoneFeet group!).
        hitSol.worldClearSlotMask =
            OW::TargetPipeline::RenderSlotBitForBoneId(OW::BONE_HEAD) |
            OW::TargetPipeline::RenderSlotBitForBoneId(leftFootBoneId);
        OW::g_fixtureHitBoneId = rightFootBoneId;
        if (OW::InSolutionHitWindow(hitSol, viewAngle, viewAngle, camera, hitSol.guidanceAimPoint)) {
            UI::Preferences::g_testRayBackend = 0;
            return false; // Right foot behind cover MUST NOT be authorized by left foot!
        }
        OW::g_fixtureHitBoneId = leftFootBoneId;
        if (!OW::InSolutionHitWindow(hitSol, viewAngle, viewAngle, camera, hitSol.guidanceAimPoint)) {
            UI::Preferences::g_testRayBackend = 0;
            return false; // Left foot has its own fresh clear slot bit and MUST pass!
        }
        // Once right foot also gets its own slot bit, right foot passes too.
        hitSol.worldClearSlotMask |= OW::TargetPipeline::RenderSlotBitForBoneId(rightFootBoneId);
        OW::g_fixtureHitBoneId = rightFootBoneId;
        if (!OW::InSolutionHitWindow(hitSol, viewAngle, viewAngle, camera, hitSol.guidanceAimPoint)) {
            UI::Preferences::g_testRayBackend = 0;
            return false;
        }
        UI::Preferences::g_testRayBackend = 0;

        // R6 fix: verify reversal ease curve has an initial reaction delay floor followed by smooth ramp.
        const float easeAt0 = OW::TargetPipeline::ComputeReversalEaseScale(true, 0.0f);
        const float easeAt20 = OW::TargetPipeline::ComputeReversalEaseScale(true, 20.0f);
        const float easeAt95 = OW::TargetPipeline::ComputeReversalEaseScale(true, 95.0f);
        const float easeAt160 = OW::TargetPipeline::ComputeReversalEaseScale(true, 160.0f);
        if (!(easeAt0 <= 0.20f && std::fabs(easeAt20 - easeAt0) < 1e-4f &&
              easeAt95 > easeAt20 + 0.25f && easeAt95 < 0.90f && easeAt160 >= 0.999f)) {
            return false;
        }

        // Round 3 HIGH #1 regression: off-guidance acceptance ray endpoint MUST shift by predicted
        // displacement so raw foot outside cover never authorizes a predicted foot behind cover.
        OW::NativeHitWindowInput hitscanPred{};
        hitscanPred.projectileSpeed = 0.0f;
        hitscanPred.displacement = OW::Vector3(1.5f, 0.0f, 0.0f); // target moving 1.5m into cover
        const OW::Vector3 rawFoot(0.0f, 0.0f, 10.0f);
        const OW::Vector3 rawChest(0.0f, 1.2f, 10.0f);
        const OW::Vector3 predChest(1.5f, 1.2f, 10.0f);
        const OW::Vector3 predFoot = OW::TargetPipeline::ComputeAcceptanceRayEndpoint(
            rawFoot, rawChest, predChest, hitscanPred);
        if (predFoot.DistTo(OW::Vector3(1.5f, 0.0f, 10.0f)) > 1e-4f ||
            predFoot.DistTo(rawFoot) < 1.0f) {
            return false;
        }
        OW::NativeHitWindowInput projPred{};
        projPred.projectileSpeed = 110.0f;
        projPred.displacement = OW::Vector3(0.2f, 0.0f, 0.0f);
        const OW::Vector3 projPredFoot = OW::TargetPipeline::ComputeAcceptanceRayEndpoint(
            rawFoot, rawChest, OW::Vector3(2.0f, 1.4f, 10.0f), projPred);
        if (projPredFoot.DistTo(OW::Vector3(2.0f, 0.2f, 10.0f)) > 1e-4f) {
            return false;
        }

        // Round 3 MEDIUM #2 regression: Observe(new) -> Observe(duplicate) -> UpdateActiveTargetFeedback
        // must NOT drop pending feedback for the fresh sample.
        OW::IntentEngine dupEngine;
        dupEngine.ObserveCamera(1000, 0.0f, 0.0f, 1, true, 10);
        dupEngine.UpdateActiveTargetFeedback(5.0f, 0.0f, true);
        dupEngine.ObserveCamera(6000, -1.0f, 0.0f, 1, true, 11);  // fresh reverse drag
        dupEngine.ObserveCamera(7000, -1.0f, 0.0f, 1, true, 11);  // duplicate sequence before feedback!
        const OW::IntentFrameSnapshot afterDupFeedback =
            dupEngine.UpdateActiveTargetFeedback(5.0f, 0.0f, true);
        if (!(afterDupFeedback.activeTargetGain < 0.75f && afterDupFeedback.reverseAccumDeg > 0.5f)) {
            return false;
        }
        // Calling feedback a second time without a new sample must be a no-op (no double integration).
        const OW::IntentFrameSnapshot afterRepeatFeedback =
            dupEngine.UpdateActiveTargetFeedback(5.0f, 0.0f, true);
        if (std::fabs(afterRepeatFeedback.activeTargetGain - afterDupFeedback.activeTargetGain) > 1e-6f ||
            std::fabs(afterRepeatFeedback.reverseAccumDeg - afterDupFeedback.reverseAccumDeg) > 1e-6f) {
            return false;
        }
    }

    return true;
}

bool TestLegacyConfigMigration()
{
    auto parseAndMigrate = [](const char* jsonText,
                              OW::AimPriority& outPrio,
                              OW::HitAcceptance& outAcc) -> bool {
        rapidjson::Document doc;
        doc.Parse(jsonText);
        if (doc.HasParseError() || !doc.IsObject())
            return false;
        const OW::SkeletonBoneMask mask = doc.HasMember("aimBoneMask") && doc["aimBoneMask"].IsInt()
            ? static_cast<OW::SkeletonBoneMask>(doc["aimBoneMask"].GetInt())
            : OW::kDefaultAimBoneMask;
        const int mode = doc.HasMember("boneSelectionMode") && doc["boneSelectionMode"].IsInt()
            ? doc["boneSelectionMode"].GetInt()
            : 2;
        const int pref = doc.HasMember("preferredBone") && doc["preferredBone"].IsInt()
            ? doc["preferredBone"].GetInt()
            : OW::Config::kLegacyAimBoneChest;
        const bool hasPrio = doc.HasMember("aimPriority") && doc["aimPriority"].IsInt();
        const bool hasAcc = doc.HasMember("hitAcceptance") && doc["hitAcceptance"].IsInt();
        OW::Config::MigrateAimTargetFields(
            hasPrio, hasPrio ? doc["aimPriority"].GetInt() : 0,
            hasAcc, hasAcc ? doc["hitAcceptance"].GetInt() : 0,
            mask, mode, pref, outPrio, outAcc);
        return true;
    };

    OW::AimPriority prio{};
    OW::HitAcceptance acc{};

    // Legacy Head-only preset (no aimPriority / hitAcceptance keys) -> HeadOnly + SelectedOnly.
    if (!parseAndMigrate(R"({"aimBoneMask":1,"boneSelectionMode":2,"preferredBone":0})", prio, acc) ||
        prio != OW::AimPriority::HeadOnly || acc != OW::HitAcceptance::SelectedOnly) {
        return false;
    }

    // Legacy All-bones + Closest -> NearestHitbox + AnyValidHitbox.
    if (!parseAndMigrate(R"({"aimBoneMask":1023,"boneSelectionMode":2,"preferredBone":0})", prio, acc) ||
        prio != OW::AimPriority::NearestHitbox || acc != OW::HitAcceptance::AnyValidHitbox) {
        return false;
    }

    // Legacy Prefer Head + multi-bone -> HeadThenBody + AnyValidHitbox.
    if (!parseAndMigrate(R"({"aimBoneMask":7,"boneSelectionMode":1,"preferredBone":1})", prio, acc) ||
        prio != OW::AimPriority::HeadThenBody || acc != OW::HitAcceptance::AnyValidHitbox) {
        return false;
    }

    // Legacy Prefer Chest -> BodyCenter.
    if (!parseAndMigrate(R"({"aimBoneMask":1023,"boneSelectionMode":1,"preferredBone":0})", prio, acc) ||
        prio != OW::AimPriority::BodyCenter || acc != OW::HitAcceptance::AnyValidHitbox) {
        return false;
    }

    // Legacy Prefer Neck -> NeckSpread.
    if (!parseAndMigrate(R"({"aimBoneMask":7,"boneSelectionMode":1,"preferredBone":2})", prio, acc) ||
        prio != OW::AimPriority::NeckSpread || acc != OW::HitAcceptance::AnyValidHitbox) {
        return false;
    }

    // Explicit v3 Phase 1 keys override legacy derivation!
    if (!parseAndMigrate(R"({"aimBoneMask":1023,"boneSelectionMode":2,"aimPriority":3,"hitAcceptance":1})", prio, acc) ||
        prio != OW::AimPriority::BodyCenter || acc != OW::HitAcceptance::AnyValidHitbox) {
        return false;
    }

    // Out-of-range explicit keys clamp safely.
    if (!parseAndMigrate(R"({"aimBoneMask":1,"aimPriority":99,"hitAcceptance":-5})", prio, acc) ||
        prio != OW::AimPriority::NearestHitbox || acc != OW::HitAcceptance::SelectedOnly) {
        return false;
    }

    return true;
}

bool TestPreAimAndReactionDelayGate()
{
    using namespace OW::PreAim;

    // 1. Dynamic full-speed peek angle table verification (§5.2: t_grace=0.25s, r_body=0.35m, FOV_base=15 deg).
    if (!Near(ComputeDynamicPeekAngleDeg(3.0f, 15.0f, 0.0f), 15.00f, 0.02f) ||
        !Near(ComputeDynamicPeekAngleDeg(5.0f, 15.0f, 0.0f), 15.00f, 0.02f) ||
        !Near(ComputeDynamicPeekAngleDeg(10.0f, 15.0f, 0.0f), 9.79f, 0.02f) ||
        !Near(ComputeDynamicPeekAngleDeg(20.0f, 15.0f, 0.0f), 4.93f, 0.02f) ||
        !Near(ComputeDynamicPeekAngleDeg(35.0f, 15.0f, 0.0f), 2.82f, 0.02f) ||
        !Near(ComputeDynamicPeekAngleDeg(35.0f, 15.0f, 110.0f), 5.67f, 0.02f) ||  // Hanzo full draw
        !Near(ComputeDynamicPeekAngleDeg(35.0f, 15.0f, 70.0f), 7.29f, 0.02f) ||   // Torbjorn primary
        !Near(ComputeDynamicPeekAngleDeg(10.0f, 15.0f, 0.0f, kCrouchPeekSpeedMps), 6.28f, 0.02f)) {
        return false;
    }
    // Tracer/Genji walk at 6.0 m/s; others 5.5 m/s.
    if (!Near(DefaultPeekSpeedMps(OW::GameData::MakeHeroId(0x003)), 6.0f, 1e-4f) ||
        !Near(DefaultPeekSpeedMps(OW::GameData::MakeHeroId(0x029)), 6.0f, 1e-4f) ||
        !Near(DefaultPeekSpeedMps(OW::GameData::MakeHeroId(0x06E)), 5.5f, 1e-4f)) {
        return false;
    }
    // Exit hysteresis: min(FOV_base + 1.5, 1.2 * theta_peek).
    if (!Near(ComputePeekExitAngleDeg(9.79f, 15.0f), 11.748f, 0.02f) ||
        !Near(ComputePeekExitAngleDeg(15.0f, 15.0f), 16.5f, 0.02f)) {
        return false;
    }

    // 2. Right-angle 6-ray EdgeFan discontinuity detection & Pass 2 refinement (§5.4, Decision #5).
    const OW::Vector3 eye(0.0f, 1.6f, 0.0f);
    const OW::Vector3 targetBehindWall(0.0f, 1.6f, 20.0f);
    const OW::Vector3 camFwd(0.15f, 0.0f, 0.988f); // Crosshair biased toward +X side
    const OW::Vector3 lateral = ComputeRightAngleLateralDirection(eye, targetBehindWall, camFwd, {});
    if (lateral.X <= 0.99f || std::fabs(lateral.Y) > 1e-4f || std::fabs(lateral.Z) > 1e-4f)
        return false;

    const float fanWidth = ComputeEdgeFanMaxWidthMeters(20.0f, 4.93f); // ~1.725 m
    const auto fanRays = BuildEdgeFanRays(eye, targetBehindWall, lateral, 0.0f, fanWidth, false);
    if (fanRays[0].segmentIndex != 0 || fanRays[5].segmentIndex != 5 ||
        !Near(fanRays[5].lateralOffsetMeters, fanWidth, 1e-3f)) {
        return false;
    }

    auto makeFanDec = [](OW::RayDecisionStatus st, float frac) {
        OW::RayDecision d{};
        d.world = st;
        d.fraction = frac;
        return std::optional<OW::RayDecision>{d};
    };

    // Pending if any ray is missing or Stale.
    std::array<std::optional<OW::RayDecision>, kEdgeFanRayCount> pendingDecs{
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.45f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.46f),
        std::nullopt,
        makeFanDec(OW::RayDecisionStatus::Clear, 1.0f),
        makeFanDec(OW::RayDecisionStatus::Clear, 1.0f),
        makeFanDec(OW::RayDecisionStatus::Clear, 1.0f)
    };
    if (AnalyzeEdgeFanResults(eye, targetBehindWall, lateral, fanRays, pendingDecs, fanWidth).status !=
        EdgeFanStatus::Pending) {
        return false;
    }

    // Wall on rays 0..2, clear on rays 3..5 -> EdgeFound at bracket [2, 3].
    std::array<std::optional<OW::RayDecision>, kEdgeFanRayCount> edgeDecs{
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.45f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.46f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.47f),
        makeFanDec(OW::RayDecisionStatus::Clear, 1.0f),
        makeFanDec(OW::RayDecisionStatus::Clear, 1.0f),
        makeFanDec(OW::RayDecisionStatus::Clear, 1.0f)
    };
    const auto edgeAnalysis = AnalyzeEdgeFanResults(eye, targetBehindWall, lateral, fanRays, edgeDecs, fanWidth);
    if (edgeAnalysis.status != EdgeFanStatus::EdgeFound || edgeAnalysis.bracketIndex != 3 ||
        !Near(edgeAnalysis.parkPoint.Y, targetBehindWall.Y, 1e-4f) ||
        edgeAnalysis.parkPoint.X <= edgeAnalysis.edgeOffsetMeters) {
        return false;
    }

    // Depth jump discontinuity (0.40 -> 0.85 without reaching 1.0) -> EdgeFound.
    std::array<std::optional<OW::RayDecision>, kEdgeFanRayCount> jumpDecs{
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.40f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.41f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.85f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.86f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.87f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.88f)
    };
    if (AnalyzeEdgeFanResults(eye, targetBehindWall, lateral, fanRays, jumpDecs, fanWidth).status !=
        EdgeFanStatus::EdgeFound) {
        return false;
    }

    // All 6 rays blocked on near wall -> AllBlocked -> PreAimTracker skips movement & enters Parked directly!
    std::array<std::optional<OW::RayDecision>, kEdgeFanRayCount> wallDecs{
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.45f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.46f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.47f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.48f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.49f),
        makeFanDec(OW::RayDecisionStatus::Occluded, 0.50f)
    };
    const auto allBlockedAnalysis = AnalyzeEdgeFanResults(eye, targetBehindWall, lateral, fanRays, wallDecs, fanWidth);
    if (allBlockedAnalysis.status != EdgeFanStatus::AllBlocked)
        return false;

    // 3. Two-stage PreAimTracker (Positioning -> Parked) and all 4 Hard Release Exits (§5.3, §5.4).
    PreAimTracker tracker{};
    uint64_t tMs = 10000;
    // Entry outside theta_peek(20m)=4.93 deg must be rejected.
    if (tracker.Update(0x11, 1, tMs, 6.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, true))
        return false;
    // Entry inside theta_peek enters Positioning and requests EdgeFan Pass 1.
    if (!tracker.Update(0x11, 1, tMs, 3.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, true) ||
        tracker.Phase() != OW::PreAimPhase::Positioning) {
        return false;
    }
    bool isRefine = true;
    float lo = 0.0f, hi = 0.0f;
    if (!tracker.NeedsEdgeFanPass(isRefine, lo, hi) || isRefine)
        return false;
    // Apply Pass 1 edge -> hasParkPoint, requests Pass 2 refinement.
    tracker.ApplyEdgeFanAnalysis(edgeAnalysis, false);
    if (!tracker.HasParkPoint() || !tracker.NeedsEdgeFanPass(isRefine, lo, hi) || !isRefine)
        return false;
    // Crosshair reaches within 1.0 deg of parkPoint -> transitions permanently to Parked!
    tracker.ObserveParkCrosshairError(0.75f);
    if (tracker.Phase() != OW::PreAimPhase::Parked)
        return false;
    // Even if crosshair slightly jiggles away while Parked, MUST stay Parked (hands-off)!
    tMs += 50;
    if (!tracker.Update(0x11, 1, tMs, 3.5f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, true) ||
        tracker.Phase() != OW::PreAimPhase::Parked) {
        return false;
    }

    // AllBlocked skips Positioning straight to Parked.
    tracker.Reset();
    tracker.Update(0x12, 1, tMs, 2.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, true);
    tracker.ApplyEdgeFanAnalysis(allBlockedAnalysis, false);
    if (tracker.Phase() != OW::PreAimPhase::Parked || tracker.HasParkPoint())
        return false;

    // Exit #1: Reverse intent immediately releases PreAim.
    tMs += 20;
    if (tracker.Update(0x12, 1, tMs, 2.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, true, true) ||
        tracker.LastReleaseReason() != PreAimReleaseReason::ReverseIntent) {
        return false;
    }

    // Exit #2: Angular exit hysteresis (stays in between 4.93 and 5.916, exits > 5.92).
    tracker.Reset();
    tracker.Update(0x13, 1, tMs, 4.5f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false);
    tMs += 20;
    if (!tracker.Update(0x13, 1, tMs, 5.5f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false))
        return false; // inside hysteresis band
    tMs += 20;
    if (tracker.Update(0x13, 1, tMs, 6.2f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false) ||
        tracker.LastReleaseReason() != PreAimReleaseReason::AngularExit) {
        return false;
    }

    // Exit #3: Target retreating deeper behind cover for >= 300 ms.
    tracker.Reset();
    tracker.Update(0x14, 1, tMs, 2.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false);
    const OW::Vector3 retreatVel(-4.0f, 0.0f, 0.0f); // Opposite to lateral (+X)
    bool releasedByRetreat = false;
    float angle = 2.0f;
    for (int step = 0; step < 8; ++step) {
        tMs += 50;
        angle += 0.25f;
        if (!tracker.Update(0x14, 1, tMs, angle, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, retreatVel, false, false)) {
            releasedByRetreat = (tracker.LastReleaseReason() == PreAimReleaseReason::TargetRetreat);
            break;
        }
    }
    if (!releasedByRetreat)
        return false;

    // Exit #4: Hard TTL timeout (>= 800 ms without becoming Engageable).
    tracker.ResetAll();
    tracker.Update(0x15, 1, tMs, 2.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false);
    tMs += 850;
    if (tracker.Update(0x15, 1, tMs, 2.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false) ||
        tracker.LastReleaseReason() != PreAimReleaseReason::TtlExpired) {
        return false;
    }

    // Finding #8 regression: TTL release MUST suppress immediate next-frame re-entry
    // until actor leaves exit window or emerges (ClearSuppressionForEntity).
    tMs += 16;
    if (tracker.Update(0x15, 1, tMs, 2.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false))
        return false; // Still suppressed!
    // Leaving exit window (> 5.916 deg) rearms eligibility.
    tMs += 16;
    tracker.Update(0x15, 1, tMs, 7.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false);
    tMs += 16;
    if (!tracker.Update(0x15, 1, tMs, 2.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false))
        return false; // Rearmed cleanly after leaving window!

    // Finding #12 regression: EdgeFan width MUST NOT inflate past d * tan(theta_peek).
    const float narrowFanWidth = ComputeEdgeFanMaxWidthMeters(3.0f, 1.0f);
    if (!Near(narrowFanWidth, 3.0f * std::tan(DEG2RAD(1.0f)), 1e-3f) || narrowFanWidth > 0.06f)
        return false;

    // Finding #13 regression: Retreat exit requires BOTH moving deeper AND increasing angle;
    // decreasing angle above half the gate must NOT trigger retreat release!
    tracker.ResetAll();
    tracker.Update(0x16, 1, tMs, 4.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false);
    float decAngle = 4.0f;
    for (int step = 0; step < 10; ++step) {
        tMs += 50; // 500 ms > 300 ms
        decAngle -= 0.1f; // Decreasing angle (player tracking toward target), still > 0.5*4.93
        if (!tracker.Update(0x16, 1, tMs, decAngle, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, retreatVel, false, false))
            return false; // Must NOT falsely trigger TargetRetreat when angle is decreasing!
    }

    // Finding #2 & #3 regressions: preAimParkPointValid round-trip & unified CandidateAutoFireGateAllowed.
    {
        OW::TargetSolution s{};
        s.valid = true;
        s.tier = OW::VisibilityTier::PreAim;
        s.preAimPhase = OW::PreAimPhase::Positioning;
        s.preAimParkPointValid = false;
        const auto cPending = s.ToCandidate();
        if (cPending.preAimParkPointValid)
            return false;
        if (OW::CandidateAutoFireGateAllowed(cPending, static_cast<int>(OW::RayDecisionPolicy::NativeOnly)) ||
            OW::CandidateAutoFireGateAllowed(cPending, static_cast<int>(OW::RayDecisionPolicy::Hybrid))) {
            return false; // PreAim never allowed to auto-fire!
        }
        s.preAimParkPointValid = true;
        if (!OW::TargetSolution::FromCandidate(s.ToCandidate()).preAimParkPointValid)
            return false;
        OW::TargetCandidate vacCand{};
        vacCand.valid = true;
        vacCand.tier = OW::VisibilityTier::Engageable;
        vacCand.ray.actor = OW::RayDecisionStatus::Hit;
        vacCand.ray.world = OW::RayDecisionStatus::Stale;
        if (OW::CandidateAutoFireGateAllowed(vacCand, static_cast<int>(OW::RayDecisionPolicy::Hybrid)))
            return false; // Hybrid vacuum never allowed to auto-fire!
        vacCand.ray.world = OW::RayDecisionStatus::Clear;
        vacCand.reactionRemainingMs = 40.0f;
        if (OW::CandidateAutoFireGateAllowed(vacCand, static_cast<int>(OW::RayDecisionPolicy::Hybrid)))
            return false; // Active reaction delay never allowed to auto-fire!
        vacCand.reactionRemainingMs = 0.0f;
        if (!OW::CandidateAutoFireGateAllowed(vacCand, static_cast<int>(OW::RayDecisionPolicy::Hybrid)))
            return false;

        // F9 seam regression: EvaluateTrackingStageDecision (consumed directly by BehaviorController::RunTracking)
        // must emit NoOutput for PreAim pending EdgeFan, PreAim Parked, and ReactionDelayGate;
        // PreAimParkMove only when Positioning + preAimParkPointValid; EngagePullOnly under Hybrid vacuum;
        // and EngagePullAndFire only when Engageable + expired reaction + Fresh Clear.
        const int hybridMode = static_cast<int>(OW::RayDecisionPolicy::Hybrid);
        if (OW::EvaluateTrackingStageDecision(cPending, hybridMode) != OW::TrackingStageDecision::NoOutput)
            return false;
        s.preAimParkPointValid = true;
        if (OW::EvaluateTrackingStageDecision(s.ToCandidate(), hybridMode) != OW::TrackingStageDecision::PreAimParkMove)
            return false;
        s.preAimPhase = OW::PreAimPhase::Parked;
        if (OW::EvaluateTrackingStageDecision(s.ToCandidate(), hybridMode) != OW::TrackingStageDecision::NoOutput)
            return false;
        vacCand.ray.world = OW::RayDecisionStatus::Stale;
        vacCand.reactionRemainingMs = 0.0f;
        if (OW::EvaluateTrackingStageDecision(vacCand, hybridMode) != OW::TrackingStageDecision::EngagePullOnly)
            return false;
        vacCand.ray.world = OW::RayDecisionStatus::Clear;
        vacCand.reactionRemainingMs = 25.0f;
        if (OW::EvaluateTrackingStageDecision(vacCand, hybridMode) != OW::TrackingStageDecision::NoOutput)
            return false;
        vacCand.reactionRemainingMs = 0.0f;
        if (OW::EvaluateTrackingStageDecision(vacCand, hybridMode) != OW::TrackingStageDecision::EngagePullAndFire)
            return false;
    }

    // F1 regression: actor-specific reverse-intent break suppresses ONLY the broken actor A
    // and never suppresses a newly selected actor B in the same tick.
    {
        tracker.ResetAll();
        tracker.Update(0xA1, 1, tMs, 2.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false);
        tracker.NotifyReverseIntentBreak(0xA1, 1);
        if (tracker.Active() || !tracker.IsSuppressedForEntity(0xA1, 1) ||
            tracker.LastReleaseReason() != PreAimReleaseReason::ReverseIntent) {
            return false;
        }
        // Actor A stays suppressed inside window; actor B enters PreAim cleanly!
        tMs += 16;
        if (tracker.Update(0xA1, 1, tMs, 2.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false))
            return false;
        if (!tracker.Update(0xB2, 1, tMs, 2.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false) ||
            !tracker.IsTrackingEntity(0xB2, 1)) {
            return false;
        }
    }

    // F7 regression: explicit ReleaseAngularExit clears active PreAim with AngularExit reason.
    {
        tracker.ResetAll();
        tracker.Update(0xA2, 3, tMs, 2.0f, 20.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false);
        if (!tracker.ReleaseAngularExit(0xA2, 3) || tracker.Active() ||
            tracker.LastReleaseReason() != PreAimReleaseReason::AngularExit) {
            return false;
        }
    }

    // F8 regression: polling-rate-invariant retreat detection.
    // (a) 500 Hz polling (2 ms ticks) with 60 Hz entity updates (duplicate samples every 16 ms)
    //     MUST still trigger TargetRetreat at ~300 ms.
    {
        tracker.ResetAll();
        tracker.Update(0xA3, 1, tMs, 2.0f, 35.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false);
        const OW::Vector3 slowRetreatVel(-2.0f, 0.0f, 0.0f);
        float polledAngle = 2.0f;
        bool firedDuplicateRetreat = false;
        for (int tick = 1; tick <= 200; ++tick) {
            tMs += 2; // 500 Hz polling
            if ((tick % 8) == 0) polledAngle += 0.052f; // 60 Hz entity update (~3.25 deg/s)
            if (!tracker.Update(0xA3, 1, tMs, polledAngle, 35.0f, 15.0f, 4.93f,
                                eye, targetBehindWall, camFwd, slowRetreatVel, false, false)) {
                firedDuplicateRetreat = (tracker.LastReleaseReason() == PreAimReleaseReason::TargetRetreat);
                break;
            }
        }
        if (!firedDuplicateRetreat)
            return false;

        // (b) Slow continuous retreat (2 m/s @ 35m -> +0.00655 deg per 2 ms tick) MUST trigger TargetRetreat.
        tracker.ResetAll();
        tracker.Update(0xA4, 1, tMs, 2.0f, 35.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false);
        float slowAngle = 2.0f;
        bool firedSlowRetreat = false;
        for (int tick = 1; tick <= 200; ++tick) {
            tMs += 2;
            slowAngle += 0.00655f;
            if (!tracker.Update(0xA4, 1, tMs, slowAngle, 35.0f, 15.0f, 4.93f,
                                eye, targetBehindWall, camFwd, slowRetreatVel, false, false)) {
                firedSlowRetreat = (tracker.LastReleaseReason() == PreAimReleaseReason::TargetRetreat);
                break;
            }
        }
        if (!firedSlowRetreat)
            return false;

        // (c) Constant flat angle with movingDeeper=true (player matching target) MUST NOT fire TargetRetreat.
        tracker.ResetAll();
        tracker.Update(0xA5, 1, tMs, 2.0f, 35.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, {}, false, false);
        for (int tick = 1; tick <= 200; ++tick) {
            tMs += 2; // 400 ms total (> 300 ms, < 800 ms TTL)
            if (!tracker.Update(0xA5, 1, tMs, 2.0f, 35.0f, 15.0f, 4.93f,
                                eye, targetBehindWall, camFwd, slowRetreatVel, false, false)) {
                return false;
            }
        }

        // (d) Increase-then-flat plateau (Codex Round 3 Finding 3 exact case):
        //     Starts at 1.0 deg, reaches 1.5 deg after 20 ms, then stays flat at 1.5 deg for 380 ms.
        //     Plateau expiry must reset the episode so TargetRetreat NEVER fires.
        tracker.ResetAll();
        tracker.Update(0xA7, 1, tMs, 1.0f, 35.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, slowRetreatVel, false, false);
        tMs += 20;
        if (!tracker.Update(0xA7, 1, tMs, 1.5f, 35.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, slowRetreatVel, false, false))
            return false;
        for (int tick = 1; tick <= 190; ++tick) {
            tMs += 2; // +380 ms flat at 1.5 deg (400 ms total < 800 ms TTL)
            if (!tracker.Update(0xA7, 1, tMs, 1.5f, 35.0f, 15.0f, 4.93f,
                                eye, targetBehindWall, camFwd, slowRetreatVel, false, false)) {
                return false;
            }
        }

        // (e) Increase-then-small-decrease (Codex Round 3 Finding 3):
        //     Rises 1.0 -> 1.5 deg over 200 ms, dips by -0.02 deg to 1.48 deg at 210 ms,
        //     then rises gently for 110 ms (total 320 ms, but only 110 ms since dip < 300 ms).
        //     The small decrease must restart the retreat timer so TargetRetreat does NOT fire.
        tracker.ResetAll();
        tracker.Update(0xA8, 1, tMs, 1.0f, 35.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, slowRetreatVel, false, false);
        for (int step = 1; step <= 10; ++step) {
            tMs += 20; // 200 ms -> 1.5 deg
            if (!tracker.Update(0xA8, 1, tMs, 1.0f + 0.05f * static_cast<float>(step), 35.0f, 15.0f, 4.93f,
                                eye, targetBehindWall, camFwd, slowRetreatVel, false, false)) {
                return false;
            }
        }
        tMs += 10; // Small -0.02 deg dip -> restarts baseline at t=210 ms!
        if (!tracker.Update(0xA8, 1, tMs, 1.48f, 35.0f, 15.0f, 4.93f, eye, targetBehindWall, camFwd, slowRetreatVel, false, false))
            return false;
        for (int step = 1; step <= 5; ++step) {
            tMs += 22; // +110 ms (total 320 ms since start, only 110 ms since dip < 300 ms)
            if (!tracker.Update(0xA8, 1, tMs, 1.48f + 0.02f * static_cast<float>(step), 35.0f, 15.0f, 4.93f,
                                eye, targetBehindWall, camFwd, slowRetreatVel, false, false)) {
                return false;
            }
        }
    }

    // F10 regression: narrow EdgeFan windows (3m @ 0.1 deg -> ~0.00524m) MUST NOT be enlarged to 0.02m
    // and degenerate fans (< kEdgeFanMinUsableWidthMeters) must skip EdgeFan straight to Parked.
    {
        const float ultraNarrow = ComputeEdgeFanMaxWidthMeters(3.0f, 0.1f);
        if (!Near(ultraNarrow, 3.0f * std::tan(DEG2RAD(0.1f)), 1e-4f) || ultraNarrow >= 0.01f)
            return false;
        tracker.ResetAll();
        if (!tracker.Update(0xA6, 1, tMs, 0.05f, 3.0f, 15.0f, 0.1f,
                            eye, {0.0f, 1.5f, 3.0f}, camFwd, {}, false, true)) {
            return false;
        }
        bool refPass = false;
        float fLo = 0.0f, fHi = 0.0f;
        if (tracker.Phase() != OW::PreAimPhase::Parked ||
            tracker.NeedsEdgeFanPass(refPass, fLo, fHi)) {
            return false;
        }
    }

    // EdgeFan TargetDirectClear & Pass 2 ray-0 clear regression:
    // (a) Pass 1 TargetDirectClear MUST mark EdgeFan done & Parked immediately so Slots 0-5
    //     are released instead of starving top1Primary until TTL expiry.
    // (b) Pass 2 refinement with ray 0 (lateralOffset > 0) Clear MUST classify as EdgeFound at
    //     refineLo + margin (NOT TargetDirectClear at targetPos) and finish Pass 2 cleanly.
    {
        tracker.ResetAll();
        const OW::Vector3 covTarget{0.0f, 1.5f, 20.0f};
        if (!tracker.Update(0xA9, 1, tMs, 1.5f, 20.0f, 15.0f, 4.93f,
                            eye, covTarget, camFwd, {3.0f, 0.0f, 0.0f}, false, true)) {
            return false;
        }
        bool refPass = false;
        float fLo = 0.0f, fHi = 0.0f;
        if (!tracker.NeedsEdgeFanPass(refPass, fLo, fHi) || refPass)
            return false;
        const auto p1Rays = BuildEdgeFanRays(eye, tracker.FanAnchorTargetPos(),
            tracker.LateralDir(), fLo, fHi, false);
        std::array<std::optional<OW::RayDecision>, kEdgeFanRayCount> p1Clear{};
        for (size_t i = 0; i < kEdgeFanRayCount; ++i) {
            OW::RayDecision d{};
            d.world = OW::RayDecisionStatus::Clear;
            d.fraction = 1.0f;
            p1Clear[i] = d;
        }
        const auto p1Analysis = AnalyzeEdgeFanResults(
            eye, tracker.FanAnchorTargetPos(), tracker.LateralDir(),
            p1Rays, p1Clear, tracker.MaxFanWidthMeters());
        if (p1Analysis.status != EdgeFanStatus::TargetDirectClear)
            return false;
        tracker.ApplyEdgeFanAnalysis(p1Analysis, false);
        if (tracker.Phase() != OW::PreAimPhase::Parked ||
            tracker.NeedsEdgeFanPass(refPass, fLo, fHi)) {
            return false;
        }

        // (b) Pass 2 refinement ray 0 clear (lateralOffset > 0):
        tracker.ResetAll();
        if (!tracker.Update(0xAA, 1, tMs, 1.5f, 20.0f, 15.0f, 4.93f,
                            eye, covTarget, camFwd, {3.0f, 0.0f, 0.0f}, false, true)) {
            return false;
        }
        if (!tracker.NeedsEdgeFanPass(refPass, fLo, fHi) || refPass)
            return false;
        const auto p1RaysB = BuildEdgeFanRays(eye, tracker.FanAnchorTargetPos(),
            tracker.LateralDir(), fLo, fHi, false);
        std::array<std::optional<OW::RayDecision>, kEdgeFanRayCount> p1Edge{};
        for (size_t i = 0; i < kEdgeFanRayCount; ++i) {
            OW::RayDecision d{};
            d.world = (i < 3) ? OW::RayDecisionStatus::Occluded : OW::RayDecisionStatus::Clear;
            d.fraction = (i < 3) ? 0.5f : 1.0f;
            p1Edge[i] = d;
        }
        const auto p1EdgeRes = AnalyzeEdgeFanResults(
            eye, tracker.FanAnchorTargetPos(), tracker.LateralDir(),
            p1RaysB, p1Edge, tracker.MaxFanWidthMeters());
        if (p1EdgeRes.status != EdgeFanStatus::EdgeFound)
            return false;
        tracker.ApplyEdgeFanAnalysis(p1EdgeRes, false);
        if (!tracker.NeedsEdgeFanPass(refPass, fLo, fHi) || !refPass || fLo <= 0.1f)
            return false;
        const auto p2Rays = BuildEdgeFanRays(eye, tracker.FanAnchorTargetPos(),
            tracker.LateralDir(), fLo, fHi, true);
        const auto p2Res = AnalyzeEdgeFanResults(
            eye, tracker.FanAnchorTargetPos(), tracker.LateralDir(),
            p2Rays, p1Clear, tracker.MaxFanWidthMeters());
        if (p2Res.status != EdgeFanStatus::EdgeFound || !Near(p2Res.edgeOffsetMeters, fLo, 1e-4f))
            return false;
        tracker.ApplyEdgeFanAnalysis(p2Res, true);
        if (!tracker.HasParkPoint() || tracker.NeedsEdgeFanPass(refPass, fLo, fHi))
            return false;
    }

    // F5 exact regression: MakeNativeHitWindowInput must preserve GroundClip-adjusted preFireAimPoint
    // instead of reconstructing displacement from zeroed post-clip velocity.
    {
        OW::LeadPredictionResult clippedLead{};
        clippedLead.predictionEnabled = true;
        clippedLead.rawAimPoint = {0.0f, 3.6f, 20.0f};
        clippedLead.preFireAimPoint = {0.0f, 0.6f, 20.0f};  // 3.6 + (-10)*0.3
        clippedLead.targetVelocity = {0.0f, -10.0f, 0.0f};
        clippedLead.timing.targetExtrapolationMs = 300.0f;
        clippedLead.projectile.projectileSpeed = 25.0f;
        clippedLead.projectile.directHitOnly = true;
        clippedLead.projectile.gravity = true;
        clippedLead.projectile.gravityAcceleration = 9.8f;

        OW::RayDecision floorHit{OW::RayDecisionStatus::Unknown, OW::RayDecisionStatus::Occluded};
        floorHit.fraction = 0.5f;
        OW::BallisticIntercept f5Solved{};
        if (!OW::TrajectoryRay::ApplyAirborneGroundClipAndSolveIntercept(
                {0.0f, 1.5f, 0.0f}, {0.0f, 3.6f, 20.0f}, {0.0f, 2.0f, 20.0f},
                {0.0f, 2.0f, 20.0f}, {0.0f, -2.0f, 20.0f}, floorHit,
                25.0f, 9.8f, clippedLead.preFireAimPoint, clippedLead.targetVelocity, &f5Solved)) {
            return false;
        }
        if (!Near(clippedLead.preFireAimPoint.Y, 1.6f, 1e-3f) ||
            !Near(clippedLead.targetVelocity.Y, 0.0f, 1e-3f)) {
            return false;
        }
        const OW::NativeHitWindowInput nativeIn = OW::MakeNativeHitWindowInput(
            clippedLead, OW::AimClass::ProjectileSingle, 100.0f);
        const float nativePreFireY = clippedLead.rawAimPoint.Y + nativeIn.displacement.Y;
        if (!Near(nativePreFireY, 1.6f, 1e-3f)) {
            return false;
        }
    }

    // 4. Unified ReactionDelayGate (§5.5, Decision #2) + TargetHitWindow enforcement.
    ReactionDelayGate gate{};
    bool peekEmerge = false;
    // Open-field target with no occlusion memory -> 0 ms delay, not PeekEmerge.
    if (!Near(gate.OnEngageable(0x20, 1, OW::GameData::MakeHeroId(0x06E), 0.0f, 20000, peekEmerge), 0.0f) ||
        peekEmerge) {
        return false;
    }
    // Finding #14 regression: stale PreAim history past recentOcclusionMemoryMs (1500ms)
    // MUST expire and NOT trigger a reaction delay in open space later!
    gate.ObserveOccludedOrPreAim(0x24, 1, 20000, true);
    if (!Near(gate.OnEngageable(0x24, 1, OW::GameData::MakeHeroId(0x06E), 0.0f, 23000, peekEmerge), 0.0f) ||
        peekEmerge) {
        return false;
    }
    // Standard hero emerging from PreAim -> ~140 ms delay.
    gate.ObserveOccludedOrPreAim(0x21, 1, 20000, true);
    if (!gate.HasRecentPreAimOrPeek(0x21, 1, 20100))
        return false;
    const float stdRem0 = gate.OnEngageable(0x21, 1, OW::GameData::MakeHeroId(0x06E), 0.0f, 20100, peekEmerge);
    if (!peekEmerge || !Near(stdRem0, 140.0f, 1e-2f))
        return false;
    const float stdRemMid = gate.OnEngageable(0x21, 1, OW::GameData::MakeHeroId(0x06E), 0.0f, 20180, peekEmerge);
    if (!peekEmerge || !Near(stdRemMid, 60.0f, 1e-2f))
        return false;

    // While reactionRemainingMs > 0, InSolutionHitWindow MUST hard-block fire!
    OW::TargetSolution emergeSol{};
    emergeSol.valid = true;
    emergeSol.tier = OW::VisibilityTier::Engageable;
    emergeSol.transition = OW::TransitionKind::PeekEmerge;
    emergeSol.reactionRemainingMs = stdRemMid;
    emergeSol.guidanceBoneId = OW::BONE_HEAD;
    emergeSol.guidanceAimPoint = OW::Vector3(0.0f, 0.0f, 10.0f);
    emergeSol.nativeHitWindow.projectileRadius = 0.05f;
    emergeSol.ray.actor = OW::RayDecisionStatus::Hit;
    emergeSol.ray.world = OW::RayDecisionStatus::Clear;
    OW::g_fixtureHitBoneId = OW::BONE_HEAD;
    if (OW::InSolutionHitWindow(emergeSol, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, emergeSol.guidanceAimPoint))
        return false;

    // After reaction window expires (20250 >= 20100 + 140), reactionRemainingMs == 0 and fire passes!
    const float stdRemDone = gate.OnEngageable(0x21, 1, OW::GameData::MakeHeroId(0x06E), 0.0f, 20250, peekEmerge);
    if (peekEmerge || !Near(stdRemDone, 0.0f, 1e-4f))
        return false;
    emergeSol.reactionRemainingMs = stdRemDone;
    if (!OW::InSolutionHitWindow(emergeSol, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, emergeSol.guidanceAimPoint))
        return false;

    // High-mobility hero (Tracer) or airborne Widowmaker jump-snipe gets 200 ms window.
    gate.ObserveOccludedOrPreAim(0x22, 1, 21000, false);
    const float tracerRem = gate.OnEngageable(0x22, 1, OW::GameData::MakeHeroId(0x003), 0.0f, 21050, peekEmerge);
    if (!peekEmerge || !Near(tracerRem, 200.0f, 1e-2f))
        return false;

    gate.ObserveOccludedOrPreAim(0x23, 1, 21000, true);
    const float widowJumpRem = gate.OnEngageable(0x23, 1, OW::GameData::MakeHeroId(0x00A), 4.5f, 21050, peekEmerge);
    if (!peekEmerge || !Near(widowJumpRem, 200.0f, 1e-2f))
        return false;

    return true;
}

} // namespace

int main()
{
    if (!TestIntentEngine())
        return Fail("IntentEngine residual / anti-tug / reversal exemption / IdleHold");
    if (!TestUtilityAndFirePhaseCommitment())
        return Fail("Utility scoring / bounded prefer / AimFirePhase anchor & breakthrough");
    if (!TestBoneDecouplingAndStage0())
        return Fail("AimPriority x HitAcceptance / Stage 0 AimPlan / TargetSolution / TargetHitWindow");
    if (!TestLegacyConfigMigration())
        return Fail("Legacy config JSON migration for aimPriority & hitAcceptance");
    if (!TestPreAimAndReactionDelayGate())
        return Fail("Phase 2 PreAim theta_peek / EdgeFan / 4 exits / ReactionDelayGate");

    std::cout << "PASS V3TargetPipelineSelfTest: IntentEngine + Utility/FirePhase + AimPriority/HitAcceptance + TargetHitWindow + LegacyMigration + Phase2(PreAim/EdgeFan/ReactionDelayGate)\n";
    return EXIT_SUCCESS;
}
