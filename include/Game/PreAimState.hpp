#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "Game/GameData.hpp"
#include "Game/RayDecision.hpp"
#include "Game/TargetSolutionTypes.hpp"
#include "Utils/Types.hpp"

namespace OW::PreAim {

inline constexpr float kDefaultPeekSpeedMps = 5.5f;
inline constexpr float kFastStrafePeekSpeedMps = 6.0f;
inline constexpr float kCrouchPeekSpeedMps = 3.0f;
inline constexpr float kDefaultPeekGraceSec = 0.25f;
inline constexpr float kDefaultBodyRadiusMeters = 0.35f;
inline constexpr float kEdgeFanFractionJump = 0.15f;
inline constexpr float kEdgeFanDepthJumpMeters = 1.5f;
inline constexpr size_t kEdgeFanRayCount = 6;

// Hero-aware full-speed peek speed (m/s, §5.2): Genji & Tracer walk at 6.0 m/s, others 5.5 m/s.
inline constexpr float DefaultPeekSpeedMps(uint64_t heroId) noexcept {
    if (heroId == GameData::MakeHeroId(0x003) ||  // Tracer
        heroId == GameData::MakeHeroId(0x029)) {  // Genji
        return kFastStrafePeekSpeedMps;
    }
    return kDefaultPeekSpeedMps;
}

// High-mobility peek / dive hero or airborne jump-peek detector (§5.5, Decision #2).
inline bool IsHighMobilityPeek(uint64_t heroId, float verticalVelocityMps = 0.0f) noexcept {
    if (std::isfinite(verticalVelocityMps) && std::fabs(verticalVelocityMps) > 1.0f) {
        return true;  // Airborne jump-peek (e.g., Widowmaker jump-snipe, Genji double jump)
    }
    switch (heroId) {
        case GameData::MakeHeroId(0x003):  // Tracer
        case GameData::MakeHeroId(0x029):  // Genji
        case GameData::MakeHeroId(0x079):  // Lucio
        case GameData::MakeHeroId(0x12E):  // Sombra
        case GameData::MakeHeroId(0x12F):  // Doomfist
        case GameData::MakeHeroId(0x1CA):  // Wrecking Ball
        case GameData::MakeHeroId(0x206):  // Echo
        case GameData::MakeHeroId(0x231):  // Kiriko
        case GameData::MakeHeroId(0x32B):  // Venture
            return true;
        default:
            return false;
    }
}

// Dynamic full-speed peek angle threshold theta_peek(d) in degrees (§5.2):
//   theta_peek(d) = min(FOV_base, atan(v_peek / v_proj + (v_peek * t_grace + r_body) / d))
inline float ComputeDynamicPeekAngleDeg(float distanceMeters,
                                        float fovBaseDeg,
                                        float projectileSpeedMps = 0.0f,
                                        float peekSpeedMps = kDefaultPeekSpeedMps,
                                        float graceSec = kDefaultPeekGraceSec,
                                        float bodyRadiusMeters = kDefaultBodyRadiusMeters) noexcept {
    if (!std::isfinite(fovBaseDeg) || fovBaseDeg <= 0.0f)
        return 0.0f;
    if (!std::isfinite(distanceMeters) || distanceMeters <= 0.05f)
        return fovBaseDeg;

    const float vPeek = (std::isfinite(peekSpeedMps) && peekSpeedMps > 0.0f)
        ? peekSpeedMps : kDefaultPeekSpeedMps;
    const float tGrace = (std::isfinite(graceSec) && graceSec >= 0.0f)
        ? graceSec : kDefaultPeekGraceSec;
    const float rBody = (std::isfinite(bodyRadiusMeters) && bodyRadiusMeters >= 0.0f)
        ? bodyRadiusMeters : kDefaultBodyRadiusMeters;

    const float leadTerm = (std::isfinite(projectileSpeedMps) && projectileSpeedMps > 0.0f)
        ? (vPeek / projectileSpeedMps)
        : 0.0f;
    const float geomTerm = (vPeek * tGrace + rBody) / distanceMeters;
    const float angleDeg = RAD2DEG(std::atan(leadTerm + geomTerm));
    if (!std::isfinite(angleDeg) || angleDeg <= 0.0f)
        return 0.0f;
    return (std::min)(fovBaseDeg, angleDeg);
}

// Hysteresis exit window for PreAim (§5.3 #2):
//   theta_exit(d) = min(FOV_base + 1.5 deg, 1.2 * theta_peek(d))
inline float ComputePeekExitAngleDeg(float peekAngleDeg, float fovBaseDeg) noexcept {
    if (!std::isfinite(peekAngleDeg) || peekAngleDeg <= 0.0f)
        return 0.0f;
    const float base = (std::isfinite(fovBaseDeg) && fovBaseDeg > 0.0f) ? fovBaseDeg : peekAngleDeg;
    return (std::min)(base + 1.5f, 1.2f * peekAngleDeg);
}

// Builds the horizontal unit vector perpendicular to eye->target in the XZ plane, oriented toward
// the expected peek-out side (target lateral velocity if strong enough, otherwise camera forward).
inline Vector3 ComputeRightAngleLateralDirection(const Vector3& eyePos,
                                                 const Vector3& targetPos,
                                                 const Vector3& cameraForward,
                                                 const Vector3& targetVelocity) noexcept {
    const float lx = targetPos.X - eyePos.X;
    const float lz = targetPos.Z - eyePos.Z;
    const float horizLen = std::hypot(lx, lz);
    if (!std::isfinite(horizLen) || horizLen <= 1e-4f)
        return Vector3(1.0f, 0.0f, 0.0f);

    // Perpendicular to (lx, 0, lz) in XZ plane: (+lz, 0, -lx) / horizLen.
    Vector3 perp(lz / horizLen, 0.0f, -lx / horizLen);

    // Prefer target lateral velocity when moving meaningfully across the sightline;
    // otherwise orient toward the side where the player's crosshair is already looking.
    const float velLat = (std::isfinite(targetVelocity.X) && std::isfinite(targetVelocity.Z))
        ? (targetVelocity.X * perp.X + targetVelocity.Z * perp.Z)
        : 0.0f;
    if (std::fabs(velLat) >= 0.8f) {
        if (velLat < 0.0f) perp = perp * -1.0f;
        return perp;
    }

    const float fwdLat = (std::isfinite(cameraForward.X) && std::isfinite(cameraForward.Z))
        ? (cameraForward.X * perp.X + cameraForward.Z * perp.Z)
        : 0.0f;
    if (fwdLat < 0.0f) perp = perp * -1.0f;
    return perp;
}

struct EdgeFanRaySpec {
    Vector3 start{};
    Vector3 end{};
    float lateralOffsetMeters = 0.0f;
    uint8_t segmentIndex = 0;  // 0..5 for Pass 1, 6..11 for Pass 2 refinement
};

// Constructs the 6 evenly spaced rays of the right-angle fan spanning [minOffset, maxOffset]
// along lateralDir at targetPos's plane (§5.4).
inline std::array<EdgeFanRaySpec, kEdgeFanRayCount> BuildEdgeFanRays(
    const Vector3& eyePos,
    const Vector3& targetPos,
    const Vector3& lateralDir,
    float minOffsetMeters,
    float maxOffsetMeters,
    bool refinementPass = false) noexcept {
    std::array<EdgeFanRaySpec, kEdgeFanRayCount> rays{};
    const float lo = std::isfinite(minOffsetMeters) ? minOffsetMeters : 0.0f;
    const float hi = (std::isfinite(maxOffsetMeters) && maxOffsetMeters > lo)
        ? maxOffsetMeters : (lo + 1.0f);
    const uint8_t indexBase = refinementPass ? static_cast<uint8_t>(kEdgeFanRayCount) : 0u;
    for (size_t i = 0; i < kEdgeFanRayCount; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kEdgeFanRayCount - 1);
        const float offset = lo + (hi - lo) * t;
        rays[i].start = eyePos;
        rays[i].end = targetPos + lateralDir * offset;
        rays[i].lateralOffsetMeters = offset;
        rays[i].segmentIndex = static_cast<uint8_t>(indexBase + i);
    }
    return rays;
}

