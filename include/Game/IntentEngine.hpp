#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "Game/TargetSolutionTypes.hpp"

namespace OW {

// Wraps an angular delta in degrees into (-180, 180].
inline float WrapDegrees180(float degrees) noexcept
{
    if (!std::isfinite(degrees))
        return 0.0f;
    float wrapped = std::fmod(degrees + 180.0f, 360.0f);
    if (wrapped < 0.0f)
        wrapped += 360.0f;
    wrapped -= 180.0f;
    if (wrapped <= -180.0f)
        wrapped += 360.0f;
    return wrapped;
}

struct IntentEngineConfig {
    // Expected transport + game-sampling pipeline latency for program-sent mouse output.
    uint64_t pipelineDelayUs = 8000;
    // Drop unconsumed program commands older than this horizon so stale commands never corrupt residuals.
    uint64_t maxPendingCommandAgeUs = 150000;
    // Below this per-frame residual (deg), treat motion as sensor/quantization noise.
    float noiseFloorDeg = 0.05f;
    // Per-frame residual threshold considered intentional drag when sustained.
    float activeDragThresholdDeg = 0.06f;
    // Number of consecutive frames above activeDragThresholdDeg to declare active drag.
    uint32_t activeDragMinFrames = 2;
    // Per-frame residual jump above this angle (deg) is rejected as teleport/respawn/charge.
    float maxValidFrameResidualDeg = 15.0f;
    // Asymmetric exponential filter constants (seconds) for intentGain.
    float gainDecayTauSec = 0.012f;      // 12 ms: rapid yield against human hand
    float gainRecoveryTauSec = 0.100f;   // 100 ms: smooth recovery without snap-back
    // Alignment threshold below which the human is explicitly pulling away from a target.
    float reverseAlignmentThreshold = -0.35f;
    // Cumulative reverse pull required to break commitment lock on the active target.
    float reverseBreakAngleDeg = 1.5f;
    float reverseBreakDurationMs = 45.0f;
    // Idle hold stance entry and FOV shrink settings.
    float idleEnterDurationMs = 200.0f;
    float idleFovDeg = 5.5f;
    float idleFovDecayTauSec = 0.120f;
    // Maximum plausible frame dt before resetting history.
    float maxSampleDtSec = 0.250f;
};

struct IntentFrameSnapshot {
    float residualYawDeg = 0.0f;
    float residualPitchDeg = 0.0f;
    float residualNormDeg = 0.0f;
    bool residualValid = false;
    bool activeDrag = false;
    uint32_t dragConsecutiveFrames = 0;

    InputStance stance = InputStance::ActiveDrag;
    float idleDurationMs = 0.0f;
    float idleFovBlend = 0.0f;           // 0 = ActiveDrag (full FOV), 1 = full IdleHold shrink

    float activeTargetAlignment = 0.0f;  // [-1, 1]
    float activeTargetConfidence = 0.0f; // [0, 1]
    float activeTargetGain = 1.0f;       // [0, 1]

    bool reverseBreakTriggered = false;
    bool reverseExemptByTargetReversal = false;
    float reverseAccumDeg = 0.0f;
    float reverseDurationMs = 0.0f;

    float dtSec = 0.0f;
    uint64_t sampleTimeUs = 0;
    uint64_t cameraSequence = 0;
    bool sampleUpdated = false;
};

// Camera-residual human intent estimator + IdleHold FOV state machine.
// Pure deterministic math with fixed-size ring buffer (zero heap allocations).
class IntentEngine {
public:
    static constexpr std::size_t kCommandRingCapacity = 64;

    explicit IntentEngine(const IntentEngineConfig& config = {}) noexcept
        : config_(config)
    {}

    void SetConfig(const IntentEngineConfig& config) noexcept
    {
        config_ = config;
    }

    const IntentEngineConfig& Config() const noexcept
    {
        return config_;
    }

