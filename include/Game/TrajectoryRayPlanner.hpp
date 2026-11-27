#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "Game/BallisticSolver.hpp"
#include "Game/RayDecision.hpp"
#include "Game/RayQueryBroker.hpp"
#include "Game/TargetSolutionTypes.hpp"
#include "Utils/Types.hpp"

namespace OW::TrajectoryRay {

inline constexpr float kParabolicTwoSegArchMeters = 0.15f;
inline constexpr float kParabolicThreeSegArchMeters = 0.60f;
inline constexpr float kAirborneMinVerticalSpeedMps = 0.1f;
inline constexpr float kDefaultActorGravityMps2 = 9.8f;
inline constexpr float kMaxAirborneHorizonSec = 0.5f;
inline constexpr size_t kMaxParabolicSegments = 3;
inline constexpr size_t kMaxRayBatchSlots = EngineTrace::RayQueryBroker::kMaxBatchSize;  // 6

struct ParabolicChordPlan {
    uint8_t segmentCount = 1;
    float archHeightMeters = 0.0f;       // h_max = (1/8) * g * T^2
    float maxChordDeviationMeters = 0.0f; // eps_N = h_max / N^2
    float flightSeconds = 0.0f;
    Vector3 impactPoint{};               // Physical impact point P_T (without launchUpBias)
    std::array<std::pair<Vector3, Vector3>, kMaxParabolicSegments> segments{};
    RaySegmentKind segmentKind = RaySegmentKind::Direct;
};

// Builds 1/2/3 equal-time chord segments along the true physical parabolic trajectory (§6.4):
//   P(uT) = (1 - u)*P0 + u*P_T + 0.5 * g * T^2 * u * (1 - u) * +Y
// Strictly separates physical trajectory tracing from CompensateProjectileLaunchBias().
inline ParabolicChordPlan BuildParabolicChordPlan(const Vector3& launchOrigin,
                                                  const Vector3& preFireTargetPoint,
                                                  const Vector3& targetVelocity,
                                                  float projectileSpeedMps,
                                                  float gravityAccelerationMps2,
                                                  const Vector3& fallbackDirectEnd) noexcept {
    ParabolicChordPlan plan{};
    plan.segmentCount = 1;
    plan.segmentKind = RaySegmentKind::Direct;
    plan.impactPoint = fallbackDirectEnd;
    plan.segments[0] = {launchOrigin, fallbackDirectEnd};

    const auto finiteVec = [](const Vector3& v) {
        return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
    };
    if (!finiteVec(launchOrigin) || !finiteVec(preFireTargetPoint) || !finiteVec(targetVelocity) ||
        !std::isfinite(projectileSpeedMps) || projectileSpeedMps <= 0.0f ||
        !std::isfinite(gravityAccelerationMps2) || gravityAccelerationMps2 <= 0.0f) {
        return plan;
    }

    const BallisticIntercept intercept = SolveBallisticIntercept(
        launchOrigin, preFireTargetPoint, targetVelocity,
        projectileSpeedMps, gravityAccelerationMps2);
    if (!intercept.solved || !std::isfinite(intercept.flightSeconds) || intercept.flightSeconds <= 0.0f) {
        return plan;
    }

    const float T = intercept.flightSeconds;
    const float g = gravityAccelerationMps2;
    const Vector3 impactPt(
        preFireTargetPoint.X + targetVelocity.X * T,
        preFireTargetPoint.Y + targetVelocity.Y * T,
        preFireTargetPoint.Z + targetVelocity.Z * T);
    if (!finiteVec(impactPt)) {
        return plan;
    }

    const float archHeight = 0.125f * g * T * T;
    uint8_t segments = 1;
    if (archHeight > kParabolicThreeSegArchMeters) {
        segments = 3;
    } else if (archHeight > kParabolicTwoSegArchMeters) {
        segments = 2;
    }

    plan.segmentCount = segments;
    plan.archHeightMeters = archHeight;
    plan.maxChordDeviationMeters = archHeight / static_cast<float>(segments * segments);
    plan.flightSeconds = T;
    plan.impactPoint = impactPt;
    plan.segmentKind = segments > 1 ? RaySegmentKind::ParabolicChord : RaySegmentKind::Direct;

    const auto sampleArc = [&](float u) -> Vector3 {
        const float lift = 0.5f * g * T * T * u * (1.0f - u);
        return Vector3(
            launchOrigin.X + (impactPt.X - launchOrigin.X) * u,
            launchOrigin.Y + (impactPt.Y - launchOrigin.Y) * u + lift,
            launchOrigin.Z + (impactPt.Z - launchOrigin.Z) * u);
    };

    for (uint8_t i = 0; i < segments; ++i) {
        const float u0 = static_cast<float>(i) / static_cast<float>(segments);
        const float u1 = static_cast<float>(i + 1) / static_cast<float>(segments);
        plan.segments[i] = {sampleArc(u0), sampleArc(u1)};
    }
    return plan;
}

// Aggregates N parabolic chord segment RayDecisions into a single path verdict (§6.4, Decision #8):
//   - Any Occluded segment -> Occluded (carries earliest hit fraction/flags/entityId/segmentIndex)
//   - All N segments Fresh Clear -> Clear (directly Engageable over low walls, no memory gate)
//   - Otherwise -> preserves observation vacuum status (Stale/Timeout/Unavailable/Invalid/Unknown)
inline RayDecision AggregateParabolicDecisions(
    const std::array<std::optional<RayDecision>, kMaxParabolicSegments>& segmentDecisions,
    uint8_t segmentCount,
    uint32_t maxAgeMs = RayDecision::kDefaultMaxAgeMs) noexcept {
    const uint8_t count = std::clamp<uint8_t>(segmentCount, 1u, static_cast<uint8_t>(kMaxParabolicSegments));
    const RaySegmentKind kind = count > 1 ? RaySegmentKind::ParabolicChord : RaySegmentKind::Direct;

    bool allClear = true;
    bool hasVacuum = false;
    bool hasMissing = false;
    uint8_t firstMissingIdx = 0;
    RayDecision firstClear{};
    RayDecision firstVacuum{};
    uint32_t maxAgeSeen = 0;
    uint32_t maxLatencySeen = 0;
    uint64_t oldestCompletedUs = 0;

    for (uint8_t i = 0; i < count; ++i) {
        if (!segmentDecisions[i].has_value()) {
            allClear = false;
            if (!hasMissing) {
                hasMissing = true;
                firstMissingIdx = i;
            }
            continue;
        }
        const RayDecision& seg = *segmentDecisions[i];
        if (seg.world == RayDecisionStatus::Occluded) {
            return seg;  // Earliest blocked chord wins immediately
        }
        if (seg.world == RayDecisionStatus::Clear && seg.Fresh(maxAgeMs)) {
            if (seg.ageMs > maxAgeSeen) maxAgeSeen = seg.ageMs;
            if (seg.latencyUs > maxLatencySeen) maxLatencySeen = seg.latencyUs;
            if (oldestCompletedUs == 0 || (seg.completedAtUs != 0 && seg.completedAtUs < oldestCompletedUs)) {
                oldestCompletedUs = seg.completedAtUs;
            }
            if (i == 0) firstClear = seg;
        } else {
            allClear = false;
            if (!hasVacuum) {
                hasVacuum = true;
                firstVacuum = seg;
            }
        }
    }

    // Only return Clear when EVERY required chord segment is present and Fresh Clear (Decision #8, Finding #1).
    if (allClear) {
        RayDecision clearOut = firstClear;
        clearOut.world = RayDecisionStatus::Clear;
        clearOut.fraction = 1.0f;
        clearOut.staleReason = RayStaleReason::None;
        clearOut.ageMs = maxAgeSeen;
        clearOut.latencyUs = maxLatencySeen;
        clearOut.completedAtUs = oldestCompletedUs;
        clearOut.segmentKind = kind;
        clearOut.segmentIndex = 0;
        return clearOut;
    }

    if (hasVacuum) {
        return firstVacuum;
    }

    RayDecision unknownOut{};
    unknownOut.world = RayDecisionStatus::Unknown;
    unknownOut.fraction = 1.0f;
    unknownOut.segmentKind = kind;
    unknownOut.segmentIndex = firstMissingIdx;
    return unknownOut;
}

// Shared production + selftest seam for 1-chord and multi-chord broker evaluation (F4/F9):
// ensures single-chord and multi-chord submission and lookup always use the exact same
// physical endpoints from chordPlan->segments[i].
inline RayDecision EvaluateParabolicBrokerDecision(
    EngineTrace::RayQueryBroker& broker,
    const ParabolicChordPlan* chordPlan,
    const Vector3& fallbackStart,
    const Vector3& fallbackEnd,
    uint64_t frame,
    uint64_t connectionEpoch,
    uint64_t sceneEpoch,
    uint64_t entityKey,
    uint64_t actorGeneration,
    int boneId,
    uint64_t sampleTick,
    uint64_t mask,
    uint32_t maxAgeMs,
    bool submitMissing) noexcept {
    const uint8_t segCount = (chordPlan != nullptr && chordPlan->segmentCount >= 1)
        ? std::clamp<uint8_t>(chordPlan->segmentCount, 1u, static_cast<uint8_t>(kMaxParabolicSegments))
        : static_cast<uint8_t>(1u);
    const RaySegmentKind segKind = (segCount > 1)
        ? RaySegmentKind::ParabolicChord
        : RaySegmentKind::Direct;

    std::array<std::optional<RayDecision>, kMaxParabolicSegments> segDecisions{};
    std::array<EngineTrace::RayQueryBroker::Query, kMaxParabolicSegments> segQueries{};
    for (uint8_t s = 0; s < segCount; ++s) {
        const Vector3 segStart = (chordPlan != nullptr) ? chordPlan->segments[s].first : fallbackStart;
        const Vector3 segEnd = (chordPlan != nullptr) ? chordPlan->segments[s].second : fallbackEnd;
        EngineTrace::RayQueryBroker::Query q{};
        q.request.start = {segStart.X, segStart.Y, segStart.Z};
        q.request.end = {segEnd.X, segEnd.Y, segEnd.Z};
        q.request.mask = mask;
        q.request.sceneEpoch = sceneEpoch;
        q.frame = frame;
        q.connectionEpoch = connectionEpoch;
        q.sceneEpoch = sceneEpoch;
        q.entityKey = entityKey;
        q.actorGeneration = actorGeneration;
        q.boneId = boneId;
        q.sampleTick = sampleTick;
        q.segmentKind = segKind;
        q.segmentIndex = s;
        segQueries[s] = q;
        segDecisions[s] = broker.Latest(
            q.request, frame, connectionEpoch, sceneEpoch,
            entityKey, actorGeneration, boneId, maxAgeMs,
            EngineTrace::RayQueryBroker::kDefaultMaxEndpointDriftMeters,
            segKind, s);
    }
    if (submitMissing) {
        if (segCount > 1) {
            broker.SubmitBatch(std::span<const EngineTrace::RayQueryBroker::Query>(
                segQueries.data(), segCount));
        } else {
            broker.Submit(std::move(segQueries[0]));
        }
    }

    if (segCount == 1) {
        if (segDecisions[0].has_value())
            return *segDecisions[0];
        RayDecision unknown{};
        unknown.world = RayDecisionStatus::Unknown;
        unknown.fraction = 1.0f;
        unknown.segmentKind = RaySegmentKind::Direct;
        unknown.segmentIndex = 0;
        return unknown;
    }
    return AggregateParabolicDecisions(segDecisions, segCount, maxAgeMs);
}

// Builds the airborne vertical-displacement terrain clip ray (§6.3 Slot 2).
// Triggered only when |v_y| > 0.1 m/s so grounded targets consume zero extra slots.
inline bool BuildAirborneGroundClipRay(const Vector3& entityRootPos,
                                       const Vector3& targetVelocity,
                                       float predictionHorizonSec,
                                       Vector3& outRayStart,
                                       Vector3& outRayEnd,
                                       float actorGravityMps2 = kDefaultActorGravityMps2) noexcept {
    if (!std::isfinite(entityRootPos.X) || !std::isfinite(entityRootPos.Y) || !std::isfinite(entityRootPos.Z) ||
        !std::isfinite(targetVelocity.X) || !std::isfinite(targetVelocity.Y) || !std::isfinite(targetVelocity.Z)) {
        return false;
    }
    if (std::fabs(targetVelocity.Y) <= kAirborneMinVerticalSpeedMps) {
        return false;
    }

    const float dt = std::clamp(
        std::isfinite(predictionHorizonSec) ? predictionHorizonSec : 0.12f,
        0.02f, kMaxAirborneHorizonSec);
    const float g = (std::isfinite(actorGravityMps2) && actorGravityMps2 >= 0.0f)
        ? actorGravityMps2 : kDefaultActorGravityMps2;

    const Vector3 predictedRoot(
        entityRootPos.X + targetVelocity.X * dt,
        entityRootPos.Y + targetVelocity.Y * dt - 0.5f * g * dt * dt,
        entityRootPos.Z + targetVelocity.Z * dt);

    // Start slightly above feet to avoid grazing the floor at t=0; cast down to predicted root
    // (or at least 0.35m below current root when descending so terrain intersection is captured).
    outRayStart = Vector3(entityRootPos.X, entityRootPos.Y + 0.10f, entityRootPos.Z);
    const float endY = (std::min)(predictedRoot.Y - 0.05f, entityRootPos.Y - 0.35f);
    outRayEnd = Vector3(predictedRoot.X, endY, predictedRoot.Z);
    return (outRayEnd - outRayStart).Size() > 0.05f;
}

// Clamps a predicted aim point vertically so an airborne target's predicted bone never sinks
// beneath terrain hit by the GroundClip ray (§6.3 Slot 2).
inline bool ApplyAirborneGroundClip(const Vector3& rawBonePoint,
                                    const Vector3& entityRootPos,
                                    const Vector3& groundRayStart,
                                    const Vector3& groundRayEnd,
                                    const RayDecision& groundClipDecision,
                                    Vector3& inOutPredictedAimPoint) noexcept {
    if (groundClipDecision.world != RayDecisionStatus::Occluded ||
        !std::isfinite(groundClipDecision.fraction) ||
        groundClipDecision.fraction < 0.0f || groundClipDecision.fraction >= 1.0f) {
        return false;
    }
    if (!std::isfinite(rawBonePoint.Y) || !std::isfinite(entityRootPos.Y) ||
        !std::isfinite(inOutPredictedAimPoint.Y)) {
        return false;
    }

    const float groundY = groundRayStart.Y +
        (groundRayEnd.Y - groundRayStart.Y) * groundClipDecision.fraction;
    const float boneHeightAboveRoot = (std::max)(0.0f, rawBonePoint.Y - entityRootPos.Y);
    const float minBoneY = groundY + boneHeightAboveRoot;
    if (inOutPredictedAimPoint.Y < minBoneY) {
        inOutPredictedAimPoint.Y = minBoneY;
        return true;
    }
    return false;
}

// 6-slot VisionProbe batch plan (§6.3): fixed-capacity, zero-allocation, deduplicated by slot key.
struct RayBatchPlan {
    std::array<EngineTrace::RayQueryBroker::Query, kMaxRayBatchSlots> queries{};
    size_t count = 0;
    bool edgeFanBorrowed = false;