inline constexpr float kEdgeFanMinUsableWidthMeters = 0.02f;

// Strictly caps the lateral fan width to the geometric limit d * tan(theta_peek(d)) (F10 fix).
// Never enlarges narrow windows; callers skip degenerate fans below kEdgeFanMinUsableWidthMeters.
inline float ComputeEdgeFanMaxWidthMeters(float distanceMeters,
                                          float peekGateDeg) noexcept {
    if (!std::isfinite(distanceMeters) || distanceMeters <= 0.05f ||
        !std::isfinite(peekGateDeg) || peekGateDeg <= 0.0f) {
        return 0.0f;
    }
    const float clampedDeg = std::clamp(peekGateDeg, 0.0f, 45.0f);
    const float width = distanceMeters * std::tan(DEG2RAD(clampedDeg));
    if (!std::isfinite(width) || width <= 0.0f)
        return 0.0f;
    return width;
}

enum class EdgeFanStatus : uint8_t {
    Pending = 0,          // Waiting for all 6 fresh verdicts from VisionProbe
    TargetDirectClear,    // Ray 0 itself is Clear -> target no longer occluded
    EdgeFound,            // Discontinuity detected between rays [bracketIndex-1, bracketIndex]
    AllBlocked,           // All 6 rays hit near wall -> deep cover, park with zero movement
};

struct EdgeFanAnalysis {
    EdgeFanStatus status = EdgeFanStatus::Pending;
    size_t bracketIndex = 0;       // First clear/breakthrough ray i in [1..5]
    float bracketLoMeters = 0.0f;  // Offset of wall-hit ray i-1
    float bracketHiMeters = 0.0f;  // Offset of breakthrough ray i
    float edgeOffsetMeters = 0.0f;
    float wallFraction = 0.0f;
    float clearFraction = 1.0f;
    Vector3 parkPoint{};           // Expected wall-edge emerge point (with outward body margin)
};