    void Reset() noexcept
    {
        hasCameraSample_ = false;
        lastConnectionEpoch_ = 0;
        lastCameraSequence_ = 0;
        lastSampleTimeUs_ = 0;
        lastCameraYawDeg_ = 0.0f;
        lastCameraPitchDeg_ = 0.0f;
        commandHead_ = 0;
        commandCount_ = 0;
        for (auto& entry : commandRing_)
            entry = CommandEntry{};
        snapshot_ = IntentFrameSnapshot{};
        reverseQuietMs_ = 0.0f;
        feedbackPendingForCurrentSample_ = false;
    }

    void ClearReverseBreakState() noexcept
    {
        snapshot_.reverseBreakTriggered = false;
        snapshot_.reverseExemptByTargetReversal = false;
        snapshot_.reverseAccumDeg = 0.0f;
        snapshot_.reverseDurationMs = 0.0f;
        reverseQuietMs_ = 0.0f;
    }

    // Records an accepted program-emitted angular command in degrees
    // (same yaw/pitch sign convention as camera Euler angles).
    void RecordProgramSent(uint64_t sentAtUs, float deltaYawDeg, float deltaPitchDeg) noexcept
    {
        if (!std::isfinite(deltaYawDeg) || !std::isfinite(deltaPitchDeg))
            return;
        if (std::fabs(deltaYawDeg) <= 1e-6f && std::fabs(deltaPitchDeg) <= 1e-6f)
            return;

        if (commandCount_ == kCommandRingCapacity) {
            // Overwrite oldest entry when ring is full.
            commandHead_ = (commandHead_ + 1) % kCommandRingCapacity;
            --commandCount_;
        }
        const std::size_t slot = (commandHead_ + commandCount_) % kCommandRingCapacity;
        commandRing_[slot] = CommandEntry{
            sentAtUs,
            sentAtUs + config_.pipelineDelayUs,
            deltaYawDeg,
            deltaPitchDeg,
            true
        };
        ++commandCount_;
    }