    static bool SameSlotKey(const EngineTrace::RayQueryBroker::Query& a,
                            const EngineTrace::RayQueryBroker::Query& b) noexcept {
        return a.entityKey == b.entityKey &&
               a.actorGeneration == b.actorGeneration &&
               a.boneId == b.boneId &&
               a.segmentKind == b.segmentKind &&
               a.segmentIndex == b.segmentIndex &&
               a.request.mask == b.request.mask;
    }

    bool TryAdd(const EngineTrace::RayQueryBroker::Query& q) noexcept {
        if (count >= kMaxRayBatchSlots || q.sceneEpoch == 0)
            return false;
        for (size_t i = 0; i < count; ++i) {
            if (SameSlotKey(queries[i], q))
                return false;
        }
        queries[count++] = q;
        return true;
    }

    std::span<const EngineTrace::RayQueryBroker::Query> Span() const noexcept {
        return {queries.data(), count};
    }
};

// Clips predicted physical actor motion (pre-fire point & downward vertical velocity)
// against terrain hit by the GroundClip ray so subsequent ballistic interception, chord rays,
// and NativeHitWindowInput all stay physically consistent above terrain (Finding #9).
inline bool ApplyAirborneGroundClipMotion(const Vector3& rawBonePoint,
                                          const Vector3& entityRootPos,
                                          const Vector3& groundRayStart,
                                          const Vector3& groundRayEnd,
                                          const RayDecision& groundClipDecision,
                                          float approxFlightSec,
                                          Vector3& inOutPreFirePoint,
                                          Vector3& inOutTargetVelocity) noexcept {
    if (groundClipDecision.world != RayDecisionStatus::Occluded ||
        !std::isfinite(groundClipDecision.fraction) ||
        groundClipDecision.fraction < 0.0f || groundClipDecision.fraction >= 1.0f) {
        return false;
    }
    if (!std::isfinite(rawBonePoint.Y) || !std::isfinite(entityRootPos.Y) ||
        !std::isfinite(inOutPreFirePoint.Y) || !std::isfinite(inOutTargetVelocity.Y)) {
        return false;
    }

    const float groundY = groundRayStart.Y +
        (groundRayEnd.Y - groundRayStart.Y) * groundClipDecision.fraction;
    const float boneHeightAboveRoot = (std::max)(0.0f, rawBonePoint.Y - entityRootPos.Y);
    const float minBoneY = groundY + boneHeightAboveRoot;
    const float safeFlight = (std::isfinite(approxFlightSec) && approxFlightSec > 0.0f)
        ? approxFlightSec : 0.0f;
    const float projectedImpactY = inOutPreFirePoint.Y + inOutTargetVelocity.Y * safeFlight;

    bool clipped = false;
    if (inOutPreFirePoint.Y < minBoneY) {
        inOutPreFirePoint.Y = minBoneY;
        if (inOutTargetVelocity.Y < 0.0f) inOutTargetVelocity.Y = 0.0f;
        clipped = true;
    } else if (projectedImpactY < minBoneY && safeFlight > 1e-4f) {
        // Target will land before impact: cap downward vertical velocity so impact stays on terrain.
        inOutTargetVelocity.Y = (minBoneY - inOutPreFirePoint.Y) / safeFlight;
        clipped = true;
    }
    return clipped;
}

// Closed-loop GroundClip + Ballistic Intercept solver (F6 fix):
// Enforces terrain clearance against the ACTUAL solved flight time T (re-solving iteratively
// until pre-fire point, vertical velocity, and impact height at T all agree above minBoneY).
inline bool ApplyAirborneGroundClipAndSolveIntercept(
    const Vector3& launchOrigin,
    const Vector3& rawBonePoint,
    const Vector3& entityRootPos,
    const Vector3& groundRayStart,
    const Vector3& groundRayEnd,
    const RayDecision& groundClipDecision,
    float projectileSpeedMps,
    float gravityAccelerationMps2,
    Vector3& inOutPreFirePoint,
    Vector3& inOutTargetVelocity,
    BallisticIntercept* outIntercept = nullptr) noexcept {
    if (outIntercept) *outIntercept = BallisticIntercept{};
    if (groundClipDecision.world != RayDecisionStatus::Occluded ||
        !std::isfinite(groundClipDecision.fraction) ||
        groundClipDecision.fraction < 0.0f || groundClipDecision.fraction >= 1.0f) {
        return false;
    }
    if (!std::isfinite(rawBonePoint.Y) || !std::isfinite(entityRootPos.Y) ||
        !std::isfinite(inOutPreFirePoint.Y) || !std::isfinite(inOutTargetVelocity.Y)) {
        return false;
    }

    const float groundY = groundRayStart.Y +
        (groundRayEnd.Y - groundRayStart.Y) * groundClipDecision.fraction;
    const float boneHeightAboveRoot = (std::max)(0.0f, rawBonePoint.Y - entityRootPos.Y);
    const float minBoneY = groundY + boneHeightAboveRoot;

    bool clipped = false;
    if (inOutPreFirePoint.Y < minBoneY) {
        inOutPreFirePoint.Y = minBoneY;
        if (inOutTargetVelocity.Y < 0.0f) inOutTargetVelocity.Y = 0.0f;
        clipped = true;
    }

    if (!std::isfinite(projectileSpeedMps) || projectileSpeedMps <= 0.0f) {
        return clipped;
    }

    const float safeGravity = (std::isfinite(gravityAccelerationMps2) && gravityAccelerationMps2 >= 0.0f)
        ? gravityAccelerationMps2 : 0.0f;
    BallisticIntercept lastSolved{};
    constexpr int kMaxClipIterations = 5;
    for (int iter = 0; iter < kMaxClipIterations; ++iter) {
        const BallisticIntercept solved = SolveBallisticIntercept(
            launchOrigin, inOutPreFirePoint, inOutTargetVelocity,
            projectileSpeedMps, safeGravity);
        if (!solved.solved || !std::isfinite(solved.flightSeconds) || solved.flightSeconds <= 1e-4f) {
            // F6 fix: never retain a stale intercept solved for an earlier velocity when
            // re-solving with the clipped motion fails.
            lastSolved = BallisticIntercept{};
            break;
        }
        lastSolved = solved;
        const float impactY = inOutPreFirePoint.Y + inOutTargetVelocity.Y * solved.flightSeconds;
        if (impactY >= minBoneY - 1e-4f) {
            break;
        }
        clipped = true;
        // Motion is about to change; invalidate lastSolved until a valid intercept is solved for it.
        lastSolved = BallisticIntercept{};
        if (iter + 1 == kMaxClipIterations) {
            // Final safeguard: clamp vertical velocity to 0 and re-solve once; if unreachable,
            // leave lastSolved.solved == false so caller explicitly rejects the ballistic step.
            inOutTargetVelocity.Y = 0.0f;
            const BallisticIntercept clampedSolved = SolveBallisticIntercept(
                launchOrigin, inOutPreFirePoint, inOutTargetVelocity,
                projectileSpeedMps, safeGravity);
            if (clampedSolved.solved && std::isfinite(clampedSolved.flightSeconds) &&
                clampedSolved.flightSeconds > 1e-4f) {
                lastSolved = clampedSolved;
            }
            break;
        }
        float cappedVy = (minBoneY - inOutPreFirePoint.Y) / solved.flightSeconds;
        if (!std::isfinite(cappedVy) || cappedVy > 0.0f) cappedVy = 0.0f;
        inOutTargetVelocity.Y = cappedVy;
    }

    if (outIntercept && lastSolved.solved) {
        *outIntercept = lastSolved;
    }
    return clipped;
}

// Ordered chord group separating urgent (missing/stale) queries [0..urgentCount) from
// already-fresh refresh queries [urgentCount..count) so PlanRayBatch never starves secondary
// candidates or GroundClip under 6-slot saturation (F2 fix).
struct RayChordGroup {
    std::array<EngineTrace::RayQueryBroker::Query, kMaxParabolicSegments> queries{};
    size_t count = 0;
    size_t urgentCount = 0;