// Detects the first fraction / depth discontinuity across a 6-ray right-angle fan (§5.4).
// Requires all 6 entries to carry fresh Clear/Occluded verdicts (never treats Unknown/Stale as Clear).
inline EdgeFanAnalysis AnalyzeEdgeFanResults(
    const Vector3& eyePos,
    const Vector3& targetPos,
    const Vector3& lateralDir,
    const std::array<EdgeFanRaySpec, kEdgeFanRayCount>& rays,
    const std::array<std::optional<RayDecision>, kEdgeFanRayCount>& decisions,
    float maxFanWidthMeters,
    float bodyMarginMeters = kDefaultBodyRadiusMeters) noexcept {
    EdgeFanAnalysis out{};

    for (size_t i = 0; i < kEdgeFanRayCount; ++i) {
        if (!decisions[i].has_value())
            return out;  // Pending
        const RayDecisionStatus ws = decisions[i]->world;
        if (ws != RayDecisionStatus::Clear && ws != RayDecisionStatus::Occluded)
            return out;  // Pending (Stale / Unknown / Timeout / Unavailable)
        if (!std::isfinite(decisions[i]->fraction))
            return out;
    }

    if (decisions[0]->world == RayDecisionStatus::Clear || decisions[0]->fraction >= 0.999f) {
        if (rays[0].lateralOffsetMeters <= 1e-4f) {
            out.status = EdgeFanStatus::TargetDirectClear;
            out.parkPoint = targetPos;
            return out;
        }
        // Pass 2 refinement ray 0 lies at refineLoMeters_ > 0 (not at targetPos): if it is now
        // clear (e.g. slight movement), treat refineLoMeters_ as the refined wall-edge boundary.
        out.status = EdgeFanStatus::EdgeFound;
        out.bracketIndex = 0;
        out.bracketLoMeters = rays[0].lateralOffsetMeters;
        out.bracketHiMeters = rays[0].lateralOffsetMeters;
        out.edgeOffsetMeters = rays[0].lateralOffsetMeters;
        out.wallFraction = 0.0f;
        out.clearFraction = decisions[0]->world == RayDecisionStatus::Clear
            ? 1.0f : decisions[0]->fraction;
        const float margin = (std::isfinite(bodyMarginMeters) && bodyMarginMeters >= 0.0f)
            ? bodyMarginMeters : kDefaultBodyRadiusMeters;
        const float boundedMax = (std::isfinite(maxFanWidthMeters) && maxFanWidthMeters > 0.0f)
            ? maxFanWidthMeters : out.bracketHiMeters;
        const float parkOffset = (std::min)(boundedMax, out.edgeOffsetMeters + margin);
        out.parkPoint = targetPos + lateralDir * parkOffset;
        out.parkPoint.Y = targetPos.Y;
        return out;
    }

    for (size_t i = 1; i < kEdgeFanRayCount; ++i) {
        const float fPrev = decisions[i - 1]->world == RayDecisionStatus::Clear
            ? 1.0f : decisions[i - 1]->fraction;
        const float fCurr = decisions[i]->world == RayDecisionStatus::Clear
            ? 1.0f : decisions[i]->fraction;
        const float rayLen = (rays[i].end - eyePos).Size();
        const float deltaF = fCurr - fPrev;
        const float deltaDepth = deltaF * (std::isfinite(rayLen) ? rayLen : 0.0f);

        const bool becameClear = (decisions[i]->world == RayDecisionStatus::Clear || fCurr >= 0.999f);
        const bool sharpJump = deltaF > kEdgeFanFractionJump || deltaDepth > kEdgeFanDepthJumpMeters;
        if (becameClear || sharpJump) {
            out.status = EdgeFanStatus::EdgeFound;
            out.bracketIndex = i;
            out.bracketLoMeters = rays[i - 1].lateralOffsetMeters;
            out.bracketHiMeters = rays[i].lateralOffsetMeters;
            out.edgeOffsetMeters = 0.5f * (out.bracketLoMeters + out.bracketHiMeters);
            out.wallFraction = fPrev;
            out.clearFraction = fCurr;
            const float margin = (std::isfinite(bodyMarginMeters) && bodyMarginMeters >= 0.0f)
                ? bodyMarginMeters : kDefaultBodyRadiusMeters;
            const float boundedMax = (std::isfinite(maxFanWidthMeters) && maxFanWidthMeters > 0.0f)
                ? maxFanWidthMeters : out.bracketHiMeters;
            const float parkOffset = (std::min)(boundedMax, out.edgeOffsetMeters + margin);
            out.parkPoint = targetPos + lateralDir * parkOffset;
            out.parkPoint.Y = targetPos.Y;  // Keep target head/torso elevation
            return out;
        }
    }

    out.status = EdgeFanStatus::AllBlocked;
    return out;
}

enum class PreAimReleaseReason : uint8_t {
    None = 0,
    ReverseIntent,   // Exit #1: player pulled away >1.5 deg for 40-60ms
    AngularExit,     // Exit #2: angular error exceeded theta_exit(d)
    TargetRetreat,   // Exit #3: target retreating deeper behind cover for 250-350ms
    TtlExpired,      // Exit #4: continuous PreAim exceeded 600-1000ms without Engageable
};

inline constexpr const char* PreAimReleaseReasonName(PreAimReleaseReason reason) noexcept {
    switch (reason) {
        case PreAimReleaseReason::None: return "none";
        case PreAimReleaseReason::ReverseIntent: return "reverse_intent";
        case PreAimReleaseReason::AngularExit: return "angular_exit";
        case PreAimReleaseReason::TargetRetreat: return "target_retreat";
        case PreAimReleaseReason::TtlExpired: return "ttl_expired";
    }
    return "none";
}

struct PreAimConfig {
    float parkToleranceDeg = 1.0f;           // Once crosshair within 1.0 deg of P_park -> Parked (§5.4)
    float retreatTimeoutMs = 300.0f;         // Exit #3: 250-350 ms (§5.3)
    float ttlMs = 800.0f;                    // Exit #4: 600-1000 ms (§5.3)
    float retreatMinSpeedMps = 0.8f;         // Lateral speed deeper behind cover to count as retreating
    float retreatMinAngularRateDegPerSec = 0.5f;  // Polling-rate-invariant angular trend floor (F8 fix)
    float retreatPlateauResetMs = 120.0f;    // Reset retreat episode if angle plateaus without fresh increase
    bool enableRefinementPass = true;        // Optional Pass 2 local 6-ray refinement (§5.4)
};