    // Observes the latest camera orientation (degrees) and advances residual + IdleHold state.
    // When cameraSequence != 0, duplicate consumer polls of the same published camera sample
    // are deduplicated (no zero-motion injection, no premature delayed-command consumption).
    IntentFrameSnapshot ObserveCamera(uint64_t nowUs,
                                      float cameraYawDeg,
                                      float cameraPitchDeg,
                                      uint64_t connectionEpoch,
                                      bool hasCommittedTarget,
                                      uint64_t cameraSequence = 0) noexcept
    {
        snapshot_.sampleUpdated = false;
        if (!std::isfinite(cameraYawDeg) || !std::isfinite(cameraPitchDeg)) {
            snapshot_.residualYawDeg = 0.0f;
            snapshot_.residualPitchDeg = 0.0f;
            snapshot_.residualNormDeg = 0.0f;
            snapshot_.residualValid = false;
            return snapshot_;
        }

        const bool epochChanged = hasCameraSample_ && connectionEpoch != lastConnectionEpoch_;
        if (!hasCameraSample_ || epochChanged || nowUs < lastSampleTimeUs_) {
            Reset();
            hasCameraSample_ = true;
            lastConnectionEpoch_ = connectionEpoch;
            lastCameraSequence_ = cameraSequence;
            lastSampleTimeUs_ = nowUs;
            lastCameraYawDeg_ = cameraYawDeg;
            lastCameraPitchDeg_ = cameraPitchDeg;
            snapshot_.sampleTimeUs = nowUs;
            snapshot_.cameraSequence = cameraSequence;
            snapshot_.sampleUpdated = true;
            feedbackPendingForCurrentSample_ = true;
            return snapshot_;
        }

        if ((cameraSequence != 0 && cameraSequence == lastCameraSequence_) || nowUs == lastSampleTimeUs_) {
            // Duplicate consumer iteration over the same camera sample: preserve latest state
            // and any pending target-feedback flag without injecting zero motion or consuming commands.
            return snapshot_;
        }

        const float dtSec = static_cast<float>(nowUs - lastSampleTimeUs_) * 1e-6f;
        const float dtMs = dtSec * 1000.0f;
        const float maxDt = (std::max)(0.02f, config_.maxSampleDtSec);
        if (!std::isfinite(dtSec) || dtSec <= 0.0f || dtSec > maxDt) {
            // Long pause or hitch: re-anchor baseline cleanly.
            const InputStance prevStance = snapshot_.stance;
            const float prevIdleMs = snapshot_.idleDurationMs;
            const float prevBlend = snapshot_.idleFovBlend;
            Reset();
            hasCameraSample_ = true;
            lastConnectionEpoch_ = connectionEpoch;
            lastCameraSequence_ = cameraSequence;
            lastSampleTimeUs_ = nowUs;
            lastCameraYawDeg_ = cameraYawDeg;
            lastCameraPitchDeg_ = cameraPitchDeg;
            snapshot_.stance = hasCommittedTarget ? InputStance::ActiveDrag : prevStance;
            snapshot_.idleDurationMs = hasCommittedTarget ? 0.0f : prevIdleMs;
            snapshot_.idleFovBlend = hasCommittedTarget ? 0.0f : prevBlend;
            snapshot_.sampleTimeUs = nowUs;
            snapshot_.cameraSequence = cameraSequence;
            snapshot_.sampleUpdated = true;
            feedbackPendingForCurrentSample_ = true;
            return snapshot_;
        }

        const float camDeltaYaw = WrapDegrees180(cameraYawDeg - lastCameraYawDeg_);
        const float camDeltaPitch = cameraPitchDeg - lastCameraPitchDeg_;

        // Consume all program-sent commands expected to have arrived by nowUs.
        float programYaw = 0.0f;
        float programPitch = 0.0f;
        ConsumeArrivedCommands(nowUs, programYaw, programPitch);

        const float resYaw = camDeltaYaw - programYaw;
        const float resPitch = camDeltaPitch - programPitch;
        const float resNorm = std::hypot(resYaw, resPitch);

        lastSampleTimeUs_ = nowUs;
        lastCameraSequence_ = cameraSequence;
        lastCameraYawDeg_ = cameraYawDeg;
        lastCameraPitchDeg_ = cameraPitchDeg;
        snapshot_.dtSec = dtSec;
        snapshot_.sampleTimeUs = nowUs;
        snapshot_.cameraSequence = cameraSequence;
        snapshot_.sampleUpdated = true;
        feedbackPendingForCurrentSample_ = true;

        const float maxJump = (std::max)(1.0f, config_.maxValidFrameResidualDeg);
        const bool validFrame = std::isfinite(resNorm) && resNorm < maxJump;
        snapshot_.residualValid = validFrame;
        if (validFrame) {
            snapshot_.residualYawDeg = resYaw;
            snapshot_.residualPitchDeg = resPitch;
            snapshot_.residualNormDeg = resNorm;
        } else {
            // Large camera discontinuity (teleport/respawn/charge): reject frame without polluting intent.
            snapshot_.residualYawDeg = 0.0f;
            snapshot_.residualPitchDeg = 0.0f;
            snapshot_.residualNormDeg = 0.0f;
            snapshot_.dragConsecutiveFrames = 0;
            snapshot_.activeDrag = false;
            return snapshot_;
        }

        const float dragFloor = (std::max)(config_.noiseFloorDeg, config_.activeDragThresholdDeg);
        if (resNorm > dragFloor) {
            ++snapshot_.dragConsecutiveFrames;
        } else {
            snapshot_.dragConsecutiveFrames = 0;
        }
        snapshot_.activeDrag = snapshot_.dragConsecutiveFrames >= (std::max)(1u, config_.activeDragMinFrames);

        // IdleHold stance state machine:
        // - Any above-noise human residual recovers ActiveDrag in 1 frame.
        // - Committed target prevents shrinking into IdleHold while tracking.
        // - Otherwise accumulate idleDurationMs; after idleEnterDurationMs enter IdleHold
        //   and smoothly shrink FOV toward idleFovDeg.
        if (resNorm > config_.noiseFloorDeg) {
            snapshot_.idleDurationMs = 0.0f;
            snapshot_.stance = InputStance::ActiveDrag;
            snapshot_.idleFovBlend = 0.0f;
        } else if (hasCommittedTarget) {
            snapshot_.idleDurationMs = 0.0f;
            snapshot_.stance = InputStance::ActiveDrag;
            snapshot_.idleFovBlend = 0.0f;
        } else {
            snapshot_.idleDurationMs = (std::min)(snapshot_.idleDurationMs + dtMs, 60000.0f);
            if (snapshot_.idleDurationMs >= config_.idleEnterDurationMs) {
                snapshot_.stance = InputStance::IdleHold;
                const float tau = (std::max)(0.005f, config_.idleFovDecayTauSec);
                const float alpha = 1.0f - std::exp(-dtSec / tau);
                snapshot_.idleFovBlend = std::clamp(
                    snapshot_.idleFovBlend + (1.0f - snapshot_.idleFovBlend) * alpha,
                    0.0f, 1.0f);
            } else {
                snapshot_.stance = InputStance::ActiveDrag;
                snapshot_.idleFovBlend = 0.0f;
            }
        }

        return snapshot_;
    }