    void Clear() noexcept {
        count = 0;
        urgentCount = 0;
    }

    bool PushUrgent(const EngineTrace::RayQueryBroker::Query& q) noexcept {
        if (count >= kMaxParabolicSegments)
            return false;
        // Keep urgent entries strictly packed at [0..urgentCount).
        for (size_t i = count; i > urgentCount; --i) {
            queries[i] = queries[i - 1];
        }
        queries[urgentCount++] = q;
        ++count;
        return true;
    }

    bool PushRefresh(const EngineTrace::RayQueryBroker::Query& q) noexcept {
        if (count >= kMaxParabolicSegments)
            return false;
        queries[count++] = q;
        return true;
    }
};

struct RayBatchPlanInput {
    // Special mode (§6.3): EdgeFan borrows all 6 slots (Slot 0..5) when PreAim needs wall edge.
    bool edgeFanBorrowActive = false;
    std::array<EngineTrace::RayQueryBroker::Query, kMaxRayBatchSlots> edgeFanQueries{};
    size_t edgeFanCount = 0;

    // Slot 0 (+ extra slots if parabolic 2-3 chords): Top-1 primary bone ray(s).
    RayChordGroup top1Primary{};

    // Slot 1 (+ extra slots if multi-chord): Top-1 fallback bone (winning or unresolved) / warmup.
    RayChordGroup top1FallbackOrWarmup{};