// Two-stage wall-edge PreAim state machine (Positioning -> Parked) with 4 hard release exits
// (§5.1-§5.4, Decision #5). Once Parked, never re-enters Positioning for the same PreAim episode.
class PreAimTracker {
public:
    explicit PreAimTracker(const PreAimConfig& config = {}) noexcept : config_(config) {}

    void SetConfig(const PreAimConfig& config) noexcept { config_ = config; }
    const PreAimConfig& Config() const noexcept { return config_; }

    void Reset() noexcept {
        active_ = false;
        entityKey_ = 0;
        actorGeneration_ = 0;
        phase_ = PreAimPhase::None;
        lastReleaseReason_ = PreAimReleaseReason::None;
        enteredAtMs_ = 0;
        lastUpdateMs_ = 0;
        retreatEpisodeActive_ = false;
        retreatStartMs_ = 0;
        retreatStartAngleDeg_ = 0.0f;
        retreatPeakAngleDeg_ = 0.0f;
        lastAngleChangeMs_ = 0;
        lateralDir_ = {};
        maxFanWidthMeters_ = 0.0f;
        pass1Done_ = false;
        pass2Done_ = false;
        refineLoMeters_ = 0.0f;
        refineHiMeters_ = 0.0f;
        hasParkPoint_ = false;
        parkPoint_ = {};
        parkTargetOrigin_ = {};
    }

    // Clears both active PreAim state and any released-episode suppression (session/scene reset).
    void ResetAll() noexcept {
        Reset();
        suppressed_ = false;
        suppressedEntityKey_ = 0;
        suppressedActorGeneration_ = 0;
    }

    // Actor-specific reverse-intent break (F1 fix): releases & suppresses ONLY the broken actor,
    // never a different candidate selected afterward in the same tick.
    void NotifyReverseIntentBreak(uint64_t entityKey, uint64_t actorGeneration) noexcept {
        if (entityKey == 0)
            return;
        if (IsTrackingEntity(entityKey, actorGeneration)) {
            Reset();
            lastReleaseReason_ = PreAimReleaseReason::ReverseIntent;
        }
        SuppressEntity(entityKey, actorGeneration);
    }

    // Explicit angular exit for the currently tracked PreAim actor (F7 fix), usable even when
    // Stage 2 rejects the candidate before constructing a PreAim solution.
    bool ReleaseAngularExit(uint64_t entityKey, uint64_t actorGeneration) noexcept {
        if (!IsTrackingEntity(entityKey, actorGeneration))
            return false;
        Reset();
        lastReleaseReason_ = PreAimReleaseReason::AngularExit;
        return true;
    }

    // Rearms PreAim eligibility for an actor after verified emergence or explicit invalidation.
    void ClearSuppressionForEntity(uint64_t entityKey, uint64_t actorGeneration = 0) noexcept {
        if (suppressed_ && suppressedEntityKey_ == entityKey &&
            (actorGeneration == 0 || suppressedActorGeneration_ == 0 ||
             suppressedActorGeneration_ == actorGeneration)) {
            suppressed_ = false;
            suppressedEntityKey_ = 0;
            suppressedActorGeneration_ = 0;
        }
    }

    bool IsSuppressedForEntity(uint64_t entityKey, uint64_t actorGeneration) const noexcept {
        return suppressed_ && suppressedEntityKey_ == entityKey &&
               (actorGeneration == 0 || suppressedActorGeneration_ == 0 ||
                suppressedActorGeneration_ == actorGeneration);
    }

    // Observes an entity outside the exit window to rearm a previously TTL/retreat-suppressed episode.
    void ObserveOutsideWindow(uint64_t entityKey, uint64_t actorGeneration) noexcept {
        ClearSuppressionForEntity(entityKey, actorGeneration);
    }

    bool Active() const noexcept { return active_; }
    uint64_t EntityKey() const noexcept { return entityKey_; }
    uint64_t ActorGeneration() const noexcept { return actorGeneration_; }
    PreAimPhase Phase() const noexcept { return phase_; }
    PreAimReleaseReason LastReleaseReason() const noexcept { return lastReleaseReason_; }
    bool HasParkPoint() const noexcept { return hasParkPoint_; }
    Vector3 ParkPoint() const noexcept { return parkPoint_; }
    Vector3 LateralDir() const noexcept { return lateralDir_; }
    float MaxFanWidthMeters() const noexcept { return maxFanWidthMeters_; }

    bool IsTrackingEntity(uint64_t entityKey, uint64_t actorGeneration) const noexcept {
        return active_ && entityKey_ == entityKey &&
               (actorGeneration == 0 || actorGeneration_ == actorGeneration);
    }

    // Returns true when an edge-fan 6-ray batch should borrow Slots 0-5 (§6.3).
    bool NeedsEdgeFanPass(bool& outIsRefinementPass,
                          float& outLoMeters,
                          float& outHiMeters) const noexcept {
        if (!active_ || phase_ != PreAimPhase::Positioning)
            return false;
        if (!std::isfinite(maxFanWidthMeters_) || maxFanWidthMeters_ < kEdgeFanMinUsableWidthMeters)
            return false;
        if (!pass1Done_) {
            outIsRefinementPass = false;
            outLoMeters = 0.0f;
            outHiMeters = maxFanWidthMeters_;
            return true;
        }
        if (config_.enableRefinementPass && !pass2Done_ && refineHiMeters_ > refineLoMeters_ + 0.05f) {
            outIsRefinementPass = true;
            outLoMeters = refineLoMeters_;
            outHiMeters = refineHiMeters_;
            return true;
        }
        return false;
    }