    // Evaluates direction alignment in [-1, 1] between current human residual and candidate angular error.
    // Returns 0 and sets *outConfidence = 0 when residual or error is below noise floor or invalid.
    float EvaluateAlignment(float errorYawDeg,
                            float errorPitchDeg,
                            float* outConfidence = nullptr) const noexcept
    {
        if (outConfidence)
            *outConfidence = 0.0f;
        if (!snapshot_.residualValid ||
            !std::isfinite(errorYawDeg) || !std::isfinite(errorPitchDeg)) {
            return 0.0f;
        }
        const float rNorm = snapshot_.residualNormDeg;
        const float eNorm = std::hypot(errorYawDeg, errorPitchDeg);
        const float eps = (std::max)(1e-4f, config_.noiseFloorDeg);
        const float maxJump = (std::max)(1.0f, config_.maxValidFrameResidualDeg);
        if (rNorm <= eps || rNorm >= maxJump || eNorm <= eps)
            return 0.0f;

        const float dot = snapshot_.residualYawDeg * errorYawDeg +
                          snapshot_.residualPitchDeg * errorPitchDeg;
        const float alignment = std::clamp(dot / (rNorm * eNorm), -1.0f, 1.0f);
        if (outConfidence) {
            // Confidence ramps cleanly from noiseFloor to 2x noiseFloor (1.0 above 2x noiseFloor).
            *outConfidence = std::clamp((rNorm - eps) / eps, 0.0f, 1.0f);
        }
        return alignment;
    }

    // Hard-blocks stealing lock onto an opposite-direction challenger while actively dragging (§3.3).
    bool IsOppositeDragHardBlocked(float errorYawDeg,
                                   float errorPitchDeg,
                                   bool isCurrentLocked) const noexcept
    {
        if (isCurrentLocked || !snapshot_.activeDrag || !snapshot_.residualValid)
            return false;
        float confidence = 0.0f;
        const float alignment = EvaluateAlignment(errorYawDeg, errorPitchDeg, &confidence);
        return confidence >= 0.5f && alignment < config_.reverseAlignmentThreshold;
    }