    // Slot 2: Top-1 airborne ground-clip ray (when |v_y| > 0.1 m/s).
    std::optional<EngineTrace::RayQueryBroker::Query> top1GroundClip{};
    bool top1GroundClipUrgent = true;

    // Slot 3: Top-1 barrier mask query (only when BarrierPolicy::WaitBarrierDrop && world Clear).
    BarrierPolicy barrierPolicy = BarrierPolicy::IgnoreBarrier;
    bool top1WorldClear = false;
    std::optional<EngineTrace::RayQueryBroker::Query> top1BarrierQuery{};
    bool top1BarrierUrgent = true;

    // Slot 4 (+ extra slots if multi-chord): Top-2 secondary candidate primary bone warmup.
    RayChordGroup top2Primary{};

    // Slot 5: Top-2 fallback bone OR Top-3 primary bone warmup (full chord sets supported).
    RayChordGroup top2Fallback{};
    RayChordGroup top3Primary{};

    // Monotonic tick cursor for fair round-robin multi-chord refresh under saturation (F2 fix).
    uint32_t rotationCursor = 0;
};

// Dynamically fills up to 6 concurrent VisionProbe slots according to §6.3 quota and yield rules.
// Two-pass bounded rotating scheduler (F2 fix):
//   Pass 1: Bounded round-robin over urgent (missing/stale) queries with Top-1 primary pinned
//           first and the 6 non-primary sources rotated by rotationCursor so persistent urgency
//           (spatial drift / repeated vacuum) NEVER starves GroundClip, Barrier, Top-2, or Top-3.
//   Pass 2: Fair round-robin refresh of already-fresh queries rotated identically by rotationCursor.
inline RayBatchPlan PlanRayBatch(const RayBatchPlanInput& in) noexcept {
    RayBatchPlan plan{};

    // Special mode: EdgeFan borrows Slots 0-5 for 1-wave wall-edge discontinuity detection.
    if (in.edgeFanBorrowActive && in.edgeFanCount > 0) {
        plan.edgeFanBorrowed = true;
        const size_t limit = (std::min)(in.edgeFanCount, kMaxRayBatchSlots);
        for (size_t i = 0; i < limit; ++i) {
            plan.TryAdd(in.edgeFanQueries[i]);
        }
        return plan;
    }

    const bool barrierEligible =
        in.barrierPolicy == BarrierPolicy::WaitBarrierDrop &&
        in.top1WorldClear &&
        in.top1BarrierQuery.has_value();

    auto addUrgentRound = [&](const RayChordGroup& group, size_t round) {
        const size_t urgent = (std::min)({group.urgentCount, group.count, kMaxParabolicSegments});
        if (round >= urgent || plan.count >= kMaxRayBatchSlots)
            return;
        const size_t idx = (static_cast<size_t>(in.rotationCursor) + round) % urgent;
        plan.TryAdd(group.queries[idx]);
    };

    // Pass 1: Bounded rotating round-robin across urgent (missing/stale) groups.
    for (size_t round = 0; round < kMaxParabolicSegments && plan.count < kMaxRayBatchSlots; ++round) {
        addUrgentRound(in.top1Primary, round);
        for (size_t offset = 0; offset < 6 && plan.count < kMaxRayBatchSlots; ++offset) {
            const size_t src = (static_cast<size_t>(in.rotationCursor) + offset) % 6u;
            switch (src) {
                case 0:
                    if (round == 0 && in.top1GroundClip.has_value() && in.top1GroundClipUrgent) {
                        plan.TryAdd(*in.top1GroundClip);
                    }
                    break;
                case 1:
                    addUrgentRound(in.top1FallbackOrWarmup, round);
                    break;
                case 2:
                    if (round == 0 && barrierEligible && in.top1BarrierUrgent) {
                        plan.TryAdd(*in.top1BarrierQuery);
                    }
                    break;
                case 3:
                    addUrgentRound(in.top2Primary, round);
                    break;
                case 4:
                    addUrgentRound(in.top2Fallback, round);
                    break;
                case 5:
                    addUrgentRound(in.top3Primary, round);
                    break;
                default:
                    break;
            }
        }
    }

    if (plan.count >= kMaxRayBatchSlots) {
        return plan;
    }

    // Pass 2: Fair rotating round-robin refresh of already-fresh segments across groups.
    auto addRefreshRound = [&](const RayChordGroup& group, size_t round) {
        const size_t total = (std::min)(group.count, kMaxParabolicSegments);
        const size_t urgent = (std::min)(group.urgentCount, total);
        const size_t refreshCount = total - urgent;
        if (round >= refreshCount || plan.count >= kMaxRayBatchSlots)
            return;
        const size_t idx = urgent + ((static_cast<size_t>(in.rotationCursor) + round) % refreshCount);
        plan.TryAdd(group.queries[idx]);
    };

    for (size_t round = 0; round < kMaxParabolicSegments && plan.count < kMaxRayBatchSlots; ++round) {
        addRefreshRound(in.top1Primary, round);
        for (size_t offset = 0; offset < 6 && plan.count < kMaxRayBatchSlots; ++offset) {
            const size_t src = (static_cast<size_t>(in.rotationCursor) + offset) % 6u;
            switch (src) {
                case 0:
                    if (round == 0 && in.top1GroundClip.has_value() && !in.top1GroundClipUrgent) {
                        plan.TryAdd(*in.top1GroundClip);
                    }
                    break;
                case 1:
                    addRefreshRound(in.top1FallbackOrWarmup, round);
                    break;
                case 2:
                    if (round == 0 && barrierEligible && !in.top1BarrierUrgent) {
                        plan.TryAdd(*in.top1BarrierQuery);
                    }
                    break;
                case 3:
                    addRefreshRound(in.top2Primary, round);
                    break;
                case 4:
                    addRefreshRound(in.top2Fallback, round);
                    break;
                case 5:
                    addRefreshRound(in.top3Primary, round);
                    break;
                default:
                    break;
            }
        }
    }

    return plan;
}

// Phase 3 (§6.3 Slot 3, §8.1, §8.2, Decision #4): shared production + selftest seam for
// dynamic barrier-layer evaluation when VisibilityPolicy::RayWorldAndBarrier is active.
// Only runs when WaitBarrierDrop is selected, barrierMask != 0, and the world path is Clear.
struct BarrierEvaluationResult {
    bool barrierActive = false;         // True when Slot 3 barrier check governs this segment
    bool barrierBlocked = false;        // Fresh barrier ray returned Occluded -> TraceHitKind::Barrier
    bool barrierClear = false;          // Fresh barrier ray returned Clear
    bool barrierVacuum = false;         // Barrier ray missing/Stale/Timeout/Unavailable/Invalid
    TraceHitKind hitKind = TraceHitKind::None;
    RayDecision barrierDecision{};
    std::optional<EngineTrace::RayQueryBroker::Query> slot3Query{};
    bool slot3Urgent = false;
};

inline BarrierEvaluationResult EvaluateBarrierLayerDecision(
    EngineTrace::RayQueryBroker& broker,
    VisibilityPolicy visibilityPolicy,
    BarrierPolicy barrierPolicy,
    uint64_t barrierMask,
    bool worldClear,
    const Vector3& rayStart,
    const Vector3& rayEnd,
    uint64_t frame,
    uint64_t connectionEpoch,
    uint64_t sceneEpoch,
    uint64_t entityKey,
    uint64_t actorGeneration,
    int boneId,
    uint32_t sampleTick,
    uint32_t maxAgeMs,
    bool submitImmediately = false) noexcept {
    BarrierEvaluationResult out{};
    if (!worldClear) {
        out.hitKind = TraceHitKind::World;
        return out;
    }
    if (visibilityPolicy != VisibilityPolicy::RayWorldAndBarrier ||
        barrierPolicy != BarrierPolicy::WaitBarrierDrop ||
        barrierMask == 0 ||
        sceneEpoch == 0) {
        out.hitKind = TraceHitKind::None;
        return out;
    }

    out.barrierActive = true;

    EngineTrace::RayQueryBroker::Query q{};
    q.request.start = {rayStart.X, rayStart.Y, rayStart.Z};
    q.request.end = {rayEnd.X, rayEnd.Y, rayEnd.Z};
    q.request.mask = barrierMask;
    q.request.sceneEpoch = sceneEpoch;
    q.frame = frame;
    q.connectionEpoch = connectionEpoch;
    q.sceneEpoch = sceneEpoch;
    q.entityKey = entityKey;
    q.actorGeneration = actorGeneration;
    q.boneId = boneId;
    q.sampleTick = sampleTick;
    q.segmentKind = RaySegmentKind::Barrier;
    q.segmentIndex = 0;

    const auto cached = broker.Latest(
        q.request, frame, connectionEpoch, sceneEpoch,
        entityKey, actorGeneration, boneId, maxAgeMs,
        EngineTrace::RayQueryBroker::kDefaultMaxEndpointDriftMeters,
        RaySegmentKind::Barrier, 0);

    out.slot3Query = q;
    if (!cached.has_value()) {
        out.barrierVacuum = true;
        out.slot3Urgent = true;
        out.hitKind = TraceHitKind::Unknown;
        out.barrierDecision.world = RayDecisionStatus::Unknown;
        out.barrierDecision.segmentKind = RaySegmentKind::Barrier;
        out.barrierDecision.segmentIndex = 0;
    } else {
        out.barrierDecision = *cached;
        if (cached->world == RayDecisionStatus::Occluded && cached->Fresh(maxAgeMs)) {
            out.barrierBlocked = true;
            out.slot3Urgent = false;
            out.hitKind = TraceHitKind::Barrier;
        } else if (cached->world == RayDecisionStatus::Clear && cached->Fresh(maxAgeMs)) {
            out.barrierClear = true;
            out.slot3Urgent = false;
            out.hitKind = TraceHitKind::None;
        } else {
            out.barrierVacuum = true;
            out.slot3Urgent = true;
            out.hitKind = TraceHitKind::Unknown;
        }
    }

    if (submitImmediately) {
        broker.Submit(q);
    }
    return out;
}

}  // namespace OW::TrajectoryRay