    // Attempts to enter or maintain PreAim for an occluded candidate.
    // Returns true if the target stays in PreAim, false if rejected or released by one of the 4 exits.
    bool Update(uint64_t entityKey,
                uint64_t actorGeneration,
                uint64_t nowMs,
                float angularErrorDeg,
                float distanceMeters,
                float fovBaseDeg,
                float peekGateDeg,
                const Vector3& eyePos,
                const Vector3& targetPos,
                const Vector3& cameraForward,
                const Vector3& targetVelocity,
                bool reverseIntentBroken,
                bool canUseEdgeFan) noexcept {
        if (entityKey == 0 || !std::isfinite(angularErrorDeg) || !std::isfinite(peekGateDeg) ||
            peekGateDeg <= 0.0f) {
            Reset();
            return false;
        }

        // Exit #1: player reverse intent always releases PreAim immediately and suppresses
        // immediate re-entry for this cover episode until verified emergence or leaving window.
        if (reverseIntentBroken) {
            Reset();
            SuppressEntity(entityKey, actorGeneration);
            lastReleaseReason_ = PreAimReleaseReason::ReverseIntent;
            return false;
        }

        const float exitDeg = ComputePeekExitAngleDeg(peekGateDeg, fovBaseDeg);
        const bool sameEntity = IsTrackingEntity(entityKey, actorGeneration);
        if (!sameEntity) {
            // If this actor previously exhausted TTL/Retreat/ReverseIntent, keep it suppressed
            // until it leaves the exit window or actually emerges to verified Engageable (Finding #8).
            if (IsSuppressedForEntity(entityKey, actorGeneration)) {
                if (angularErrorDeg > exitDeg) {
                    ClearSuppressionForEntity(entityKey, actorGeneration);
                }
                return false;
            }
            // Entry requires angular error inside theta_peek(d).
            if (angularErrorDeg > peekGateDeg) {
                return false;
            }
            Reset();
            active_ = true;
            entityKey_ = entityKey;
            actorGeneration_ = actorGeneration;
            enteredAtMs_ = nowMs;
            lastUpdateMs_ = nowMs;
            lateralDir_ = ComputeRightAngleLateralDirection(eyePos, targetPos, cameraForward, targetVelocity);
            maxFanWidthMeters_ = ComputeEdgeFanMaxWidthMeters(distanceMeters, peekGateDeg);
            parkTargetOrigin_ = targetPos;
            // Without an active ray backend or with a degenerate (<2cm) geometric fan window (F10),
            // skip Positioning and go straight to Parked (never enlarge narrow windows or pull blindly).
            const bool fanUsable = canUseEdgeFan &&
                std::isfinite(maxFanWidthMeters_) &&
                maxFanWidthMeters_ >= kEdgeFanMinUsableWidthMeters;
            if (!fanUsable) {
                pass1Done_ = true;
                pass2Done_ = true;
                phase_ = PreAimPhase::Parked;
            } else {
                phase_ = PreAimPhase::Positioning;
            }
            return true;
        }

        lastUpdateMs_ = nowMs;

        // Exit #2: angular error exceeded hysteresis exit window theta_exit(d).
        if (angularErrorDeg > exitDeg) {
            Reset();
            lastReleaseReason_ = PreAimReleaseReason::AngularExit;
            return false;
        }

        // Exit #4: hard TTL timeout without becoming Engageable.
        if (nowMs >= enteredAtMs_ && static_cast<float>(nowMs - enteredAtMs_) >= config_.ttlMs) {
            Reset();
            SuppressEntity(entityKey, actorGeneration);
            lastReleaseReason_ = PreAimReleaseReason::TtlExpired;
            return false;
        }

        // Exit #3: target retreating deeper behind cover (BOTH lateral velocity deeper behind cover
        // AND time-based monotonic angular increase) continuously for retreatTimeoutMs (§5.3, F8 fix).
        // Decoupled from polling frequency: duplicate observations between entity updates do NOT reset
        // continuity, while stationary/decreasing angular error prevents false retreat triggers.
        const float velTowardPeekEdge =
            (std::isfinite(targetVelocity.X) && std::isfinite(targetVelocity.Z))
                ? (targetVelocity.X * lateralDir_.X + targetVelocity.Z * lateralDir_.Z)
                : 0.0f;
        const bool movingDeeper = velTowardPeekEdge < -config_.retreatMinSpeedMps;
        if (!movingDeeper) {
            retreatEpisodeActive_ = false;
            retreatStartMs_ = 0;
            retreatStartAngleDeg_ = 0.0f;
            retreatPeakAngleDeg_ = 0.0f;
            lastAngleChangeMs_ = 0;
        } else if (!retreatEpisodeActive_ || nowMs < retreatStartMs_) {
            retreatEpisodeActive_ = true;
            retreatStartMs_ = nowMs;
            retreatStartAngleDeg_ = angularErrorDeg;
            retreatPeakAngleDeg_ = angularErrorDeg;
            lastAngleChangeMs_ = nowMs;
        } else if (angularErrorDeg < retreatPeakAngleDeg_ - 0.01f) {
            // Even a small distinct angular decrease toward the peek edge interrupts the
            // monotonic retreat trend and restarts the baseline (F8 fix).
            retreatStartMs_ = nowMs;
            retreatStartAngleDeg_ = angularErrorDeg;
            retreatPeakAngleDeg_ = angularErrorDeg;
            lastAngleChangeMs_ = nowMs;
        } else {
            if (angularErrorDeg > retreatPeakAngleDeg_ + 1e-4f) {
                retreatPeakAngleDeg_ = angularErrorDeg;
                lastAngleChangeMs_ = nowMs;
            }
            // Check plateau expiry BEFORE release: duplicate polling samples within a normal
            // entity update interval (< retreatPlateauResetMs) preserve continuity, while an
            // early jump followed by a sustained plateau resets the episode baseline (F8 fix).
            const float plateauLimitMs = (std::max)(20.0f, config_.retreatPlateauResetMs);
            if (nowMs >= lastAngleChangeMs_ &&
                static_cast<float>(nowMs - lastAngleChangeMs_) >= plateauLimitMs) {
                retreatStartMs_ = nowMs;
                retreatStartAngleDeg_ = angularErrorDeg;
                retreatPeakAngleDeg_ = angularErrorDeg;
                lastAngleChangeMs_ = nowMs;
            } else {
                const float elapsedMs = static_cast<float>(nowMs - retreatStartMs_);
                const float totalAngleGainDeg = angularErrorDeg - retreatStartAngleDeg_;
                const float minRateDegPerSec = (std::max)(0.0f, config_.retreatMinAngularRateDegPerSec);
                const float minRequiredGainDeg = (std::max)(0.05f, (elapsedMs / 1000.0f) * minRateDegPerSec);
                if (elapsedMs >= config_.retreatTimeoutMs && totalAngleGainDeg >= minRequiredGainDeg) {
                    Reset();
                    SuppressEntity(entityKey, actorGeneration);
                    lastReleaseReason_ = PreAimReleaseReason::TargetRetreat;
                    return false;
                }
            }
        }
        return true;
    }