    // Updates asymmetric intentGain (12ms fast yield / 100ms smooth recovery) and reverse lock-break
    // state for the currently locked/tracked target.
    // If targetReversalActive is true and player residual aligns with the target's new velocity
    // angular direction (targetNewVelYawDeg, targetNewVelPitchDeg), reverse lock-break is exempted
    // (Decision #1: yield smoothly without breaking lock).
    IntentFrameSnapshot UpdateActiveTargetFeedback(float errorYawDeg,
                                                   float errorPitchDeg,
                                                   bool hasActiveTarget,
                                                   bool targetReversalActive = false,
                                                   float targetNewVelYawDeg = 0.0f,
                                                   float targetNewVelPitchDeg = 0.0f) noexcept
    {
        if (!feedbackPendingForCurrentSample_)
            return snapshot_;
        feedbackPendingForCurrentSample_ = false;
        snapshot_.sampleUpdated = false;
        const float dtSec = std::clamp(snapshot_.dtSec, 0.0f, 0.05f);
        const float dtMs = dtSec * 1000.0f;

        if (!hasActiveTarget) {
            snapshot_.activeTargetAlignment = 0.0f;
            snapshot_.activeTargetConfidence = 0.0f;
            const float tauUp = (std::max)(0.005f, config_.gainRecoveryTauSec);
            if (dtSec > 0.0f) {
                snapshot_.activeTargetGain +=
                    (1.0f - snapshot_.activeTargetGain) * (1.0f - std::exp(-dtSec / tauUp));
            } else {
                snapshot_.activeTargetGain = 1.0f;
            }
            snapshot_.activeTargetGain = std::clamp(snapshot_.activeTargetGain, 0.0f, 1.0f);
            ClearReverseBreakState();
            return snapshot_;
        }

        float confidence = 0.0f;
        const float alignment = EvaluateAlignment(errorYawDeg, errorPitchDeg, &confidence);
        snapshot_.activeTargetAlignment = alignment;
        snapshot_.activeTargetConfidence = confidence;

        float wantedGain = 1.0f;
        if (confidence > 0.0f) {
            const float rawWanted = std::clamp((alignment + 0.35f) / 0.55f, 0.0f, 1.0f);
            wantedGain = 1.0f + (rawWanted - 1.0f) * confidence;
        }
        const float tau = wantedGain < snapshot_.activeTargetGain
            ? (std::max)(0.002f, config_.gainDecayTauSec)
            : (std::max)(0.005f, config_.gainRecoveryTauSec);
        if (dtSec > 0.0f) {
            snapshot_.activeTargetGain +=
                (wantedGain - snapshot_.activeTargetGain) * (1.0f - std::exp(-dtSec / tau));
        }
        snapshot_.activeTargetGain = std::clamp(snapshot_.activeTargetGain, 0.0f, 1.0f);

        // Check sudden-stop / velocity reversal exemption (Decision #1):
        // Player anticipates target's new movement direction before lagged prediction catches up.
        bool exemptByReversal = false;
        if (targetReversalActive && snapshot_.residualValid && confidence > 0.0f) {
            const float velNorm = std::hypot(targetNewVelYawDeg, targetNewVelPitchDeg);
            if (std::isfinite(velNorm) && velNorm > 1e-4f && snapshot_.residualNormDeg > 1e-4f) {
                const float velDot = snapshot_.residualYawDeg * targetNewVelYawDeg +
                                     snapshot_.residualPitchDeg * targetNewVelPitchDeg;
                const float velAlign = velDot / (snapshot_.residualNormDeg * velNorm);
                if (velAlign > 0.35f) {
                    exemptByReversal = true;
                }
            }
        }
        snapshot_.reverseExemptByTargetReversal = exemptByReversal;

        if (confidence >= 0.5f && alignment < config_.reverseAlignmentThreshold) {
            if (exemptByReversal) {
                // Exempt from lock-break; allow gain attenuation only.
                snapshot_.reverseBreakTriggered = false;
                snapshot_.reverseAccumDeg = 0.0f;
                snapshot_.reverseDurationMs = 0.0f;
                reverseQuietMs_ = 0.0f;
            } else {
                reverseQuietMs_ = 0.0f;
                snapshot_.reverseAccumDeg += snapshot_.residualNormDeg * (-alignment);
                snapshot_.reverseDurationMs += dtMs;
                snapshot_.reverseBreakTriggered =
                    snapshot_.reverseAccumDeg >= config_.reverseBreakAngleDeg &&
                    snapshot_.reverseDurationMs >= config_.reverseBreakDurationMs;
            }
        } else {
            reverseQuietMs_ += dtMs;
            if (alignment > 0.1f || reverseQuietMs_ >= 40.0f) {
                snapshot_.reverseAccumDeg = 0.0f;
                snapshot_.reverseDurationMs = 0.0f;
                snapshot_.reverseBreakTriggered = false;
            }
        }

        return snapshot_;
    }