    // Consumes a completed 6-ray EdgeFan analysis (Pass 1 or Pass 2).
    void ApplyEdgeFanAnalysis(const EdgeFanAnalysis& analysis, bool wasRefinementPass) noexcept {
        if (!active_ || phase_ != PreAimPhase::Positioning)
            return;
        if (analysis.status == EdgeFanStatus::Pending)
            return;

        if (analysis.status == EdgeFanStatus::TargetDirectClear) {
            // Target ray 0 itself is already clear: immediately release all 6 EdgeFan slots
            // (never loop borrowing Slots 0-5 until TTL) so top1Primary refreshes to Engageable.
            pass1Done_ = true;
            pass2Done_ = true;
            hasParkPoint_ = false;
            phase_ = PreAimPhase::Parked;
            return;
        }

        if (analysis.status == EdgeFanStatus::AllBlocked) {
            // Deep behind cover with no edge found: skip movement and enter Parked immediately (§5.4).
            pass1Done_ = true;
            pass2Done_ = true;
            hasParkPoint_ = false;
            phase_ = PreAimPhase::Parked;
            return;
        }

        if (analysis.status == EdgeFanStatus::EdgeFound) {
            hasParkPoint_ = true;
            parkPoint_ = analysis.parkPoint;
            if (!wasRefinementPass) {
                pass1Done_ = true;
                refineLoMeters_ = analysis.bracketLoMeters;
                refineHiMeters_ = analysis.bracketHiMeters;
            } else {
                pass2Done_ = true;
            }
        }
    }

    // Once crosshair reaches within parkToleranceDeg of parkPoint, transition permanently
    // to PreAimPhase::Parked (100% hands-off, never re-triggers Positioning).
    void ObserveParkCrosshairError(float parkAngularErrorDeg) noexcept {
        if (!active_ || phase_ != PreAimPhase::Positioning || !hasParkPoint_)
            return;
        if (std::isfinite(parkAngularErrorDeg) && parkAngularErrorDeg <= config_.parkToleranceDeg) {
            phase_ = PreAimPhase::Parked;
        }
    }

    Vector3 FanAnchorTargetPos() const noexcept { return parkTargetOrigin_; }

private:
    void SuppressEntity(uint64_t entityKey, uint64_t actorGeneration) noexcept {
        suppressed_ = entityKey != 0;
        suppressedEntityKey_ = entityKey;
        suppressedActorGeneration_ = actorGeneration;
    }

    PreAimConfig config_{};
    bool active_ = false;
    uint64_t entityKey_ = 0;
    uint64_t actorGeneration_ = 0;
    bool suppressed_ = false;
    uint64_t suppressedEntityKey_ = 0;
    uint64_t suppressedActorGeneration_ = 0;
    PreAimPhase phase_ = PreAimPhase::None;
    PreAimReleaseReason lastReleaseReason_ = PreAimReleaseReason::None;
    uint64_t enteredAtMs_ = 0;
    uint64_t lastUpdateMs_ = 0;
    bool retreatEpisodeActive_ = false;
    uint64_t retreatStartMs_ = 0;
    float retreatStartAngleDeg_ = 0.0f;
    float retreatPeakAngleDeg_ = 0.0f;
    uint64_t lastAngleChangeMs_ = 0;
    Vector3 lateralDir_{};
    float maxFanWidthMeters_ = 0.0f;
    bool pass1Done_ = false;
    bool pass2Done_ = false;
    float refineLoMeters_ = 0.0f;
    float refineHiMeters_ = 0.0f;
    bool hasParkPoint_ = false;
    Vector3 parkPoint_{};
    Vector3 parkTargetOrigin_{};
};

struct ReactionDelayConfig {
    bool enabled = true;
    float standardWindowMs = 140.0f;      // §5.5: 120-160 ms baseline
    float highMobilityWindowMs = 200.0f;  // §5.5: 180-240 ms for dive/blink/jump-peek
    uint64_t recentOcclusionMemoryMs = 1500;
};

// Unified Peek Reaction Delay Gate (§5.5, Decision #2):
// Tracks recent occlusion / PreAim history per actor and gates Stage 3 pull & auto-fire
// for reactionWindowMs upon PeekEmerge without introducing a separate behavior mode.
class ReactionDelayGate {
public:
    static constexpr size_t kCapacity = 32;

    explicit ReactionDelayGate(const ReactionDelayConfig& config = {}) noexcept
        : config_(config) {}

    void SetConfig(const ReactionDelayConfig& config) noexcept { config_ = config; }
    const ReactionDelayConfig& Config() const noexcept { return config_; }

    void Reset() noexcept {
        for (auto& entry : entries_) entry = Entry{};
    }

    void InvalidateEntity(uint64_t entityKey, uint64_t actorGeneration) noexcept {
        for (auto& entry : entries_) {
            if (entry.used && entry.entityKey == entityKey &&
                (actorGeneration == 0 || entry.actorGeneration == actorGeneration)) {
                entry = Entry{};
            }
        }
    }

    // Records that an actor was observed occluded or held in PreAim at nowMs.
    void ObserveOccludedOrPreAim(uint64_t entityKey,
                                 uint64_t actorGeneration,
                                 uint64_t nowMs,
                                 bool inPreAim) noexcept {
        if (entityKey == 0) return;
        Entry& slot = FindOrAllocate(entityKey, actorGeneration, nowMs);
        slot.lastOccludedAtMs = nowMs;
        slot.lastTouchedAtMs = nowMs;
        slot.wasPreAim = slot.wasPreAim || inPreAim;
        // Re-arm emergence gate whenever the target returns behind cover.
        slot.emergedAtMs = 0;
        slot.activeWindowMs = 0.0f;
    }

    // Called when an actor is currently Engageable.
    // Returns remaining reaction delay in ms (>0 means Stage 3 must defer pull & auto-fire).
    // Sets outPeekEmerge = true when the target just emerged from PreAim or recent occlusion.
    float OnEngageable(uint64_t entityKey,
                       uint64_t actorGeneration,
                       uint64_t heroId,
                       float verticalVelocityMps,
                       uint64_t nowMs,
                       bool& outPeekEmerge) noexcept {
        outPeekEmerge = false;
        if (entityKey == 0 || !config_.enabled)
            return 0.0f;

        Entry* slot = Find(entityKey, actorGeneration);
        if (!slot)
            return 0.0f;

        slot->lastTouchedAtMs = nowMs;
        if (slot->emergedAtMs == 0) {
            // Both ordinary occlusion and PreAim history must expire after recentOcclusionMemoryMs
            // so stale PreAim records never trigger a delay in open space later (Finding #14).
            const bool withinMemory = slot->lastOccludedAtMs != 0 &&
                nowMs >= slot->lastOccludedAtMs &&
                (nowMs - slot->lastOccludedAtMs) <= config_.recentOcclusionMemoryMs;
            if (!withinMemory) {
                slot->wasPreAim = false;
                slot->lastOccludedAtMs = 0;
                return 0.0f;
            }
            slot->emergedAtMs = nowMs;
            slot->activeWindowMs = IsHighMobilityPeek(heroId, verticalVelocityMps)
                ? (std::max)(0.0f, config_.highMobilityWindowMs)
                : (std::max)(0.0f, config_.standardWindowMs);
            slot->wasPreAim = false;
            slot->lastOccludedAtMs = 0;
            outPeekEmerge = true;
            return slot->activeWindowMs;
        }

        const float elapsedMs = nowMs >= slot->emergedAtMs
            ? static_cast<float>(nowMs - slot->emergedAtMs)
            : 0.0f;
        if (elapsedMs < slot->activeWindowMs) {
            outPeekEmerge = true;
            return slot->activeWindowMs - elapsedMs;
        }
        return 0.0f;
    }

    // Returns true if the actor is eligible for IdleHold FOV relaxation (§3.3, Decision #2):
    // either recently in PreAim or currently inside/just past a PeekEmerge window.
    bool HasRecentPreAimOrPeek(uint64_t entityKey,
                               uint64_t actorGeneration,
                               uint64_t nowMs) const noexcept {
        const Entry* slot = Find(entityKey, actorGeneration);
        if (!slot) return false;
        if (slot->wasPreAim && slot->lastOccludedAtMs != 0 &&
            nowMs >= slot->lastOccludedAtMs &&
            (nowMs - slot->lastOccludedAtMs) <= config_.recentOcclusionMemoryMs) {
            return true;
        }
        if (slot->emergedAtMs != 0 && nowMs >= slot->emergedAtMs) {
            const float graceWindow = (std::max)(slot->activeWindowMs + 250.0f, 400.0f);
            if (static_cast<float>(nowMs - slot->emergedAtMs) <= graceWindow) {
                return true;
            }
        }
        return false;
    }

private:
    struct Entry {
        bool used = false;
        uint64_t entityKey = 0;
        uint64_t actorGeneration = 0;
        uint64_t lastOccludedAtMs = 0;
        uint64_t lastTouchedAtMs = 0;
        uint64_t emergedAtMs = 0;
        float activeWindowMs = 0.0f;
        bool wasPreAim = false;
    };