    // Computes effective selection FOV (deg) honoring IdleHold shrink and PreAim/PeekEmerge exemption (§3.3).
    float EffectiveSelectionFovDeg(float activeFovDeg,
                                   bool hasPreAimOrPeekEmerge = false,
                                   float peekGateDeg = 0.0f) const noexcept
    {
        if (!std::isfinite(activeFovDeg) || activeFovDeg <= 0.0f)
            return 0.0f;
        if (snapshot_.stance != InputStance::IdleHold || snapshot_.idleFovBlend <= 0.0f)
            return activeFovDeg;

        const float idleTarget = (std::min)(activeFovDeg, (std::max)(0.5f, config_.idleFovDeg));
        float effective = activeFovDeg + (idleTarget - activeFovDeg) * snapshot_.idleFovBlend;
        if (hasPreAimOrPeekEmerge && std::isfinite(peekGateDeg) && peekGateDeg > 0.0f) {
            const float relaxed = (std::min)(activeFovDeg, peekGateDeg);
            effective = (std::max)(effective, relaxed);
        }
        return effective;
    }

    const IntentFrameSnapshot& Snapshot() const noexcept
    {
        return snapshot_;
    }

private:
    struct CommandEntry {
        uint64_t sentAtUs = 0;
        uint64_t arriveAtUs = 0;
        float deltaYawDeg = 0.0f;
        float deltaPitchDeg = 0.0f;
        bool active = false;
    };

    void ConsumeArrivedCommands(uint64_t nowUs,
                                float& outProgramYawDeg,
                                float& outProgramPitchDeg) noexcept
    {
        outProgramYawDeg = 0.0f;
        outProgramPitchDeg = 0.0f;

        while (commandCount_ > 0) {
            const CommandEntry& head = commandRing_[commandHead_];
            if (!head.active) {
                commandHead_ = (commandHead_ + 1) % kCommandRingCapacity;
                --commandCount_;
                continue;
            }
            if (head.arriveAtUs > nowUs) {
                // Commands are appended in non-decreasing timestamp order.
                break;
            }
            const bool tooOld = nowUs > head.arriveAtUs &&
                (nowUs - head.arriveAtUs) > config_.maxPendingCommandAgeUs;
            if (!tooOld) {
                outProgramYawDeg += head.deltaYawDeg;
                outProgramPitchDeg += head.deltaPitchDeg;
            }
            commandRing_[commandHead_] = CommandEntry{};
            commandHead_ = (commandHead_ + 1) % kCommandRingCapacity;
            --commandCount_;
        }
    }

    IntentEngineConfig config_{};
    bool hasCameraSample_ = false;
    uint64_t lastConnectionEpoch_ = 0;
    uint64_t lastCameraSequence_ = 0;
    uint64_t lastSampleTimeUs_ = 0;
    float lastCameraYawDeg_ = 0.0f;
    float lastCameraPitchDeg_ = 0.0f;

    std::array<CommandEntry, kCommandRingCapacity> commandRing_{};
    std::size_t commandHead_ = 0;
    std::size_t commandCount_ = 0;

    IntentFrameSnapshot snapshot_{};
    float reverseQuietMs_ = 0.0f;
    bool feedbackPendingForCurrentSample_ = false;
};

} // namespace OW