    Entry* Find(uint64_t entityKey, uint64_t actorGeneration) noexcept {
        for (auto& entry : entries_) {
            if (entry.used && entry.entityKey == entityKey &&
                entry.actorGeneration == actorGeneration) {
                return &entry;
            }
        }
        return nullptr;
    }

    const Entry* Find(uint64_t entityKey, uint64_t actorGeneration) const noexcept {
        for (const auto& entry : entries_) {
            if (entry.used && entry.entityKey == entityKey &&
                entry.actorGeneration == actorGeneration) {
                return &entry;
            }
        }
        return nullptr;
    }

    Entry& FindOrAllocate(uint64_t entityKey, uint64_t actorGeneration, uint64_t nowMs) noexcept {
        if (Entry* existing = Find(entityKey, actorGeneration))
            return *existing;
        size_t victim = 0;
        uint64_t oldestTouch = UINT64_MAX;
        for (size_t i = 0; i < kCapacity; ++i) {
            if (!entries_[i].used) {
                victim = i;
                break;
            }
            if (entries_[i].lastTouchedAtMs < oldestTouch) {
                oldestTouch = entries_[i].lastTouchedAtMs;
                victim = i;
            }
        }
        entries_[victim] = Entry{};
        entries_[victim].used = true;
        entries_[victim].entityKey = entityKey;
        entries_[victim].actorGeneration = actorGeneration;
        entries_[victim].lastTouchedAtMs = nowMs;
        return entries_[victim];
    }

    ReactionDelayConfig config_{};
    std::array<Entry, kCapacity> entries_{};
};

// Tracks per-actor Hybrid observation vacuum elapsed time (Decision #7: <= 60 ms tracking grace
// when native entity.Vis == true, immediately cleared on Fresh Clear or Vis == false).
class VacuumGraceTracker {
public:
    static constexpr size_t kCapacity = 16;

    void Reset() noexcept {
        for (auto& e : entries_) e = Entry{};
    }

    void RecordClear(uint64_t entityKey, uint64_t actorGeneration, uint64_t nowMs) noexcept {
        if (entityKey == 0) return;
        Entry& slot = FindOrAllocate(entityKey, actorGeneration, nowMs);
        slot.hadRecentClear = true;
        slot.lastClearAtMs = nowMs;
        slot.vacuumStartedAtMs = 0;
        slot.lastTouchedAtMs = nowMs;
    }

    void ClearEntity(uint64_t entityKey, uint64_t actorGeneration) noexcept {
        for (auto& e : entries_) {
            if (e.used && e.entityKey == entityKey &&
                (actorGeneration == 0 || e.actorGeneration == actorGeneration)) {
                e = Entry{};
            }
        }
    }

    // Returns elapsed ms in the current observation vacuum. Returns UINT32_MAX if actor has no
    // prior Clear continuity (never fabricates tracking grace on a brand-new unseen target behind cover
    // unless Vis==true allows initial cold-cache warmup grace).
    uint32_t ObserveVacuum(uint64_t entityKey,
                           uint64_t actorGeneration,
                           bool entityVis,
                           uint64_t nowMs) noexcept {
        if (entityKey == 0 || !entityVis) {
            ClearEntity(entityKey, actorGeneration);
            return UINT32_MAX;
        }
        Entry& slot = FindOrAllocate(entityKey, actorGeneration, nowMs);
        slot.lastTouchedAtMs = nowMs;
        if (slot.vacuumStartedAtMs == 0) {
            slot.vacuumStartedAtMs = nowMs;
        }
        return nowMs >= slot.vacuumStartedAtMs
            ? static_cast<uint32_t>((std::min<uint64_t>)(nowMs - slot.vacuumStartedAtMs, UINT32_MAX))
            : 0u;
    }

private:
    struct Entry {
        bool used = false;
        uint64_t entityKey = 0;
        uint64_t actorGeneration = 0;
        bool hadRecentClear = false;
        uint64_t lastClearAtMs = 0;
        uint64_t vacuumStartedAtMs = 0;
        uint64_t lastTouchedAtMs = 0;
    };

    Entry& FindOrAllocate(uint64_t entityKey, uint64_t actorGeneration, uint64_t nowMs) noexcept {
        for (auto& e : entries_) {
            if (e.used && e.entityKey == entityKey && e.actorGeneration == actorGeneration)
                return e;
        }
        size_t victim = 0;
        uint64_t oldest = UINT64_MAX;
        for (size_t i = 0; i < kCapacity; ++i) {
            if (!entries_[i].used) {
                victim = i;
                break;
            }
            if (entries_[i].lastTouchedAtMs < oldest) {
                oldest = entries_[i].lastTouchedAtMs;
                victim = i;
            }
        }
        entries_[victim] = Entry{};
        entries_[victim].used = true;
        entries_[victim].entityKey = entityKey;
        entries_[victim].actorGeneration = actorGeneration;
        entries_[victim].lastTouchedAtMs = nowMs;
        return entries_[victim];
    }

    std::array<Entry, kCapacity> entries_{};
};

}  // namespace OW::PreAim
