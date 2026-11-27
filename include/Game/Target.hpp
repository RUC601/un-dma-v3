#pragma once
#include <cstdint>
#include "Game/PresentationTiming.hpp"
#include <cmath>
#include <algorithm>
#include <complex>
#include <vector>
#include <string>
#include <ctime>
#include <limits>
#include <mutex>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <atomic>
#include <functional>
#include <map>
#include <unordered_map>
#include <windows.h>
#include <chrono>
#include <optional>
#include <thread>
#include <utility>

#include "Game/Decrypt.hpp"
#include "Game/AimFirePhase.hpp"
#include "Game/AimHybridController.hpp"
#include "Game/AimSmoothingTiming.hpp"
#include "Game/CalibrationPolicy.hpp"
#include "Game/Entity.hpp"
#include "Game/IntentEngine.hpp"
#include "Game/snapshots/SnapshotAccess.hpp"
#include "Game/FovGeometry.hpp"
#include "Game/HeroGeometrySpec.hpp"
#include "Game/TargetHitWindow.hpp"
#include "Game/TargetPipeline.hpp"
#include "Game/RoleUtilityPlugin.hpp"
#include "Game/PreAimState.hpp"
#include "Game/TrajectoryRayPlanner.hpp"
#include "output/InputOrchestrator.hpp"
#include "Game/LeadPrediction.hpp"
#include "Game/Motion.hpp"
#include "Game/RuntimeState.hpp"
#include "Game/TrackingContinuityPolicy.hpp"
#include "Game/TriggerBoneSelection.hpp"
#include "Game/RayRuntime.hpp"
#include "Game/V3Raycast.hpp"
#include "config/UiPreferences.hpp"
#include "Game/WeaponSpec.hpp"
#include "output/OutputRuntime.hpp"
#include "output/Actions.hpp"
#include "output/PhysicalMask.hpp"
#include "features/aim/Smoothing.hpp"
#include "Utils/Config.hpp"
#include "Utils/Diagnostics.hpp"
#include "Utils/InputLabels.hpp"
#include "Utils/ProcessConnection.hpp"
#include "vision/ContourService.hpp"

extern std::mutex g_mutex;

namespace OW {

    inline constexpr uint64_t kPlayerControllerViewDirectionOffset = 0x1260;

    inline Vector3 DirectionToAimEuler(const Vector3& direction) {
        const float len = direction.Size();
        if (!std::isfinite(len) || len <= 1e-6f)
            return Vector3{};
        const Vector3 unit = direction / len;
        return Vector3(
            -asinf(std::clamp(unit.Y, -1.0f, 1.0f)),
            atan2f(unit.X, unit.Z),
            0.0f
        );
    }

    inline Vector3 ReadPlayerControllerViewDirection(uint64_t playerControllerBase) {
        if (!playerControllerBase || !RuntimeWorkersRunning() ||
            !ProcessConnection::IsConnected() || !SDK || !SDK->IsInitialized())
            return Vector3{};
        if(consumerFrame)return consumerFrame->local.AngleBase==playerControllerBase ? consumerFrame->local.view_direction : Vector3{};
        return SDK->RPM<Vector3>(playerControllerBase + kPlayerControllerViewDirectionOffset);
    }

    // =========================================================================
    // Global extern declarations (defined in Overwatch.hpp)
    // =========================================================================

    inline int ReadPresentEnvFlagState(const char* name, int defaultValue)
    {
        char buffer[32]{};
        const DWORD length = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
        if (length == 0)
            return defaultValue;
        if (length >= sizeof(buffer))
            return defaultValue;

        std::string value(buffer, buffer + (std::min<DWORD>)(length, sizeof(buffer) - 1));
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (value == "1" || value == "true" || value == "yes" || value == "on")
            return 1;
        if (value == "0" || value == "false" || value == "no" || value == "off")
            return 0;
        return defaultValue;
    }

    inline DWORD ReadPresentEnvDword(const char* name, DWORD fallback, DWORD minValue, DWORD maxValue)
    {
        char buffer[32]{};
        const DWORD length = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
        if (length == 0)
            return fallback;
        if (length >= sizeof(buffer))
            return fallback;

        char* end = nullptr;
        const unsigned long parsed = std::strtoul(buffer, &end, 10);
        if (end == buffer)
            return fallback;

        return std::clamp<DWORD>(static_cast<DWORD>(parsed), minValue, maxValue);
    }

    inline bool PresentInterpolationEnabled()
    {
        static const bool enabled = ReadPresentEnvFlagState("UN_DMA_PRESENT_INTERP", 0) == 1;
        return enabled;
    }

    inline bool PresentUseForRenderEnabled()
    {
        static const bool enabled =
            PresentInterpolationEnabled() &&
            ReadPresentEnvFlagState("UN_DMA_PRESENT_USE_FOR_RENDER", 1) == 1;
        return enabled;
    }

    inline DWORD PresentEntityHz()
    {
        static const DWORD hz = ReadPresentEnvDword("UN_DMA_ENTITY_PRESENT_HZ", 120, 30, 144);
        return hz;
    }

    inline DWORD PresentEntityDelayMs()
    {
        static const DWORD delayMs = ReadPresentEnvDword("UN_DMA_ENTITY_PRESENT_DELAY_MS", 16, 0, 48);
        return delayMs;
    }

    inline DWORD PresentRenderEntityDelayMs()
    {
        static const DWORD delayMs = ReadPresentEnvDword(
            "UN_DMA_ENTITY_PRESENT_RENDER_DELAY_MS",
            24,
            0,
            64);
        return delayMs;
    }

    inline DWORD PresentMaxExtrapolationMs()
    {
        static const DWORD maxMs = ReadPresentEnvDword("UN_DMA_PRESENT_MAX_EXTRAP_MS", 8, 0, 16);
        return maxMs;
    }

    inline DWORD PresentRenderMicroExtrapolationMs()
    {
        static const DWORD maxMs = ReadPresentEnvDword("UN_DMA_RENDER_PRESENT_MICRO_EXTRAP_MS", 8, 0, 16);
        return maxMs;
    }

    // =========================================================================
    // Color4 = ImVec4 (Config uses ImVec4 directly)

    // =========================================================================
    // Config variables are in Utils/Config.hpp
    // =========================================================================

    // =========================================================================
    // Keyboard / input helpers
    // =========================================================================

    // =========================================================================
    // Sensitivity auto-calibration
    // =========================================================================

    inline float ResolveCalibrationReferenceGameSensitivity(float referenceOverride = 0.0f) {
        if (std::isfinite(referenceOverride) && referenceOverride > 0.0f)
            return referenceOverride;
        if (std::isfinite(Config::gameMouseSensitivity) && Config::gameMouseSensitivity > 0.0f)
            return Config::gameMouseSensitivity;
        return Config::referenceGameSensitivity;
    }

    struct CalibrationOutputCleanup {
        bool enabled;
        ~CalibrationOutputCleanup() {
            if (enabled && V2::Output::ReleaseActiveRuntime() != success)
                Diagnostics::Warn("[CALIBRATE] Output cleanup failed; device output gate remains closed.");
        }
    };

    inline float CalibrateSensitivity(bool calibrateBothAxes = true,
                                      float referenceGameSensitivityOverride = 0.0f,
                                      CalibrationMeasurement* measurement = nullptr) {
        if (measurement)
            *measurement = {};
        CalibrationRunGuard calibrationRun(Config::calibrationInProgress);
        if (!calibrationRun || ShouldSuppressOutput("calibration_move") ||
            !SDK || !ProcessConnection::IsConnected()) {
            Diagnostics::Aim("output.calibration skipped reason=busy_or_output_suppressed");
            return 0.0f;
        }
        CalibrationOutputCleanup cleanup{calibrationRun.IsRoot()};
        if (calibrationRun.IsRoot() && V2::Output::ReleaseActiveRuntime() != success)
            return 0.0f;
        const OutputRuntimeState outputRuntime = CurrentOutputRuntimeState();
        const uint64_t connectionEpoch = ProcessConnection::ConnectionEpoch();
        if (!SDK || connectionEpoch == 0 || !ProcessConnection::IsConnected() ||
            !outputRuntime.outputGateOpen || outputRuntime.backendGeneration == 0 ||
            Config::calibrationMovePixels <= 0 || Config::calibrationStabilityWaitMs < 0)
            return 0.0f;
        const uint64_t controller = SDK->g_player_controller;
        if (!controller)
            return 0.0f;
        const float referenceSensitivity =
            ResolveCalibrationReferenceGameSensitivity(referenceGameSensitivityOverride);
        if (!std::isfinite(referenceSensitivity) || referenceSensitivity <= 0.0f)
            return 0.0f;

        const auto sessionCurrent = [&] {
            const auto current = CurrentOutputRuntimeState();
            return RuntimeWorkersRunning() && ProcessConnection::IsConnected() &&
                ProcessConnection::ConnectionEpoch() == connectionEpoch &&
                SDK && SDK->IsInitialized() && SDK->g_player_controller.load() == controller &&
                current.outputGateOpen &&
                current.backendGeneration == outputRuntime.backendGeneration &&
                !ShouldSuppressOutput("calibration_move");
        };
        const auto readAngle = [&](bool pitch, float& angle) {
            if (!sessionCurrent())
                return false;
            Vector3 direction{};
            if (!SDK->read_range(controller + kPlayerControllerViewDirectionOffset,
                                 &direction, sizeof(direction)))
                return false;
            const double lengthSquared = double(direction.X) * direction.X +
                double(direction.Y) * direction.Y + double(direction.Z) * direction.Z;
            if (!std::isfinite(lengthSquared) || lengthSquared < 0.90 || lengthSquared > 1.10 ||
                !std::isfinite(direction.Y) || std::fabs(direction.Y) > 1.0f)
                return false;
            const Vector3 euler = DirectionToAimEuler(direction);
            angle = pitch ? euler.X : euler.Y;
            return std::isfinite(angle) && sessionCurrent();
        };
        const auto waitStable = [&] {
            return RuntimeWorkerSleep(static_cast<uint32_t>(Config::calibrationStabilityWaitMs)) &&
                sessionCurrent();
        };
        const auto measure = [&](bool pitch) {
            float before = 0.0f, after = 0.0f;
            if (!waitStable() || !readAngle(pitch, before))
                return 0.0f;
            const int counts = Config::calibrationMovePixels;
            if (EnqueueOutputCountMoveForGeneration(pitch ? 0 : counts, pitch ? counts : 0,
                    0, outputRuntime.backendGeneration, connectionEpoch) != success ||
                !waitStable() || !readAngle(pitch, after))
                return 0.0f;
            return CalibrationCountsPerRadian(counts, before, after);
        };
        const float yaw = measure(false);
        const float pitch = calibrateBothAxes && yaw > 0.0f ? measure(true) : 0.0f;
        if (!CalibrationAxesReady(yaw, pitch, calibrateBothAxes) || !sessionCurrent()) {
            Diagnostics::Warn("[CALIBRATE] Incomplete sample or changed session; previous calibration retained.");
            return 0.0f;
        }
        // Commit only a complete measurement from the same device and game session.
        if (measurement)
            *measurement = {yaw, calibrateBothAxes ? pitch : yaw, referenceSensitivity};
        else {
            Config::calibratedCountsPerRadian = yaw;
            Config::calibratedPitchCountsPerRadian = calibrateBothAxes && std::fabs(pitch - yaw) / yaw > 0.05f
                ? pitch : 0.0f;
            Config::referenceGameSensitivity = referenceSensitivity;
        }
        std::printf("[CALIBRATE] Result: yaw=%.1f pitch=%.1f counts/rad refGameSens=%.3f\n",
            yaw, calibrateBothAxes ? pitch : yaw, referenceSensitivity);
        return yaw;
    }

    inline float RunCalibrationSamples(float referenceGameSensitivityOverride = 0.0f) {
        CalibrationRunGuard calibrationRun(Config::calibrationInProgress);
        if (!calibrationRun || Config::calibrationStabilityWaitMs < 0 ||
            ShouldSuppressOutput("calibration_move") || !SDK || !ProcessConnection::IsConnected())
            return 0.0f;
        CalibrationOutputCleanup cleanup{calibrationRun.IsRoot()};
        if (calibrationRun.IsRoot() && V2::Output::ReleaseActiveRuntime() != success)
            return 0.0f;
        const uint64_t connectionEpoch = ProcessConnection::ConnectionEpoch();
        const auto device = CurrentOutputRuntimeState();
        const uint64_t controller = SDK ? SDK->g_player_controller.load() : 0;
        const float referenceSensitivity =
            ResolveCalibrationReferenceGameSensitivity(referenceGameSensitivityOverride);
        const auto batchCurrent = [&] {
            const auto current = CurrentOutputRuntimeState();
            return RuntimeWorkersRunning() && ProcessConnection::IsConnected() &&
                connectionEpoch != 0 && ProcessConnection::ConnectionEpoch() == connectionEpoch &&
                SDK && SDK->IsInitialized() && controller != 0 &&
                SDK->g_player_controller.load() == controller && device.outputGateOpen &&
                current.outputGateOpen && current.backendGeneration == device.backendGeneration &&
                !ShouldSuppressOutput("calibration_move");
        };
        if (!batchCurrent() || !std::isfinite(referenceSensitivity) || referenceSensitivity <= 0.0f)
            return 0.0f;
        double yawTotal = 0.0, pitchTotal = 0.0;
        int validSamples = 0;
        const int numSamples = (std::max)(Config::calibrationSampleCount, 1);
        for (int i = 0; i < numSamples; ++i) {
            if (!batchCurrent())
                return 0.0f;
            CalibrationMeasurement sample;
            std::printf("[CALIBRATE] Sample %d/%d starting...\n", i + 1, numSamples);
            const float result = CalibrateSensitivity(true, referenceSensitivity, &sample);
            if (!batchCurrent())
                return 0.0f;
            if (result > 0.0f && CalibrationAxesReady(sample.yaw, sample.pitch, true)) {
                yawTotal += sample.yaw;
                pitchTotal += sample.pitch;
                ++validSamples;
            }
            if (i < numSamples - 1) {
                const uint64_t pause = uint64_t(Config::calibrationStabilityWaitMs) * 2;
                if (pause > UINT32_MAX || !RuntimeWorkerSleep(static_cast<uint32_t>(pause)) || !batchCurrent())
                    return 0.0f;
            }
        }
        const auto result = CalibrationMean(yawTotal, pitchTotal, validSamples, referenceSensitivity);
        if (!CalibrationAxesReady(result.yaw, result.pitch, true) || !batchCurrent()) {
            Diagnostics::Warn("[CALIBRATE] Batch failed; previous calibration retained.");
            return 0.0f;
        }
        // No intermediate sample changed persistent settings; publish the paired mean once.
        Config::calibratedCountsPerRadian = result.yaw;
        Config::calibratedPitchCountsPerRadian = std::fabs(result.pitch - result.yaw) / result.yaw > 0.05f
            ? result.pitch : 0.0f;
        Config::referenceGameSensitivity = result.referenceSensitivity;
        std::printf("[CALIBRATE] Averaged %d valid paired samples: yaw=%.1f pitch=%.1f counts/rad refGameSens=%.3f\n",
            validSamples, result.yaw, result.pitch, result.referenceSensitivity);
        return result.yaw;
    }

    inline int get_bind_id(int setting) {
        return Labels::AimActivationKeyVk(setting);
    }

    inline unsigned int convertToHex(const ImVec4& color) {
        unsigned int red   = static_cast<unsigned int>(color.x * 255.f);
        unsigned int green = static_cast<unsigned int>(color.y * 255.f);
        unsigned int blue  = static_cast<unsigned int>(color.z * 255.f);
        unsigned int alpha = static_cast<unsigned int>(color.w * 255.f);
        return (alpha << 24) | (blue << 16) | (green << 8) | red;
    }

    // =========================================================================
    // Simple math helpers
    // =========================================================================

    inline std::pair<double, double> dd(double a, double b, double c, double d, double m, double n) {
        return std::pair<double, double>(
            (m * d - c * n) / (a * d - c * b),
            (a * n - b * m) / (a * d - c * b)
        );
    }

    // =========================================================================
    // Aim smoothing
    // =========================================================================

    // =========================================================================
    // Target selection and shared projectile prediction
    // =========================================================================

    namespace TargetingDetail {

        constexpr float kNoTargetScore = 100000.f;
        inline std::atomic<size_t> published_entity_count{0};

        struct SelectionResult {
            int index = -1;
            Vector3 target{};
            c_entity entity{};
            float score = kNoTargetScore;
        };

        struct VelocitySample {
            Vector3 velocity{}, acceleration{};
            uint64_t tick = 0;
            bool initialized = false;
        };

        inline std::mutex velocity_history_mutex;
        inline std::unordered_map<uint64_t, VelocitySample> velocity_history;

        struct TargetLockRuntime {
            uint64_t entityKey = 0;
            uint64_t actorGeneration = 0;
            int entityIndex = -1;
            int boneId = Plexies20260609::kUnusedRenderSkeletonBone;
            DWORD boneStartedTick = 0;
            DWORD lockStartedTick = 0;
            DWORD lastSeenTick = 0;
            float lastScore = kNoTargetScore;
            Vector3 lastVelocity{};
            bool hasLastVelocity = false;
            DWORD reversalStartedTick = 0;
            DWORD reversalLatchedUntilTick = 0;
            Vector3 reversalReferenceVelocity{};
            bool active = false;
        };

        inline std::mutex target_lock_mutex;
        inline TargetLockRuntime target_lock_runtime;

        inline bool IsZeroVector(const Vector3& value) {
            return value == Vector3(0, 0, 0);
        }

        inline Vector2 CrosshairCenter() {
            float width = static_cast<float>(GetSystemMetrics(SM_CXSCREEN));
            float height = static_cast<float>(GetSystemMetrics(SM_CYSCREEN));
            if (ScreenWidth() > 0.0f && ScreenHeight() > 0.0f) {
                width = ScreenWidth();
                height = ScreenHeight();
            } else if (Config::manualScreenWidth > 0 && Config::manualScreenHeight > 0) {
                width = static_cast<float>(Config::manualScreenWidth);
                height = static_cast<float>(Config::manualScreenHeight);
            }
            return Vector2(
                width * 0.5f,
                height * 0.5f
            );
        }

        inline Vector3 CameraPosition() {
            return OW::SnapshotCameraPosition();
        }

        inline Vector3 SnapshotLaunchPoint() {
            const Vector3 sampledPoint = CameraPosition();
            if (!FovGeometry::IsFiniteVector(sampledPoint))
                return sampledPoint;
            uint64_t sampledAtMs = 0;
            if (consumerFrame)
                sampledAtMs = consumerFrame->cameraSample.lastSuccessfulSampleMs;
            else {
                std::lock_guard<std::mutex> lock(g_viewMatrixMutex);
                sampledAtMs = viewMatrixSampleState.lastSuccessfulSampleMs;
            }
            const uint64_t nowMs = GetTickCount64();
            if (!sampledAtMs || nowMs < sampledAtMs)
                return sampledPoint;
            const c_entity local = OW::SnapshotLocalEntity();
            return ExtrapolateLaunchPoint(sampledPoint, local.velocity,
                static_cast<float>(nowMs - sampledAtMs));
        }

        using FovRuntimeContext = FovGeometry::RuntimeContext;

        inline bool IsFiniteVector(const Vector3& value) {
            return FovGeometry::IsFiniteVector(value);
        }

        inline Vector3 NormalizeVector(const Vector3& value) {
            return FovGeometry::NormalizeVector(value);
        }

        inline FovRuntimeContext SnapshotFovRuntimeContext() {
            Matrix view{}, viewXor{};
            SnapshotViewMatrices(view, viewXor);

            const auto camera = viewXor.get_location();
            const auto forward = viewXor.get_rotation();

            FovRuntimeContext context{};
            context.camera = Vector3(camera.x, camera.y, camera.z);
            context.forward = NormalizeVector(Vector3(forward.x, forward.y, forward.z));
            context.valid = IsFiniteVector(context.camera) &&
                            !IsZeroVector(context.camera) &&
                            !IsZeroVector(context.forward);
            return context;
        }

        inline float FovAngleLimitDeg(float fovDeg) {
            return Config::ClampFovDeg(fovDeg);
        }

        inline float AngularSeparationDegFromCamera(const Vector3& camera,
                                                    const Vector3& a,
                                                    const Vector3& b) {
            return FovGeometry::AngularSeparationDegFromCamera(camera, a, b);
        }

        inline float FovScoreDeg(const FovRuntimeContext& context, const Vector3& position) {
            return FovGeometry::FovScoreDeg(context, position);
        }

        inline bool IsWithinFovDeg(const FovRuntimeContext& context,
                                   const Vector3& position,
                                   float fovDeg,
                                   float* outScoreDeg = nullptr) {
            return FovGeometry::IsWithinFovDeg(
                context,
                position,
                FovAngleLimitDeg(fovDeg),
                outScoreDeg);
        }

        using CandidateFovEvaluation = FovGeometry::CandidateFovEvaluation;

        inline bool EvaluateCandidateFov(const FovRuntimeContext& context,
                                         const Vector3& rawAimPoint,
                                         const Vector3& finalAimPoint,
                                         float fixedFovDeg,
                                         FovGeometry::ResolveFovForDistanceFn resolveFov,
                                         CandidateFovEvaluation& outEvaluation) {
            return FovGeometry::EvaluateCandidateFov(
                context,
                rawAimPoint,
                finalAimPoint,
                fixedFovDeg,
                resolveFov,
                outEvaluation);
        }

        inline bool IsTrainingBot(uint64_t heroId) {
            return GameData::IsTrainingBotHeroId(heroId);
        }

        inline bool IsSpecialAimEntity(uint64_t heroId) {
            return heroId == 0x16dd || heroId == 0x16ee;
        }

        inline void SetPublishedEntityCount(size_t count) {
            published_entity_count.store(count, std::memory_order_release);
            Diagnostics::RecordEntityPublish(count);
        }

        inline size_t PublishedEntityCount() {
            return published_entity_count.load(std::memory_order_acquire);
        }

        inline bool HasPublishedEntities() {
            return PublishedEntityCount() > 0;
        }

        template<class Tick = decltype(&::GetTickCount)>
        inline bool TryPublishEntitySnapshots(
            std::vector<c_entity>&& nextEntities,
            std::vector<hpanddy>&& nextDynamicEntities,
            uint64_t expectedConnectionEpoch,
            size_t& publishedCount,
            const EntityRegistry::Snapshot* registry=nullptr,
            uint64_t expectedScene=0, Tick tick = &::GetTickCount)
        {
            const size_t count = nextEntities.size();
            {
                std::lock_guard<std::mutex> lock(::g_mutex);
                if (expectedScene && expectedScene != EntitySceneGeneration()) {
                    ++rejectedEntitySceneResults; return false;
                }
                std::unique_lock<std::mutex> registryLock(entityRegistryMonitor.mutex);
                if(registry && registry->generation) {
                    // Invalidation must publish even when the source is unreadable.
                    if(!entityRegistryMonitor.state.Accept(*registry) &&
                        (!nextDynamicEntities.empty() || std::any_of(nextEntities.begin(),nextEntities.end(),
                            [](const auto& value){return value.body_valid;}))) return false;
                }
                if (expectedConnectionEpoch != 0 &&
                    (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected() ||
                     ProcessConnection::ConnectionEpoch() != expectedConnectionEpoch)) {
                    return false;
                }
                std::lock_guard<std::mutex> bodyLock(entityBodyValidityMutex);
                if(expectedScene && expectedScene!=EntitySceneGeneration())return false;
                const auto previousFrame=SnapshotFrame();
                const auto& previousEntities=previousFrame->entities;
                for(const auto& value:nextEntities) {
                    const auto current=std::find_if(previousEntities.begin(),previousEntities.end(),[&](const auto& old) {
                        return old.address==value.address && old.connection_epoch==value.connection_epoch && old.scene_generation==value.scene_generation;
                    });
                    if(current!=previousEntities.end() && int32_t(value.anchor_sample_tick_ms-current->anchor_sample_tick_ms)<0)return false;
                    const auto authority=entityBodyAuthorities.find(value.address);
                    if(value.body_valid && (authority==entityBodyAuthorities.end() || !authority->second.alive))return false;
                    // Revoked seats may publish unavailable metadata; they cannot displace an admitted successor.
                    const bool invalidation=!value.body_valid && (authority==entityBodyAuthorities.end() ||
                        (value.body_authority_revision && !authority->second.admitted && authority->second.generation==value.actor_generation &&
                        authority->second.epoch==value.connection_epoch && authority->second.scene==value.scene_generation));
                    if((value.body_authority_revision || value.body_valid || authority!=entityBodyAuthorities.end()) && !invalidation &&
                        !EntityBodyAuthorityMatchesLocked({value.address,value.LinkParent,value.HeroID,value.match_id,value.last_discovered_tick_ms},
                            value.actor_generation,value.body_authority_revision,value.connection_epoch,value.scene_generation,value.body_absence_revision))return false;
                    if(authority!=entityBodyAuthorities.end() && registry && authority->second.registry!=registry->generation)return false;
                }
                {
                    activeEntityBodies.clear();
                    for(auto& value:nextEntities) {
                        if(value.body_valid && value.roster_state==EntityRosterState::Fresh &&
                            value.Alive && value.HasFreshLifeState(tick()) && value.scene_generation==EntitySceneGeneration() &&
                            PresentationTiming::Fresh(value.anchor_sample_tick_ms,tick())) {
                            value.body_valid=TryActivateEntityBodyLocked({value.address,value.LinkParent,value.HeroID,value.match_id,
                                value.last_discovered_tick_ms},value.actor_generation,value.scene_generation,tick(),value.body_absence_revision,
                                value.body_authority_revision,value.connection_epoch);
                        } else value.body_valid=false;
                        if(!value.body_valid && value.roster_state==EntityRosterState::Fresh) {
                            value.roster_state=EntityRosterState::Missing;value.Alive=false;value.Vis=false;
                        }
                    }
                }
                auto frame=std::make_shared<ConsumptionFrame>();
                frame->sequence=previousFrame->sequence+1;
                uint64_t sampleUs=0;
                for(const auto& entity:nextEntities)sampleUs=(std::max)(sampleUs,entity.sample_time_us);
                frame->sampledAt=sampleUs ? std::chrono::steady_clock::time_point(std::chrono::microseconds(sampleUs)) : std::chrono::steady_clock::now();
                frame->local=local_entity;
                // Only verified local observations populate local_entity. Its confirmed affiliation
                // belongs to this scene, not to the current body's life/death authority revision.
                const bool localTeamKnown=local_entity.team_known && local_entity.address && local_entity.actor_generation &&
                    local_entity.connection_epoch==ProcessConnection::ConnectionEpoch() &&
                    local_entity.scene_generation==EntitySceneGeneration();
                frame->local.team_known=localTeamKnown;
                for(auto& value:nextEntities) {
                    value.team_known=value.team_known && localTeamKnown;
                    value.Team=value.team_known && value.address!=local_entity.address &&
                        (value.team_key==(1u<<0x1B) || value.team_key!=local_entity.team_key);
                    if(GameData::IsFriendlyTrainingBotHeroId(value.HeroID))value.Team=false;
                }
                frame->entities=std::move(nextEntities);frame->dynamic=std::move(nextDynamicEntities);
                { std::lock_guard<std::mutex> cameraLock(g_viewMatrixMutex);
                    frame->view=viewMatrix;frame->camera=viewMatrix_xor;frame->cameraSample=viewMatrixSampleState; }
                PublishFrame(std::move(frame));
#ifdef UNLEASHED_SELF_TESTS
                entities=SnapshotFrame()->entities;hp_dy_entities=SnapshotFrame()->dynamic;
#endif
                SetPublishedEntityCount(count);
                Diagnostics::SetEntityCount(count);
            }

            publishedCount = count;
            return true;
        }

        inline size_t PublishEntitySnapshots(std::vector<c_entity>&& nextEntities,
                                             std::vector<hpanddy>&& nextDynamicEntities) {
            size_t publishedCount = 0;
            TryPublishEntitySnapshots(
                std::move(nextEntities),
                std::move(nextDynamicEntities),
                0,
                publishedCount);
            return publishedCount;
        }

        inline std::vector<c_entity> SnapshotEntities() {
            const auto frame=consumerFrame?consumerFrame:SnapshotFrame();
            return frame->entities;
        }

        inline std::vector<c_entity> SnapshotPresentEntities() {
            std::vector<c_entity> snapshot;
            if (!OW::PresentInterpolationEnabled())
                return snapshot;

            std::lock_guard<std::mutex> lock(OW::g_presentEntityMutex);
            snapshot = OW::present_entities;
            return snapshot;
        }

        inline std::vector<c_entity> SnapshotPresentRenderEntities() {
            std::vector<c_entity> snapshot;
            if (!OW::PresentUseForRenderEnabled())
                return snapshot;

            std::lock_guard<std::mutex> lock(OW::g_presentRenderEntityMutex);
            snapshot = OW::present_render_entities;
            return snapshot;
        }

        inline std::vector<hpanddy> SnapshotDynamicEntities() {
            const auto frame=consumerFrame?consumerFrame:SnapshotFrame();
            return frame->dynamic;
        }

        inline std::vector<hpanddy> SnapshotPresentDynamicEntities() {
            std::vector<hpanddy> snapshot;
            if (!OW::PresentInterpolationEnabled())
                return snapshot;

            std::lock_guard<std::mutex> lock(OW::g_presentEntityMutex);
            snapshot = OW::present_hp_dy_entities;
            return snapshot;
        }

        inline std::vector<hpanddy> SnapshotPresentRenderDynamicEntities() {
            std::vector<hpanddy> snapshot;
            if (!OW::PresentUseForRenderEnabled())
                return snapshot;

            std::lock_guard<std::mutex> lock(OW::g_presentRenderEntityMutex);
            snapshot = OW::present_render_hp_dy_entities;
            return snapshot;
        }

        inline c_entity SnapshotLocalEntity() {
            return OW::SnapshotLocalEntity();
        }

        inline bool IsValidIndex(int index, size_t size) {
            return index >= 0 && static_cast<size_t>(index) < size;
        }

        inline Contour::Identity ContourIdentity(const c_entity& entity) {
            return {entity.roster_key ? entity.roster_key : entity.address,
                entity.actor_generation,entity.scene_generation,entity.connection_epoch};
        }
        inline thread_local std::vector<std::pair<Contour::Identity,bool>> contourVisibility;
        inline thread_local Contour::Observation lastContourObservation;
        inline bool SelectionVisible(const c_entity& entity) {
            // Preserve the native UN visibility result. Contour is only a
            // fallback for candidates rejected by the native visibility test.
            if (entity.Vis) return true;
            if(!Config::View_aimbotContourEnabled()) return entity.Vis;
            const auto identity=ContourIdentity(entity);
            for(const auto& entry:contourVisibility) if(entry.first==identity) return entry.second;
            return false;
        }
        inline void ResetContourSelection() { contourVisibility.clear();lastContourObservation={}; }

        inline bool IsRuntimeTargetValidStage0(const c_entity& entity) {
            if (!entity.team_known) return false;
            if (!entity.HasUsableBody(GetTickCount(),EntitySceneGeneration())) return false;
            if (entity.roster_state != EntityRosterState::Fresh) return false;
            if (!PresentationTiming::Fresh(entity.anchor_sample_tick_ms, GetTickCount())) return false;
            if (!entity.address || !entity.Alive) return false;
            if(Config::targetAimPlanEnabled) {
                const auto& plan=Config::View_aimPlan();
                if(!AimPlan::EligiblePreGeometryStage0(plan.selection,AimPlan::Resolve(plan,entity.HeroID),
                    entity.HeroID,entity.PlayerHealth,entity.PlayerHealthMax)) return false;
            }
            return true;
        }

        inline bool IsRuntimeTargetValid(const c_entity& entity, bool requireVisible = true) {
            if (!entity.team_known) return false;
            if (!entity.HasUsableBody(GetTickCount(),EntitySceneGeneration())) return false;
            if (entity.roster_state != EntityRosterState::Fresh) return false;
            if (!PresentationTiming::Fresh(entity.anchor_sample_tick_ms, GetTickCount())) return false;
            if (!entity.address || !entity.Alive) return false;
            if (requireVisible && !SelectionVisible(entity)) return false;
            if(Config::targetAimPlanEnabled) {
                const auto& plan=Config::View_aimPlan();
                if(!AimPlan::Eligible(plan.selection,AimPlan::Resolve(plan,entity.HeroID),
                    entity.HeroID,entity.PlayerHealth,entity.PlayerHealthMax,SelectionVisible(entity))) return false;
            }
            return true;
        }

        // Protection Filter is a slot-owned hard gate.  Keep the freshness
        // boundary in one helper so target selection and behavior consumers do
        // not accidentally reject protected targets when the filter is off.
        inline bool ProtectionFilterPasses(const c_entity& entity,
                                           bool requireTargetable) {
            bool staleHealth = false;
            const bool allowed = TargetPipeline::ProtectionFilterPasses(
                entity.Team,
                requireTargetable,
                entity.imort,
                entity.barrprot,
                entity.health_sample_tick_ms,
                GetTickCount(),
                &staleHealth);
            if (staleHealth) {
                Diagnostics::Aim("target.protection rejected reason=stale_health_sample");
            }
            return allowed;
        }

        inline bool ProtectionFilterPasses(const c_entity& entity) {
            return ProtectionFilterPasses(
                entity,
                Config::slotTargetRules.active &&
                    Config::slotTargetRules.requireTargetable);
        }

        inline bool TryEntityAtStage0(int index, c_entity& entity) {
            const auto snapshot = SnapshotEntities();
            if (!IsValidIndex(index, snapshot.size())) return false;
            entity = snapshot[static_cast<size_t>(index)];
            return IsRuntimeTargetValidStage0(entity);
        }

        inline bool TryEntityAt(int index, c_entity& entity, bool requireVisible = false) {
            const auto snapshot = SnapshotEntities();
            if (!IsValidIndex(index, snapshot.size())) return false;
            entity = snapshot[static_cast<size_t>(index)];
            return IsRuntimeTargetValid(entity, requireVisible);
        }

        inline bool TargetTeamMatches(const c_entity& entity,
                                      int teamMode,
                                      const c_entity& local,
                                      const Config::SlotTargetRules* rulesOverride = nullptr) {
            if (!entity.team_known || !local.team_known) return false;
            if (entity.address == local.address)
                return false;

            const auto& rules = rulesOverride ? *rulesOverride : Config::slotTargetRules;
            if(rules.active) {
                if (rules.maxTargetSpeedMps > 0.0f) {
                    const Vector3 velocity = entity.velocity;
                    const float speed = std::sqrt(
                        velocity.X * velocity.X +
                        velocity.Y * velocity.Y +
                        velocity.Z * velocity.Z);
                    if (std::isfinite(speed) && speed > rules.maxTargetSpeedMps)
                        return false;
                }
                if (rules.requireTargetable) {
                    if (!ProtectionFilterPasses(entity, true))
                        return false;
                }
                if(!entity.Team) {
                    if(!std::isfinite(entity.PlayerHealthMax)||entity.PlayerHealthMax<=0 ||
                       entity.PlayerHealth/entity.PlayerHealthMax*100.f>rules.allyHealthMaxPercent) return false;
                } else {
                    if(rules.ignoreSombraStealth && entity.HeroID==eHero::HERO_SOMBRA &&
                       (!entity.sombraStealthKnown || entity.sombraStealth))return false;
                }
            }
            switch (teamMode) {
            case 0:  return entity.Team;  // Enemies
            case 1:  return !entity.Team; // Allies
            case 2:  return true;         // All
            default: return entity.Team;
            }
        }

        inline bool IsSelectableCandidate(const c_entity& entity, int teamMode, const c_entity& local) {
            return IsRuntimeTargetValid(entity, false) && TargetTeamMatches(entity, teamMode, local);
        }

        inline Vector3 SemanticSkeletonRawPosition(const c_entity& entity, SkeletonSemantics::Slot semantic) {
            const size_t slot = SkeletonSemantics::SlotIndex(semantic);
            Vector3 point{};
            if (!entity.TrySkeletonPoint(slot, point))
                return {};
            return point != entity.pos ? point : Vector3{};
        }

        inline Vector3 CachedSkeletonBoneRawPosition(const c_entity& entity,int boneId) {
            const auto ids=entity.GetRenderSkel();
            for(size_t slot=0;slot<ids.size();++slot) {
                if(ids[slot] != boneId)
                    continue;
                const auto point = SemanticSkeletonRawPosition(entity, static_cast<SkeletonSemantics::Slot>(slot));
                if (!IsZeroVector(point))
                    return point;
            }
            return {};
        }

        inline Vector3 CoreBoneRawPosition(const c_entity& entity, int boneId) {
            const size_t slot = boneId == BONE_HEAD ? 0 : boneId == BONE_NECK ? 1 : 2;
            const auto source = entity.core_anchor_source[slot];
            if (source == SkeletonSemantics::CoreAnchorSource::Unavailable)
                return {};
            if (source == SkeletonSemantics::CoreAnchorSource::ObservedBone) {
                const bool observedBotChest = slot == 2 && GameData::IsTrainingBotHeroId(entity.HeroID) &&
                    entity.rotation_valid && entity.skeleton_form != SkeletonSemantics::Form::Unknown && entity.skeleton_revision != 0 &&
                    entity.cached_bot_chest_bone_valid && IsFiniteVector(entity.cached_bot_chest_bone) &&
                    !IsZeroVector(entity.cached_bot_chest_bone);
                if (!observedBotChest &&
                    IsZeroVector(SemanticSkeletonRawPosition(entity, static_cast<SkeletonSemantics::Slot>(slot))))
                    return {};
            }
            const Vector3 point = slot == 0 ? entity.head_pos : slot == 1 ? entity.neck_pos : entity.chest_pos;
            return IsFiniteVector(point) && !IsZeroVector(point) && point != entity.pos ? point : Vector3{};
        }

        inline Vector3 CoreBoneHitboxCenter(const c_entity& entity, int boneId) {
            Vector3 nativeCenter{};
            if (TryResolveNativeHitboxCenter(entity, boneId, nativeCenter))
                return nativeCenter;
            const Vector3 point = CoreBoneRawPosition(entity, boneId);
            return IsZeroVector(point) || !entity.rotation_valid ? point : ResolveBoneHitboxCenter(entity, boneId, point);
        }

        inline bool TryResolveSkeletonBonePoint(const c_entity& entity,
                                                std::size_t renderSlot,
                                                int& outBoneId,
                                                Vector3& outPoint,
                                                SkeletonSemantics::CoreAnchorSource* outSource = nullptr) {
            if (outSource)
                *outSource = SkeletonSemantics::CoreAnchorSource::Unavailable;
            if (renderSlot >= entity.skeleton_bones.size() ||
                renderSlot >= entity.skeleton_bone_valid.size()) {
                return false;
            }

            const int boneId = Plexies20260609::HitboxBoneIdForRenderSlot(renderSlot);
            if (boneId == Plexies20260609::kUnusedRenderSkeletonBone)
                return false;

            Vector3 hitboxCenter{};
            if (TryResolveNativeHitboxCenter(entity, boneId, hitboxCenter)) {
                outBoneId = boneId;
                outPoint = hitboxCenter;
                if (outSource)
                    *outSource = SkeletonSemantics::CoreAnchorSource::ObservedBone;
                return true;
            }

            Vector3 rawPoint{};
            SkeletonSemantics::CoreAnchorSource source = SkeletonSemantics::CoreAnchorSource::ObservedBone;
            if (renderSlot < 3) {
                rawPoint = CoreBoneRawPosition(entity, boneId);
                source = entity.core_anchor_source[renderSlot];
            } else {
                rawPoint = SemanticSkeletonRawPosition(entity, static_cast<SkeletonSemantics::Slot>(renderSlot));
            }

            if (IsZeroVector(rawPoint) || rawPoint == entity.pos || !IsFiniteVector(rawPoint))
                return false;

            hitboxCenter = entity.rotation_valid
                ? ResolveBoneHitboxCenter(entity, boneId, rawPoint) : rawPoint;
            if (IsZeroVector(hitboxCenter) || !IsFiniteVector(hitboxCenter))
                return false;

            outBoneId = boneId;
            outPoint = hitboxCenter;
            if (outSource)
                *outSource = source;
            return true;
        }

        inline bool TrySelectClosestSelectedSkeletonPoint(
            const c_entity& entity,
            SkeletonBoneMask rawMask,
            const FovRuntimeContext& fovContext,
            const Matrix& view,
            const Vector2& crosshair,
            int& outBoneId,
            Vector3& outPoint,
            int preferredBoneId = Plexies20260609::kUnusedRenderSkeletonBone,
            int selectionMode = 2,
            float* outScore = nullptr,
            int retainedBoneId = Plexies20260609::kUnusedRenderSkeletonBone) {
            const SkeletonBoneMask mask = NormalizeSkeletonBoneMask(rawMask);
            bool found = false;
            float bestScore = (std::numeric_limits<float>::max)();

            for (std::size_t renderSlot = 0;
                 renderSlot < kSkeletonBoneGroupForRenderSlot.size();
                 ++renderSlot) {
                if (!SkeletonBoneMaskIncludesRenderSlot(mask, renderSlot))
                    continue;

                int boneId = Plexies20260609::kUnusedRenderSkeletonBone;
                Vector3 point{};
                if (!TryResolveSkeletonBonePoint(entity, renderSlot, boneId, point))
                    continue;

                float score = fovContext.valid
                    ? TargetingDetail::FovScoreDeg(fovContext, point)
                    : crosshair.Distance(view.WorldToScreen(point));
                if (!std::isfinite(score))
                    continue;

                if (selectionMode == 1 && boneId == preferredBoneId)
                    score *= 0.35f;
                if (boneId == retainedBoneId)
                    score *= 0.82f;
                if (!found || score < bestScore) {
                    found = true;
                    bestScore = score;
                    outBoneId = boneId;
                    outPoint = point;
                }
            }

            if (found && outScore)
                *outScore = bestScore;
            return found;
        }

        inline bool ComputeAngularErrorDeg(const FovRuntimeContext& fovContext,
                                           const Vector3& targetPoint,
                                           float& outYawDeg,
                                           float& outPitchDeg) {
            outYawDeg = 0.0f;
            outPitchDeg = 0.0f;
            if (!fovContext.valid || !IsFiniteVector(targetPoint))
                return false;
            const Vector3 unitDelta = NormalizeVector(targetPoint - fovContext.camera);
            const Vector3 unitForward = NormalizeVector(fovContext.forward);
            if (IsZeroVector(unitDelta) || IsZeroVector(unitForward))
                return false;
            const Vector3 camEuler = DirectionToAimEuler(unitForward);
            const Vector3 tgtEuler = DirectionToAimEuler(unitDelta);
            outYawDeg = WrapDegrees180(RAD2DEG(tgtEuler.Y - camEuler.Y));
            outPitchDeg = RAD2DEG(tgtEuler.X - camEuler.X);
            return std::isfinite(outYawDeg) && std::isfinite(outPitchDeg);
        }

        // Prefers explicit native CriticalState::Critical shapes for HeadOnly/HeadThenBody (§7.2).
        // Never fabricates critical from CriticalState::Unknown.
        inline bool TryResolveExplicitCriticalHitboxPoint(const c_entity& entity,
                                                          const FovRuntimeContext& fovContext,
                                                          const Matrix& view,
                                                          const Vector2& crosshair,
                                                          int& outBoneId,
                                                          Vector3& outPoint) {
            if (!NativeHitboxSampleUsable(entity, GetTickCount()))
                return false;
            const auto& sample = entity.native_hitboxes;
            const float worldScale = (std::min)({
                sample.world.record.scale[0],
                sample.world.record.scale[1],
                sample.world.record.scale[2]});
            const Vector3 shapeShift = entity.pos - sample.world.sampleAnchor;
            if (!std::isfinite(worldScale) || worldScale <= 0.0f || !IsFiniteVector(shapeShift))
                return false;

            bool found = false;
            float bestScore = (std::numeric_limits<float>::max)();
            for (size_t i = 0; i < sample.shapes.size() && i < sample.poses.size(); ++i) {
                const auto& source = sample.shapes[i];
                const auto& pose = sample.poses[i];
                if (source.critical != ModelShapes::CriticalState::Critical ||
                    !pose.Valid() || !std::isfinite(source.radius) || source.radius <= 0.0f) {
                    continue;
                }
                const Vector3 a = sample.world.TransformPoint(ModelShapes::Transform(source.a, pose)) + shapeShift;
                const Vector3 b = sample.world.TransformPoint(ModelShapes::Transform(source.b, pose)) + shapeShift;
                const Vector3 center = (a + b) * 0.5f;
                if (!IsFiniteVector(center) || IsZeroVector(center))
                    continue;
                const float score = fovContext.valid
                    ? TargetingDetail::FovScoreDeg(fovContext, center)
                    : crosshair.Distance(view.WorldToScreen(center));
                if (!std::isfinite(score))
                    continue;
                if (!found || score < bestScore) {
                    found = true;
                    bestScore = score;
                    outBoneId = source.boneId >= 0 ? source.boneId : BONE_HEAD;
                    outPoint = center;
                }
            }
            return found;
        }

        // Resolves the best point belonging to a single SkeletonBoneMask group bit.
        inline bool TryResolveBoneGroupPoint(const c_entity& entity,
                                             SkeletonBoneMask groupMask,
                                             bool preferExplicitCritical,
                                             const FovRuntimeContext& fovContext,
                                             const Matrix& view,
                                             const Vector2& crosshair,
                                             int& outBoneId,
                                             Vector3& outPoint,
                                             int retainedBoneId = Plexies20260609::kUnusedRenderSkeletonBone,
                                             bool* outUsedExplicitCritical = nullptr) {
            if (outUsedExplicitCritical)
                *outUsedExplicitCritical = false;
            if (preferExplicitCritical && groupMask == kSkeletonBoneHead) {
                if (TryResolveExplicitCriticalHitboxPoint(
                        entity, fovContext, view, crosshair, outBoneId, outPoint)) {
                    if (outUsedExplicitCritical)
                        *outUsedExplicitCritical = true;
                    return true;
                }
            }
            bool found = false;
            float bestScore = (std::numeric_limits<float>::max)();
            for (std::size_t renderSlot = 0;
                 renderSlot < kSkeletonBoneGroupForRenderSlot.size();
                 ++renderSlot) {
                if ((kSkeletonBoneGroupForRenderSlot[renderSlot] & groupMask) == 0)
                    continue;
                int boneId = Plexies20260609::kUnusedRenderSkeletonBone;
                Vector3 point{};
                if (!TryResolveSkeletonBonePoint(entity, renderSlot, boneId, point))
                    continue;
                float score = fovContext.valid
                    ? TargetingDetail::FovScoreDeg(fovContext, point)
                    : crosshair.Distance(view.WorldToScreen(point));
                if (!std::isfinite(score))
                    continue;
                if (boneId == retainedBoneId)
                    score *= 0.85f;
                if (!found || score < bestScore) {
                    found = true;
                    bestScore = score;
                    outBoneId = boneId;
                    outPoint = point;
                }
            }
            return found;
        }

        struct ContourProjectionCache {
            std::uint64_t gameSequence=0,sequence=0,epoch=0,revision=0,scene=0,connection=0;
            Matrix view{};
            std::vector<Contour::ProjectedActor> actors;
            std::map<std::pair<std::size_t,std::uint16_t>,Contour::Observation> observations;
        };
        inline thread_local ContourProjectionCache contourProjectionCache;
        inline thread_local std::optional<V2::Vision::Settings> lastContourSettings;

        struct ContourEvaluation {
            bool enabled=false;
            const ConsumptionFrame& game;
            V2::Vision::Settings settings{};
            std::shared_ptr<const V2::Vision::Frame> image;
            std::vector<Contour::ProjectedActor> actors;
            double now=0;
            explicit ContourEvaluation(const ConsumptionFrame& frame,bool active) : enabled(active),game(frame) {
                if(!enabled) return;
                image=V2::Vision::GetService().Snapshot(settings);now=V2::Vision::MonotonicMs();
                if(!lastContourSettings || *lastContourSettings!=settings) {
                    lastContourObservation={};contourProjectionCache={};lastContourSettings=settings;
                }
                contourVisibility.clear();
                if(image && (image->epoch!=lastContourObservation.evidence.epoch ||
                    image->settingsRevision!=lastContourObservation.evidence.revision)) lastContourObservation={};
                // Both clocks are local steady-clock receipt/publication times.
                // This bounds local skew; it does not measure HDMI latency.
                const double sampled=std::chrono::duration<double,std::milli>(frame.sampledAt.time_since_epoch()).count();
                if(image && (!std::isfinite(sampled)||std::fabs(sampled-image->arrivalMs)>settings.maxFrameAgeMs)) image.reset();
                auto& cache=contourProjectionCache;
                const bool cached=image && game.sequence && cache.gameSequence==game.sequence &&
                    cache.sequence==image->sequence && cache.epoch==image->epoch && cache.revision==image->settingsRevision &&
                    cache.scene==game.local.scene_generation && cache.connection==game.local.connection_epoch &&
                    std::memcmp(&cache.view,&game.view,sizeof(Matrix))==0;
                if(cached) actors=cache.actors;
                else {
                cache={};
                actors.resize(game.entities.size());
                for(std::size_t i=0;i<game.entities.size();++i) {
                    const auto& entity=game.entities[i];auto& actor=actors[i];actor.identity=ContourIdentity(entity);
                    if(!image || !entity.team_known || !entity.Alive || !entity.address ||
                        entity.roster_state!=EntityRosterState::Fresh ||
                        !entity.HasUsableBody(GetTickCount(),EntitySceneGeneration()) ||
                        !PresentationTiming::Fresh(entity.anchor_sample_tick_ms,GetTickCount())) continue;
                    Vector2 rootScreen{};
                    if(entity.pos!=Vector3{} && Contour::Project(game.view,entity.pos,image->width,image->height,rootScreen))
                        actor.root=rootScreen;
                    for(std::size_t slot=0;slot<kSkeletonBoneGroupForRenderSlot.size();++slot) {
                        int id=0;Vector3 world{};Vector2 screen{};
                        if(TryResolveSkeletonBonePoint(entity,slot,id,world) &&
                            Contour::Project(game.view,world,image->width,image->height,screen)) {
                            const float radius=ResolveBoneHitboxRadius(entity.HeroID,id,0.0f);
                            float supportRadius=0;
                            if(radius>0) for(const auto& delta:std::array<Vector3,6>{Vector3(radius,0,0),Vector3(-radius,0,0),
                                Vector3(0,radius,0),Vector3(0,-radius,0),Vector3(0,0,radius),Vector3(0,0,-radius)}) {
                                Vector2 extent{};
                                if(Contour::Project(game.view,world+delta,image->width,image->height,extent))
                                    supportRadius=(std::max)(supportRadius,screen.Distance(extent));
                            }
                            if(supportRadius>0) supportRadius=(std::min)(settings.associationRadiusPx,supportRadius*1.5f+2.f);
                            actor.bones.push_back({id,slot,world,screen,supportRadius});
                        }
                    }
                }
                if(image) {
                    cache.gameSequence=game.sequence;cache.sequence=image->sequence;cache.epoch=image->epoch;
                    cache.revision=image->settingsRevision;cache.scene=game.local.scene_generation;
                    cache.connection=game.local.connection_epoch;cache.view=game.view;cache.actors=actors;
                }
                }
                for(std::size_t i=0;i<actors.size();++i) {
                    const auto observation=Resolve(i,0xffffu);
                    // Retention preserves identity for diagnostics only. It is
                    // not a current screen observation and cannot make a
                    // hidden actor selectable.
                    contourVisibility.push_back({actors[i].identity,
                        observation.evidence.exposed && observation.evidence.fresh});
                }
            }
            Contour::Observation Resolve(std::size_t index,std::uint16_t slots) const {
                Contour::Observation observation{};
                if(!enabled || index>=actors.size()) return observation;
                observation.evidence.identity=actors[index].identity;
                if(image) {
                    auto& cache=contourProjectionCache.observations;
                    const auto key=std::make_pair(index,slots);
                    const auto found=cache.find(key);
                    if(found!=cache.end()) observation=found->second;
                    else {
                        observation=Contour::Associate(*image,settings,game.view,actors,index,slots,now);
                        cache.emplace(key,observation);
                    }
                }
                if(observation.evidence.exposed) return observation;
                const auto& prior=lastContourObservation;
                const double continuityMs=image ? image->arrivalMs : now;
                bool priorBoneAllowed=false;
                for(std::size_t slot=0;slot<16;++slot)
                    if((slots&(std::uint16_t(1u)<<slot)) && Plexies20260609::HitboxBoneIdForRenderSlot(slot)==prior.boneId)
                        priorBoneAllowed=true;
                if(settings.enabled && priorBoneAllowed && prior.evidence.identity==actors[index].identity && prior.evidence.fresh &&
                    prior.evidence.sequence && continuityMs>=prior.evidence.capturedMs &&
                    continuityMs-prior.evidence.capturedMs<=settings.lostGraceMs &&
                    (!image || (image->epoch==prior.evidence.epoch && image->settingsRevision==prior.evidence.revision))) {
                    observation=prior;observation.retained=true;observation.evidence.fresh=false;
                    observation.evidence.exposed=false;observation.evidence.touching=false;observation.evidence.inside=false;
                    if(image) {
                        observation.evidence.fresh=V2::Vision::IsFresh(*image,settings,now);
                        observation.evidence.sequence=image->sequence;observation.evidence.capturedMs=image->arrivalMs;
                    }
                }
                return observation;
            }
            Contour::Observation ResolveMask(std::size_t index,SkeletonBoneMask mask) const {
                return Resolve(index,Contour::RenderSlotsForMask(mask));
            }
            bool Visible(std::size_t index) const {
                return index<contourVisibility.size() && contourVisibility[index].second;
            }
        };

        inline Vector3 ConfiguredBonePosition(const c_entity& entity, int boneSetting) {
            const int normalized = Config::NormalizeAimBone(boneSetting);
            const int boneId = AimBoneToSkeletonBoneId(normalized);
            return CoreBoneHitboxCenter(entity, boneId);
        }

        inline int ResolveAimBoneForDistance(int configuredBone, float distance) {
            const int normalized = Config::NormalizeAimBone(configuredBone);
            const bool headRequested = normalized == Config::kAimBoneHead;
            const float headGate = std::clamp(Config::View_aimbotMaxHead(), 0.0f, 500.0f);
            if (headRequested && headGate > 0.0f && distance > headGate)
                return Config::kAimBoneNeck;
            return normalized;
        }

        inline bool TrySelectClosestCoreAimPoint(const c_entity& entity,
                                                 const FovRuntimeContext& fovContext,
                                                 const Matrix& view,
                                                 const Vector2& crosshair,
                                                 int& outBoneId,
                                                 Vector3& outPoint,
                                                 float* outScore = nullptr) {
            static constexpr std::array<int, 3> kCoreBones{
                BONE_HEAD,
                BONE_NECK,
                BONE_CHEST,
            };

            bool found = false;
            float bestScore = (std::numeric_limits<float>::max)();
            for (int boneId : kCoreBones) {
                const Vector3 point = CoreBoneHitboxCenter(entity, boneId);
                if (IsZeroVector(point) || point == entity.pos || !IsFiniteVector(point))
                    continue;

                const float score = fovContext.valid
                    ? TargetingDetail::FovScoreDeg(fovContext, point)
                    : crosshair.Distance(view.WorldToScreen(point));
                if (!std::isfinite(score))
                    continue;

                if (!found || score < bestScore) {
                    found = true;
                    bestScore = score;
                    outBoneId = boneId;
                    outPoint = point;
                }
            }

            if (found && outScore)
                *outScore = bestScore;
            return found;
        }

        inline bool TrySelectClosestSkeletonAimPoint(const c_entity& entity,
                                                     const FovRuntimeContext& fovContext,
                                                     const Matrix& view,
                                                     const Vector2& crosshair,
                                                     size_t maxSkeletonBones,
                                                     Vector3& outPoint,
                                                     float* outScore = nullptr) {
            const size_t limit = (std::min)(
                maxSkeletonBones,
                (std::min)(entity.skeleton_bones.size(), entity.skeleton_bone_valid.size()));
            if (limit == 0)
                return false;

            bool found = false;
            float bestScore = (std::numeric_limits<float>::max)();
            for (size_t i = 0; i < limit; ++i) {
                const Vector3 point = SemanticSkeletonRawPosition(entity, static_cast<SkeletonSemantics::Slot>(i));
                if (IsZeroVector(point) || point == entity.pos || !IsFiniteVector(point))
                    continue;

                const float score = fovContext.valid
                    ? TargetingDetail::FovScoreDeg(fovContext, point)
                    : crosshair.Distance(view.WorldToScreen(point));
                if (!std::isfinite(score))
                    continue;

                if (!found || score < bestScore) {
                    found = true;
                    bestScore = score;
                    outPoint = point;
                }
            }

            if (found && outScore)
                *outScore = bestScore;
            return found;
        }

        inline bool DistancePassesAimFilter(float distance,
                                            bool currentTarget = false) {
            const float minDistance = std::clamp(Config::View_aimbotMinDist(), 0.0f, 500.0f);
            const float maxDistance = std::clamp(Config::View_aimbotMaxDist(), 0.0f, 500.0f);
            if (minDistance > 0.0f && distance < minDistance)
                return false;
            const float effectiveMaxDistance =
                TrackingContinuityPolicy::EffectiveMaxDistance(
                    maxDistance,
                    currentTarget);
            if (effectiveMaxDistance > 0.0f && distance > effectiveMaxDistance)
                return false;
            return true;
        }

        inline uint64_t StableTargetKey(const c_entity& entity) {
            if (entity.roster_key != 0)
                return entity.roster_key;
            return entity.address ? entity.address : entity.LinkBase;
        }

        inline DWORD ResolvePolicySliderMs(float value) {
            return static_cast<DWORD>(std::clamp(value, 0.0f, 5000.0f) * 10.0f);
        }

        inline DWORD ResolveAimSessionTimeoutMs() {
            const float value = std::clamp(Config::View_aimbotMaxAim(), 0.0f, 100.0f);
            if (value <= 0.0f || value >= 100.0f)
                return 0;
            return static_cast<DWORD>(value * 20.0f);
        }

        inline TargetLockPolicy ResolveTargetLockPolicy() {
            TargetLockPolicy policy{};
            policy.traceMode = ClampTraceMode(Config::View_aimbotTrace());
            policy.unlockMode = ClampUnlockMode(Config::View_aimbotUnlock());
            policy.minLockMs = static_cast<float>(ResolvePolicySliderMs(Config::View_aimbotLockTime()));
            policy.maxLockMs = static_cast<float>(ResolveAimSessionTimeoutMs());
            policy.retargetHysteresis = std::clamp(Config::View_aimbotStickiness(), 0.0f, 100.0f);
            return policy;
        }

        inline TargetLockRuntime SnapshotTargetLockRuntime() {
            std::lock_guard<std::mutex> lock(target_lock_mutex);
            return target_lock_runtime;
        }

        inline std::mutex& LastTargetCandidateMutex() {
            static std::mutex mutex;
            return mutex;
        }

        inline TargetCandidate& LastTargetCandidateStorage() {
            static TargetCandidate candidate{};
            return candidate;
        }

        inline TargetSolution& LastTargetSolutionStorage() {
            static TargetSolution solution{};
            return solution;
        }

        inline void StoreLastTargetCandidate(const TargetCandidate& candidate) {
            std::lock_guard<std::mutex> lock(LastTargetCandidateMutex());
            LastTargetCandidateStorage() = candidate;
            LastTargetSolutionStorage() = TargetSolution::FromCandidate(candidate);
        }

        inline void StoreLastTargetSolution(const TargetSolution& solution) {
            std::lock_guard<std::mutex> lock(LastTargetCandidateMutex());
            LastTargetSolutionStorage() = solution;
            LastTargetCandidateStorage() = solution.ToCandidate();
        }

        inline TargetCandidate SnapshotLastTargetCandidate() {
            std::lock_guard<std::mutex> lock(LastTargetCandidateMutex());
            return LastTargetCandidateStorage();
        }

        inline TargetSolution SnapshotLastTargetSolution() {
            std::lock_guard<std::mutex> lock(LastTargetCandidateMutex());
            return LastTargetSolutionStorage();
        }

        inline std::mutex& RuntimeIntentEngineMutex() {
            static std::mutex mutex;
            return mutex;
        }

        inline IntentEngine& RuntimeIntentEngineStorage() {
            static IntentEngine engine{};
            return engine;
        }

        inline uint64_t MonotonicNowUs() {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        }

        inline void ResetIntentEngine() {
            std::lock_guard<std::mutex> lock(RuntimeIntentEngineMutex());
            RuntimeIntentEngineStorage().Reset();
        }

        inline void RecordIntentProgramSent(const Vector3& acceptedAngularCommandRad,
                                            uint64_t nowUs = 0) {
            if (!std::isfinite(acceptedAngularCommandRad.X) ||
                !std::isfinite(acceptedAngularCommandRad.Y)) {
                return;
            }
            const float deltaPitchDeg = RAD2DEG(acceptedAngularCommandRad.X);
            const float deltaYawDeg = RAD2DEG(acceptedAngularCommandRad.Y);
            if (std::fabs(deltaYawDeg) <= 1e-5f && std::fabs(deltaPitchDeg) <= 1e-5f)
                return;
            std::lock_guard<std::mutex> lock(RuntimeIntentEngineMutex());
            RuntimeIntentEngineStorage().RecordProgramSent(
                nowUs != 0 ? nowUs : MonotonicNowUs(), deltaYawDeg, deltaPitchDeg);
        }

        inline IntentFrameSnapshot ObserveIntentCamera(const FovRuntimeContext& fovContext,
                                                       uint64_t connectionEpoch,
                                                       bool hasCommittedTarget,
                                                       uint64_t nowUs = 0,
                                                       uint64_t cameraSequenceOverride = 0) {
            uint64_t cameraSeq = cameraSequenceOverride;
            uint64_t sampleTimeUs = nowUs;
            if (cameraSeq == 0 || sampleTimeUs == 0) {
                if (consumerFrame) {
                    if (cameraSeq == 0)
                        cameraSeq = consumerFrame->cameraSample.publishSequence;
                    // R1 fix: always pair camera orientation with its own steady-clock capture
                    // timestamp (NOT consumerFrame->sampledAt which comes from entity samples).
                    if (sampleTimeUs == 0 && consumerFrame->cameraSample.steadyCaptureUs != 0)
                        sampleTimeUs = consumerFrame->cameraSample.steadyCaptureUs;
                } else {
                    std::lock_guard<std::mutex> vmLock(g_viewMatrixMutex);
                    if (cameraSeq == 0)
                        cameraSeq = viewMatrixSampleState.publishSequence;
                    if (sampleTimeUs == 0 && viewMatrixSampleState.steadyCaptureUs != 0)
                        sampleTimeUs = viewMatrixSampleState.steadyCaptureUs;
                }
            }
            if (sampleTimeUs == 0)
                sampleTimeUs = MonotonicNowUs();

            std::lock_guard<std::mutex> lock(RuntimeIntentEngineMutex());
            if (!fovContext.valid)
                return RuntimeIntentEngineStorage().Snapshot();
            const Vector3 euler = DirectionToAimEuler(fovContext.forward);
            const float camPitchDeg = RAD2DEG(euler.X);
            const float camYawDeg = RAD2DEG(euler.Y);
            return RuntimeIntentEngineStorage().ObserveCamera(
                sampleTimeUs,
                camYawDeg,
                camPitchDeg,
                connectionEpoch,
                hasCommittedTarget,
                cameraSeq);
        }

        inline IntentFrameSnapshot SnapshotIntentState() {
            std::lock_guard<std::mutex> lock(RuntimeIntentEngineMutex());
            return RuntimeIntentEngineStorage().Snapshot();
        }

        struct ActiveFirePhaseState {
            AimFirePhase::Phase phase{};
            bool active = false;
        };

        inline thread_local ActiveFirePhaseState activeFirePhaseState{};

        inline void SetActiveFirePhase(const AimFirePhase::Phase& phase, bool active) {
            activeFirePhaseState.phase = phase;
            activeFirePhaseState.active = active;
        }

        inline void ClearActiveFirePhase() {
            activeFirePhaseState = ActiveFirePhaseState{};
        }

        inline std::mutex& RuntimePreAimMutex() {
            static std::mutex mutex;
            return mutex;
        }

        inline PreAim::PreAimTracker& RuntimePreAimTrackerStorage() {
            static PreAim::PreAimTracker tracker{};
            return tracker;
        }

        inline PreAim::ReactionDelayGate& RuntimeReactionDelayGateStorage() {
            static PreAim::ReactionDelayGate gate{};
            return gate;
        }

        inline PreAim::VacuumGraceTracker& RuntimeVacuumGraceTrackerStorage() {
            static PreAim::VacuumGraceTracker tracker{};
            return tracker;
        }

        inline void ResetPhase2RuntimeState() {
            std::lock_guard<std::mutex> lock(RuntimePreAimMutex());
            RuntimePreAimTrackerStorage().ResetAll();
            RuntimeReactionDelayGateStorage().Reset();
            RuntimeVacuumGraceTrackerStorage().Reset();
        }

        inline void ResetTargetLockRuntime() {
            {
                std::lock_guard<std::mutex> lock(target_lock_mutex);
                target_lock_runtime = TargetLockRuntime{};
            }
            std::lock_guard<std::mutex> preAimLock(RuntimePreAimMutex());
            RuntimePreAimTrackerStorage().ResetAll();
        }

        inline void InvalidateTargetBody(uint64_t address,uint64_t rosterKey,uint64_t generation) {
            {
                std::lock_guard<std::mutex> lock(target_lock_mutex);
                if(target_lock_runtime.active && target_lock_runtime.actorGeneration==generation &&
                   (target_lock_runtime.entityKey==address || target_lock_runtime.entityKey==rosterKey ||
                    target_lock_runtime.entityKey==PresentationTiming::ActorControlKey(address,generation)))
                    target_lock_runtime=TargetLockRuntime{};
            }
            std::lock_guard<std::mutex> preAimLock(RuntimePreAimMutex());
            auto& preAim = RuntimePreAimTrackerStorage();
            if (preAim.IsTrackingEntity(address, generation) ||
                preAim.IsTrackingEntity(rosterKey, generation)) {
                preAim.Reset();
            }
            preAim.ClearSuppressionForEntity(address, generation);
            if (rosterKey != 0 && rosterKey != address)
                preAim.ClearSuppressionForEntity(rosterKey, generation);
            auto& reaction = RuntimeReactionDelayGateStorage();
            reaction.InvalidateEntity(address, generation);
            if (rosterKey != 0 && rosterKey != address)
                reaction.InvalidateEntity(rosterKey, generation);
            auto& vacuum = RuntimeVacuumGraceTrackerStorage();
            vacuum.ClearEntity(address, generation);
            if (rosterKey != 0 && rosterKey != address)
                vacuum.ClearEntity(rosterKey, generation);
        }

        inline bool TargetLockExpired(const TargetLockRuntime& state,
                                      const TargetLockPolicy& policy,
                                      DWORD now) {
            if (!state.active || state.entityKey == 0)
                return true;
            if (policy.maxLockMs <= 0.0f)
                return false;
            return now - state.lockStartedTick >= static_cast<DWORD>(policy.maxLockMs);
        }

        inline bool CandidateCanBypassTrace(const TargetLockPolicy& policy,
                                            const TargetLockRuntime& state,
                                            uint64_t candidateKey) {
            if (policy.traceMode == TraceMode::Off)
                return true;
            if (policy.traceMode == TraceMode::Relaxed &&
                state.active &&
                candidateKey != 0 &&
                candidateKey == state.entityKey) {
                return true;
            }
            return false;
        }

        inline bool CandidateBlockedByMinLock(const TargetLockPolicy& policy,
                                              const TargetLockRuntime& state,
                                              uint64_t candidateKey,
                                              DWORD now) {
            if (!state.active || state.entityKey == 0 || candidateKey == 0 ||
                candidateKey == state.entityKey) {
                return false;
            }
            const DWORD minLockMs = static_cast<DWORD>(policy.minLockMs);
            return minLockMs > 0 && now - state.lockStartedTick < minLockMs;
        }

        inline float ApplyRetargetHysteresis(float score,
                                             const TargetLockPolicy& policy,
                                             const TargetLockRuntime& state,
                                             uint64_t candidateKey) {
            if (!state.active || state.entityKey == 0 || candidateKey == state.entityKey)
                return score;
            const float multiplier = 1.0f + std::clamp(policy.retargetHysteresis, 0.0f, 100.0f) * 0.01f;
            return score * multiplier;
        }

        inline void CommitTargetLockRuntime(const TargetCandidate& candidate,
                                            float selectedScore,
                                            const TargetLockPolicy& policy,
                                            DWORD now,
                                            DWORD missingTargetGraceMs = 0,
                                            bool forceUnlock = false,
                                            DWORD reversalStartedTick = 0,
                                            DWORD reversalLatchedUntilTick = 0,
                                            const Vector3& reversalReferenceVelocity = {}) {
            std::lock_guard<std::mutex> lock(target_lock_mutex);
            if (!candidate.valid || candidate.entityKey == 0) {
                const bool expired = target_lock_runtime.active &&
                    TargetLockExpired(target_lock_runtime, policy, now);
                const bool withinMissingGrace = !forceUnlock && target_lock_runtime.active &&
                    TrackingContinuityPolicy::IsWithinTargetDropoutGrace(
                        now,
                        target_lock_runtime.lastSeenTick,
                        missingTargetGraceMs);
                if (forceUnlock || expired ||
                    (policy.unlockMode == UnlockMode::Anytime &&
                     !withinMissingGrace)) {
                    target_lock_runtime = TargetLockRuntime{};
                }
                return;
            }

            const bool switchedActor = !target_lock_runtime.active ||
                target_lock_runtime.entityKey != candidate.entityKey ||
                target_lock_runtime.actorGeneration != candidate.entitySnapshot.actor_generation ||
                TargetLockExpired(target_lock_runtime, policy, now);
            if (switchedActor) {
                target_lock_runtime.entityKey = candidate.entityKey;
                target_lock_runtime.actorGeneration = candidate.entitySnapshot.actor_generation;
                target_lock_runtime.entityIndex = candidate.entityIndex;
                target_lock_runtime.lockStartedTick = now;
                target_lock_runtime.boneId = candidate.boneId;
                target_lock_runtime.boneStartedTick = now;
                target_lock_runtime.reversalStartedTick = 0;
                target_lock_runtime.reversalLatchedUntilTick = 0;
                target_lock_runtime.reversalReferenceVelocity = {};
                target_lock_runtime.active = true;
            } else if (reversalLatchedUntilTick != 0 &&
                       static_cast<int32_t>(reversalLatchedUntilTick - now) > 0) {
                target_lock_runtime.reversalStartedTick = reversalStartedTick;
                target_lock_runtime.reversalLatchedUntilTick = reversalLatchedUntilTick;
                target_lock_runtime.reversalReferenceVelocity = reversalReferenceVelocity;
            }

            if (target_lock_runtime.entityKey == candidate.entityKey &&
                candidate.boneId != Plexies20260609::kUnusedRenderSkeletonBone &&
                candidate.boneId != target_lock_runtime.boneId) {
                target_lock_runtime.boneId = candidate.boneId;
                target_lock_runtime.boneStartedTick = now;
            }

            target_lock_runtime.lastSeenTick = now;
            target_lock_runtime.lastScore = selectedScore;
            target_lock_runtime.entityIndex = candidate.entityIndex;
            target_lock_runtime.lastVelocity = candidate.motion.worldVelocity;
            target_lock_runtime.hasLastVelocity = true;
        }

        inline EntityMotionState EstimateMotionState(const c_entity& entity, const Vector2& screenPoint) {
            (void)screenPoint;

            const Motion::EntityMotionEstimate motion = Motion::EstimateEntityMotion(entity);
            const Vector3 velocity = motion.valid ? motion.effectiveVelocity : entity.velocity;

            EntityMotionState state{};
            state.worldVelocity = velocity;
            state.verticalVelocity = velocity.Y;

            const float horizontalSpeed = sqrtf(
                velocity.X * velocity.X +
                velocity.Z * velocity.Z);
            const float verticalSpeed = velocity.Y;

            if (!std::isfinite(horizontalSpeed) || !std::isfinite(verticalSpeed) ||
                horizontalSpeed > 250.0f || fabsf(verticalSpeed) > 250.0f) {
                state.kind = EntityMotionState::Kind::TeleportOrInvalid;
                state.confidence = 0.25f;
                return state;
            }

            if (verticalSpeed > 1.0f) {
                state.kind = EntityMotionState::Kind::AirborneRising;
                state.confidence = 0.55f;
            } else if (verticalSpeed < -1.0f) {
                state.kind = EntityMotionState::Kind::AirborneFalling;
                state.confidence = 0.55f;
            } else if (horizontalSpeed > 2.0f) {
                state.kind = EntityMotionState::Kind::Strafing;
                state.confidence = motion.usedWorldDeltaFallback ? 0.50f : 0.45f;
            } else {
                state.kind = EntityMotionState::Kind::Grounded;
                state.confidence = 0.35f;
            }

            return state;
        }

        #include "Game/TargetPredictionPipeline.inl"

        inline float CandidateScore(const Vector3& position, const FovRuntimeContext& fovContext) {
            return TargetingDetail::FovScoreDeg(fovContext, position);
        }

        inline SelectionResult SelectTargetFromSnapshot(const std::vector<c_entity>& snapshot,
                                                        const c_entity& local,
                                                        bool predit,
                                                        bool secondary,
                                                        int teamMode,
                                                        int boneSetting,
                                                        float fov) {
            SelectionResult result{};
            Config::ScopedConfigRead configRead;
            const FovRuntimeContext fovContext = SnapshotFovRuntimeContext();

            for (size_t i = 0; i < snapshot.size(); ++i) {
                c_entity entity = snapshot[i];
                if (!IsSelectableCandidate(entity, teamMode, local)) continue;

                Vector3 rootPosition = ConfiguredBonePosition(entity, boneSetting);
                if (IsZeroVector(rootPosition)) continue;

                Vector3 aimPosition = ApplyPrediction(entity, rootPosition, predit, secondary);
                const float score = CandidateScore(aimPosition, fovContext);
                if (score >= result.score) continue;
                const float distance = fovContext.valid
                    ? fovContext.camera.DistTo(rootPosition)
                    : local.pos.DistTo(rootPosition);
                const float effectiveFov = Config::ResolveDynamicAimFovForDistance(fov, distance);
                if (score > FovAngleLimitDeg(effectiveFov)) continue;

                result.index = static_cast<int>(i);
                result.target = aimPosition;
                result.entity = entity;
                result.score = score;
            }

            return result;
        }

        inline Vector3 SelectAutoBone(c_entity entity, const Vector2& crosshair, bool predit, bool secondary, size_t maxSkeletonBones) {
            const Matrix view = OW::SnapshotViewMatrix();
            Config::ScopedConfigRead configRead;
            const FovRuntimeContext fovContext = SnapshotFovRuntimeContext();
            int bestBoneId = BONE_CHEST;
            Vector3 bestRoot{};
            if (!TrySelectClosestCoreAimPoint(entity, fovContext, view, crosshair, bestBoneId, bestRoot)) {
                const Vector3 configuredRoot =
                    ConfiguredBonePosition(entity, secondary ? Config::Bone2 : Config::View_Bone());
                if (!IsZeroVector(configuredRoot) &&
                    configuredRoot != entity.pos &&
                    IsFiniteVector(configuredRoot)) {
                    bestRoot = configuredRoot;
                } else if (!TrySelectClosestSkeletonAimPoint(
                    entity, fovContext, view, crosshair, maxSkeletonBones, bestRoot)) {
                    return Vector3{};
                }
            }
            return ApplyPrediction(entity, bestRoot, predit, secondary);
        }

        inline void ApplyTargetOutline(const c_entity& entity, bool secondary) {
            (void)entity;
            (void)secondary;
        }

        inline void UpdateHanzoAutoSpeed(const c_entity& local) {
            if (!Config::hanzoautospeed || local.HeroID != eHero::HERO_HANJO) return;
            Config::predit_level = readult(local.SkillBase + 0x40, 0xB, 0x2A5) * 85.f + 25.f;
            if (local.skill2act) Config::predit_level = 110.f;
        }
    }

    inline bool IsValidTargetIndex(int index) {
        c_entity target{};
        return TargetingDetail::TryEntityAt(index, target, false);
    }

    inline bool TryGetTargetEntity(int index, c_entity& entity, bool requireVisible = false) {
        return TargetingDetail::TryEntityAt(index, entity, requireVisible);
    }

    inline bool TryGetTargetEntityStage0(int index, c_entity& entity) {
        return TargetingDetail::TryEntityAtStage0(index, entity);
    }

    // =========================================================================
    // Angle helpers
    // =========================================================================

    inline bool in_range(Vector3 MyAngle, Vector3 EnemyAngle, Vector3 MyPosition, Vector3 EnemyPosition, float radius) {
        float dist = MyPosition.DistTo(EnemyPosition);
        radius /= dist;
        return MyAngle.DistTo(EnemyAngle) <= radius;
    }

    inline XMFLOAT3 CalcAngle(XMFLOAT3 Target, XMFLOAT3 CameraPos) {
        XMFLOAT3 Result;
        float Distance = XMVectorGetX(XMVector3Length(XMLoadFloat3(&Target) - XMLoadFloat3(&CameraPos)));
        if (Distance < 0.0001f) Distance = 0.0001f;
        XMStoreFloat3(&Result, (XMLoadFloat3(&Target) - XMLoadFloat3(&CameraPos)) / Distance);
        return Result;
    }

    // =========================================================================
    // Skill lookup (hanzo projectile speed)
    // =========================================================================

    inline float GetLookUpSkill(uint16_t a1) {
        const uint64_t localSkillBase = SnapshotLocalSkillBase();
        if (!localSkillBase)
            return 0.f;

        __try {
            uint64_t pSkill = SDK->RPM<uint64_t>(localSkillBase + 0x1848);
            uint64_t SkillRawList = SDK->RPM<uint64_t>(pSkill + 0x10);
            uint32_t SkillSize = SDK->RPM<uint32_t>(pSkill + 0x18);
            for (uint32_t i = 0; i < SkillSize; i++) {
                if (SDK->RPM<uint16_t>(SkillRawList + (i * 0x80)) == a1) {
                    float val = SDK->RPM<float>(SkillRawList + (i * 0x80) + 0x30);
                    if (val >= 0.5f) return val;
                }
            }
        } __except (1) {}
        return 0.f;
    }

    // =========================================================================
    // Main target selection (GetVector3)
    // =========================================================================

    // Candidate evaluation changes only a bounded set of worker-local scalar views.
    // No controller state, global config, or per-candidate configuration copy is involved.
    struct AimCandidateConfig {
        AimPlan::Effective value{};
        bool active = Config::targetAimPlanEnabled;
        float fov=Config::View_Fov(), lo=Config::View_aimbotMinDist(), hi=Config::View_aimbotMaxDist();
        int mode=Config::View_aimbotFovMode(), prediction=Config::View_aimbotPredictionMode();
        bool legacyPrediction=Config::View_Prediction();
        SkeletonBoneMask mask=Config::View_aimbotBoneMask();
        explicit AimCandidateConfig(uint64_t hero) {
            if(!active) return;
            value=AimPlan::Resolve(Config::View_aimPlan(),hero);
            if(value.overrideLimits) {
                Config::View_Fov()=value.limits.fov; Config::View_aimbotFovMode()=0;
                Config::View_aimbotMinDist()=value.limits.minDistance; Config::View_aimbotMaxDist()=value.limits.maxDistance;
            }
            if(value.overrideAimPoint) {
                Config::View_aimbotBoneMask()=value.point.boneMask; Config::View_aimbotPredictionMode()=value.point.predictionMode;
                const auto local=TargetingDetail::SnapshotLocalEntity();
                Config::View_Prediction()=ResolvePredictionEnabled(ClampPredictionOverride(value.point.predictionMode),
                    ResolveWeaponSpec(local.HeroID,Config::View_aimbotAttack()),legacyPrediction);
            }
        }
        ~AimCandidateConfig() {
            if(!active) return;
            Config::View_Fov()=fov; Config::View_aimbotMinDist()=lo; Config::View_aimbotMaxDist()=hi;
            Config::View_aimbotFovMode()=mode; Config::View_aimbotPredictionMode()=prediction; Config::View_aimbotBoneMask()=mask; Config::View_Prediction()=legacyPrediction;
        }
        bool PassesStage0(const c_entity& e) const {
            if(!active) return true;
            const auto& p=Config::View_aimPlan().selection;
            return AimPlan::EligiblePreGeometryStage0(p,value,e.HeroID,e.PlayerHealth,e.PlayerHealthMax);
        }
        bool Passes(const c_entity& e) const {
            if(!active) return true;
            const auto& p=Config::View_aimPlan().selection;
            return AimPlan::EligiblePreGeometry(p,value,e.HeroID,e.PlayerHealth,e.PlayerHealthMax,TargetingDetail::SelectionVisible(e));
        }
        bool Passes(const c_entity& e, float distance, float angleDeg,
                    std::optional<bool> stage2VisibleOverride = std::nullopt) const {
            if(!active) return true;
            const auto& p=Config::View_aimPlan().selection;
            const bool vis = stage2VisibleOverride.has_value()
                ? *stage2VisibleOverride
                : TargetingDetail::SelectionVisible(e);
            const AimPlan::RuleObservation obs{
                e.HeroID, e.PlayerHealth, e.PlayerHealthMax,
                vis, distance, angleDeg};
            return AimPlan::Eligible(p, value, obs);
        }
        bool Preferred(const c_entity& e,
                       float distance = std::numeric_limits<float>::quiet_NaN(),
                       float angleDeg = std::numeric_limits<float>::quiet_NaN()) const {
            if (!active) return false;
            const auto& p=Config::View_aimPlan().selection;
            const AimPlan::RuleObservation obs{
                e.HeroID, e.PlayerHealth, e.PlayerHealthMax,
                TargetingDetail::SelectionVisible(e), distance, angleDeg};
            return AimPlan::Preferred(p, obs);
        }
    };
    inline void PublishSelectedAimPlan(const TargetCandidate& candidate) {
        Config::selectedAimFovDeg=candidate.valid?candidate.effectiveFovDeg:0;
        Config::selectedAimPlan=Config::targetAimPlanEnabled && candidate.valid ? candidate.plan : AimPlan::Effective{};
        Config::selectedAimPlanTarget=candidate.valid?candidate.entityKey:0;
        Config::selectedAimHero=candidate.valid?candidate.entitySnapshot.HeroID:0;
        Config::selectedAimActor=candidate.valid?candidate.entitySnapshot.actor_generation:0;
        Config::selectedAimEpoch=candidate.valid?candidate.entitySnapshot.connection_epoch:0;
        Config::selectedAimScene=candidate.valid?candidate.entitySnapshot.scene_generation:0;
        Config::selectedAimSample=candidate.valid?(candidate.entitySnapshot.sample_time_us?candidate.entitySnapshot.sample_time_us:uint64_t(candidate.entitySnapshot.anchor_sample_tick_ms)*1000):0;
        static_cast<AimPlan::Motion&>(Config::selectedAimMethod)=Config::selectedAimPlan.motion;
    }

    inline RayDecision MakeNativeRayDecision(const c_entity& entity, int boneId,
                                             const Vector3& origin,
                                             const Vector3& rayDirection,
                                             const Vector3& worldTargetPoint,
                                             const NativeHitWindowInput& input,
                                             uint64_t frame,
                                             bool requireExplicitCritical = false,
                                             const TrajectoryRay::ParabolicChordPlan* chordPlan = nullptr,
                                             bool submitBrokerQuery = true,
                                             uint32_t maxAgeMs = RayDecision::kDefaultMaxAgeMs) {
        RayDecision decision{};
        decision.frame = frame;
        decision.connectionEpoch = entity.connection_epoch;
        decision.sceneEpoch = RayRuntime::SceneEpoch();
        decision.entityKey = TargetingDetail::StableTargetKey(entity);
        decision.actorGeneration = entity.actor_generation;
        decision.sampleTick = entity.native_hitboxes.tick;
        const uint32_t now = GetTickCount();
        decision.actor = RayDecisionStatus::Unknown;
        if (NativeHitboxSampleUsable(entity, now) && input.projectileRadius >= 0.0f) {
            const auto& sample = entity.native_hitboxes;
            const float worldScale = (std::min)({
                sample.world.record.scale[0],
                sample.world.record.scale[1],
                sample.world.record.scale[2]});
            const Vector3 shapeShift = (entity.pos - sample.world.sampleAnchor) + input.displacement;
            if (std::isfinite(worldScale) && worldScale > 0.0f &&
                std::isfinite(shapeShift.X) && std::isfinite(shapeShift.Y) &&
                std::isfinite(shapeShift.Z)) {
                std::vector<V3Raycast::WorldShape> shapes;
                shapes.reserve(sample.shapes.size());
                float maxShapeRadius = 0.0f;
                for (size_t index = 0; index < sample.shapes.size() && index < sample.poses.size(); ++index) {
                    const auto& source = sample.shapes[index];
                    const auto& pose = sample.poses[index];
                    if (source.boneId != boneId || !pose.Valid() ||
                        !std::isfinite(source.radius) || source.radius <= 0.0f) continue;
                    if (requireExplicitCritical &&
                        source.critical != ModelShapes::CriticalState::Critical) continue;
                    const float poseScale = (std::min)({pose.scale[0], pose.scale[1], pose.scale[2]});
                    if (!std::isfinite(poseScale) || poseScale <= 0.0f) continue;
                    V3Raycast::WorldShape shape{};
                    shape.a = sample.world.TransformPoint(ModelShapes::Transform(source.a, pose)) + shapeShift;
                    shape.b = sample.world.TransformPoint(ModelShapes::Transform(source.b, pose)) + shapeShift;
                    shape.radius = source.radius * poseScale * worldScale;
                    shape.mappedBone = source.boneId;
                    shape.critical = source.critical;
                    if (!std::isfinite(shape.a.X) || !std::isfinite(shape.a.Y) || !std::isfinite(shape.a.Z) ||
                        !std::isfinite(shape.b.X) || !std::isfinite(shape.b.Y) || !std::isfinite(shape.b.Z) ||
                        !std::isfinite(shape.radius) || shape.radius <= 0.0f) continue;
                    maxShapeRadius = (std::max)(maxShapeRadius, shape.radius);
                    shapes.push_back(shape);
                }
                if (!shapes.empty()) {
                    const float clampedScale = Config::ClampHitboxScalePercent(input.hitboxScalePercent);
                    if (input.projectileSpeed > 0.0f) {
                        // Projectile / gravity weapons require the continuous relative-frame
                        // trajectory intersection rather than a straight-line segment.
                        const auto score = NativeHitWindowScore(
                            entity, boneId, now, origin, rayDirection, input, {},
                            requireExplicitCritical);
                        decision.actor = (score && std::isfinite(*score) && *score <= 1.0f)
                            ? RayDecisionStatus::Hit
                            : RayDecisionStatus::Miss;
                    } else {
                        const Vector3 launchDirection = rayDirection + Vector3(0.0f, input.launchUpBias, 0.0f);
                        const float distance = launchDirection.Size();
                        const float safeProjectileRadius = std::isfinite(input.projectileRadius)
                            ? (std::max)(0.0f, input.projectileRadius)
                            : 0.0f;
                        const float maxEffectiveRadius =
                            (maxShapeRadius + safeProjectileRadius) * (clampedScale / 100.0f);
                        const float maxDistance = distance + maxEffectiveRadius + 0.5f;
                        V3Raycast::Query rayQuery{origin, launchDirection, maxDistance,
                                                  safeProjectileRadius, clampedScale};
                        const auto hit = V3Raycast::TraceNative(rayQuery, shapes);
                        decision.actor = hit.status == V3Raycast::Status::Hit
                            ? RayDecisionStatus::Hit
                            : hit.status == V3Raycast::Status::Miss ? RayDecisionStatus::Miss
                                                                    : RayDecisionStatus::Unknown;
                    }
                }
            }
        }
        // Only skip world occlusion on a definitive actor Miss; Unknown falls back to
        // skeleton geometry for the actor part and still requires UniCore WorldClear in Hybrid.
        if (decision.actor == RayDecisionStatus::Miss)
            return decision;
        if (UI::Preferences::ActiveRayBackend() == static_cast<int>(RayDecisionPolicy::NativeOnly)) {
            decision.world = RayDecisionStatus::Clear;
            return decision;
        }
        // The live ray scene is owned by the probe runtime: it advances only when the
        // product-resolved level context changes or dies. The entity pipeline's
        // scene_generation was measured not to change on a map switch and must not be
        // used as the world-query scene key.
        const uint64_t rayScene = RayRuntime::SceneEpoch();
        if (rayScene == 0) {
            decision.world = RayDecisionStatus::Invalid;
            return decision;
        }
        decision.sceneEpoch = rayScene;

        const RayDecision worldVerdict = TrajectoryRay::EvaluateParabolicBrokerDecision(
            RayRuntime::Broker(),
            chordPlan,
            origin,
            worldTargetPoint,
            frame,
            entity.connection_epoch,
            rayScene,
            decision.entityKey,
            decision.actorGeneration,
            boneId,
            decision.sampleTick,
            RayRuntime::WorldRayMask(),
            maxAgeMs,
            submitBrokerQuery);
        decision.world = worldVerdict.world;
        decision.ageMs = worldVerdict.ageMs;
        decision.latencyUs = worldVerdict.latencyUs;
        decision.completedAtUs = worldVerdict.completedAtUs;
        decision.fraction = worldVerdict.fraction;
        decision.hitPosX = worldVerdict.hitPosX;
        decision.hitPosY = worldVerdict.hitPosY;
        decision.hitPosZ = worldVerdict.hitPosZ;
        decision.hasHitPosition = worldVerdict.hasHitPosition;
        decision.hitNormalX = worldVerdict.hitNormalX;
        decision.hitNormalY = worldVerdict.hitNormalY;
        decision.hitNormalZ = worldVerdict.hitNormalZ;
        decision.hasHitNormal = worldVerdict.hasHitNormal;
        decision.hitFlags = worldVerdict.hitFlags;
        decision.hitbox = worldVerdict.hitbox;
        decision.hitEntityId = worldVerdict.hitEntityId;
        decision.staleReason = worldVerdict.staleReason;
        decision.segmentKind = worldVerdict.segmentKind;
        decision.segmentIndex = worldVerdict.segmentIndex;
        return decision;
    }

    inline void WarnIfHybridServiceUnbound(int activeRayBackend, DWORD now) {
        if (activeRayBackend != static_cast<int>(RayDecisionPolicy::Hybrid) ||
            RayRuntime::HasAttachedService())
            return;
        static std::atomic<DWORD> lastWarnTick{0};
        DWORD previous = lastWarnTick.load(std::memory_order_relaxed);
        if (previous != 0 && (now - previous) < 5000) return;
        if (lastWarnTick.compare_exchange_strong(previous, now, std::memory_order_relaxed)) {
            Diagnostics::Warn(
                "[RAY] Hybrid policy active without an attached live UniCore EngineTrace::Service; "
                "world occlusion is unavailable and non-contour targets are strictly rejected.");
        }
    }

    inline float AimRankingFovScale() {
        float upper = Config::View_Fov();
        if (Config::IsDynamicAimFovActive()) {
            const auto* dynamic = Config::FindDynamicFovPreset(Config::View_aimbotDynamicFovPresetId());
            for (int i = 0; i < dynamic->pointCount; ++i)
                upper = (std::max)(upper, dynamic->points[static_cast<size_t>(i)].fovDeg);
        }
        if (Config::targetAimPlanEnabled)
            for (const auto& group : Config::View_aimPlan().groups)
                if (group.overrideLimits) upper = (std::max)(upper, group.limits.fov);
        return upper;
    }

    inline TargetSolution AcquireTargetSolution(
        const ConsumptionFrame& frame,
        bool predit = false,
        bool ignoreInvisible = Config::View_aimbotIgnoreInvisible(),
        DWORD trackingContinuityGraceMs = 0,
        const TargetPipeline::AcquisitionContext* contextOverride = nullptr) {
        TargetSolution best{};
        Config::ScopedConfigRead configRead;
        TargetingDetail::ContourEvaluation contour(frame,Config::View_aimbotContourEnabled());
        TargetSolution lockedSolution{};
        const float commonFov=TrackingContinuityPolicy::EffectiveFovDeg(AimRankingFovScale(),trackingContinuityGraceMs!=0);
        float commonDistance=Config::View_aimbotMaxDist();
        if(commonDistance<=0) commonDistance=500;
        if(Config::targetAimPlanEnabled) for(const auto& g:Config::View_aimPlan().groups) if(g.overrideLimits) {
            commonDistance=(std::max)(commonDistance,g.limits.maxDistance>0?g.limits.maxDistance:500.0f);
        }
        Vector2 CrossHair = TargetingDetail::CrosshairCenter();
        const Matrix& aimViewMatrix = frame.view;
        const TargetingDetail::FovRuntimeContext fovContext = TargetingDetail::SnapshotFovRuntimeContext();
        const auto& entities = frame.entities;
        const auto& hp_dy_entities = frame.dynamic;
        const auto& local_entity = frame.local;
        const DWORD now = GetTickCount();
        const uint64_t now64 = GetTickCount64();
        const bool localTargetingReady =
            local_entity.Alive &&
            local_entity.roster_state == EntityRosterState::Fresh &&
            local_entity.HasUsableBody(now, EntitySceneGeneration()) &&
            PresentationTiming::Fresh(local_entity.anchor_sample_tick_ms, now);
        if (!localTargetingReady) {
            // A local death or body handoff must not leave the previous enemy
            // highlighted while the renderer continues showing the scene.
            Config::Targetenemyi = -1;
            Config::health = 0.0f;
            Config::aimbotEffectiveHitWindow = 0.0f;
            TargetingDetail::ResetTargetLockRuntime();
            TargetingDetail::ResetPhase2RuntimeState();
            TargetingDetail::StoreLastTargetSolution(best);
            PublishSelectedAimPlan(TargetCandidate{});
            return best;
        }

        TargetPipeline::AcquisitionContext acqCtx{};
        if (contextOverride) {
            acqCtx = *contextOverride;
        } else {
            acqCtx.firePhase = TargetingDetail::activeFirePhaseState.phase;
            acqCtx.firePhaseActive = TargetingDetail::activeFirePhaseState.active;
            acqCtx.useIntentEngine = true;
            acqCtx.stage1TopK = 3;
        }

        const WeaponSpec* weaponSpec = ResolveWeaponSpec(local_entity.HeroID, Config::View_aimbotAttack());
        const TargetLockPolicy lockPolicy = TargetingDetail::ResolveTargetLockPolicy();
        TargetingDetail::TargetLockRuntime activeLock = TargetingDetail::SnapshotTargetLockRuntime();
        if (TargetingDetail::TargetLockExpired(activeLock, lockPolicy, now))
            activeLock.active = false;
        const bool trackingContinuityEnabled = trackingContinuityGraceMs != 0;
        bool activeLockCandidateSelectable = false;
        const bool resolvedPrediction = ResolvePredictionEnabled(
            ClampPredictionOverride(Config::View_aimbotPredictionMode()),
            weaponSpec,
            predit);

        const int activeRayBackend = UI::Preferences::ActiveRayBackend();
        const uint64_t worldRayMask = RayRuntime::WorldRayMask();
        const uint64_t barrierRayMask = RayRuntime::BarrierRayMask();
        const BarrierPolicy slotBarrierPolicy = ClampBarrierPolicy(Config::slotTargetRules.active
            ? Config::slotTargetRules.barrierPolicy : Config::aimBarrierPolicy);
        const RoleUtilityMode slotRoleUtilityMode = ClampRoleUtilityMode(Config::slotTargetRules.active
            ? Config::slotTargetRules.roleUtilityMode : Config::aimRoleUtilityMode);
        const VisibilityPolicy slotVisibilityPolicy = ResolveVisibilityPolicy(
            activeRayBackend, contour.enabled, slotBarrierPolicy, barrierRayMask);
        const int activeTeamMode = Config::View_aimbotTeam();
        const float activeAllyHealthMaxPercent = Config::slotTargetRules.active
            ? Config::slotTargetRules.allyHealthMaxPercent : 100.0f;
        const auto evaluateRoleUtility = [&](const c_entity& ent,
                                             float angleDeg,
                                             float effFovDeg,
                                             float dist) noexcept -> float {
            TargetPipeline::RoleUtilityContext rCtx{};
            rCtx.localHeroId = local_entity.HeroID;
            rCtx.teamMode = activeTeamMode;
            rCtx.candidateIsEnemy = ent.Team;
            rCtx.candidateHeroId = ent.HeroID;
            rCtx.hp = ent.PlayerHealth;
            rCtx.maxHp = ent.PlayerHealthMax;
            rCtx.angleDeg = angleDeg;
            rCtx.effectiveFovDeg = effFovDeg;
            rCtx.distance = dist;
            rCtx.allyHealthMaxPercent = activeAllyHealthMaxPercent;
            rCtx.candidateImmortal = ent.imort;
            rCtx.candidateBarrierProtected = ent.barrprot;
            return TargetPipeline::RoleUtilityRegistry::Default().Evaluate(slotRoleUtilityMode, rCtx);
        };
        const bool hybridLiveWorldGate =
            activeRayBackend == static_cast<int>(RayDecisionPolicy::Hybrid) &&
            RayRuntime::HasAttachedService() &&
            RayRuntime::SceneEpoch() != 0;
        WarnIfHybridServiceUnbound(activeRayBackend, now);
        size_t selectableCandidates = 0;
        size_t rayRejectedCandidates = 0;
        Diagnostics::Aim("target.primary start prediction=%d predictionMode=%d weapon=%s entities=%zu dynamic=%zu local_addr=0x%llX local_hero=0x%llX fovDeg=%.6f bone=%d autobone=%d boneMask=0x%03X teamMode=%d distance=(%.2f,%.2f) lock=(active=%d key=0x%llX minMs=%.0f maxMs=%.0f hysteresis=%.1f trace=%d unlock=%d) crosshair=(%.3f,%.3f)",
            resolvedPrediction ? 1 : 0,
            Config::View_aimbotPredictionMode(),
            weaponSpec ? weaponSpec->weaponId.data() : "none",
            entities.size(),
            hp_dy_entities.size(),
            static_cast<unsigned long long>(local_entity.address),
            static_cast<unsigned long long>(local_entity.HeroID),
            Config::View_Fov(),
            Config::View_Bone(),
            Config::View_autobone() ? 1 : 0,
            static_cast<unsigned int>(Config::View_aimbotBoneMask()),
            Config::View_aimbotTeam(),
            Config::View_aimbotMinDist(),
            Config::View_aimbotMaxDist(),
            activeLock.active ? 1 : 0,
            static_cast<unsigned long long>(activeLock.entityKey),
            lockPolicy.minLockMs,
            lockPolicy.maxLockMs,
            lockPolicy.retargetHysteresis,
            static_cast<int>(lockPolicy.traceMode),
            static_cast<int>(lockPolicy.unlockMode),
            CrossHair.X,
            CrossHair.Y);
        const ProjectileRuntimeSpec projectileSpec =
            TargetingDetail::ResolveProjectileRuntimeSpec(weaponSpec, local_entity, false);

        // =====================================================================
        // Stage 0: Cheap Entity-Level Hard Filter (NEVER eliminates on entity Vis)
        // =====================================================================
        struct Stage0Entity {
            size_t index = 0;
            uint64_t key = 0;
            bool isLocked = false;
            float coarseDistance = 0.0f;
            float coarseAngleDeg = 0.0f;
            float coarseErrYawDeg = 0.0f;
            float coarseErrPitchDeg = 0.0f;
            Vector3 coarseAnchor{};
            bool hasCoarseAngle = false;
        };
        std::vector<Stage0Entity> stage0;
        stage0.reserve((std::min)(entities.size(), static_cast<size_t>(16)));

        const float entryOutsidePadDeg = Config::View_aimbotPredictFovEntry()
            ? Config::ClampFovEntryMaxOutsideDeg(Config::View_aimbotFovEntryMaxOutsideDeg())
            : 0.0f;

        for (size_t i = 0; i < entities.size(); ++i) {
            const c_entity& entity = entities[i];
            AimCandidateConfig candidateConfig(entity.HeroID);
            if (!candidateConfig.PassesStage0(entity))
                continue;
            if (!TargetingDetail::IsRuntimeTargetValidStage0(entity))
                continue;
            if (!TargetingDetail::TargetTeamMatches(entity, Config::View_aimbotTeam(), local_entity))
                continue;

            const uint64_t candidateKey = TargetingDetail::StableTargetKey(entity);
            const bool isLocked = activeLock.active && candidateKey != 0 && candidateKey == activeLock.entityKey;
            const bool applyTrackingExitMargin = trackingContinuityEnabled && isLocked;

            Vector3 anchor = !TargetingDetail::IsZeroVector(entity.chest_pos) ? entity.chest_pos : entity.pos;
            if (!TargetingDetail::IsFiniteVector(anchor) || TargetingDetail::IsZeroVector(anchor))
                continue;

            const float initialDistance = fovContext.valid
                ? fovContext.camera.DistTo(anchor)
                : TargetingDetail::CameraPosition().DistTo(anchor);
            if (!std::isfinite(initialDistance))
                continue;
            if (!TargetingDetail::DistancePassesAimFilter(initialDistance, applyTrackingExitMargin))
                continue;

            float coarseAngleDeg = 0.0f;
            float errYawDeg = 0.0f;
            float errPitchDeg = 0.0f;
            bool hasCoarseAngle = false;
            if (fovContext.valid) {
                coarseAngleDeg = TargetingDetail::FovScoreDeg(fovContext, anchor);
                hasCoarseAngle = TargetingDetail::ComputeAngularErrorDeg(
                    fovContext, anchor, errYawDeg, errPitchDeg);
                const float activeFovDeg = TrackingContinuityPolicy::EffectiveFovDeg(
                    Config::ResolveDynamicAimFovForDistance(Config::View_Fov(), initialDistance),
                    applyTrackingExitMargin);
                const float peekGateCoarseDeg = PreAim::ComputeDynamicPeekAngleDeg(
                    initialDistance,
                    activeFovDeg,
                    projectileSpec.projectileSpeed,
                    PreAim::DefaultPeekSpeedMps(entity.HeroID));
                const float peekExitCoarseDeg = PreAim::ComputePeekExitAngleDeg(
                    peekGateCoarseDeg, activeFovDeg);
                // Generous +6 deg coarse envelope so Stage 0 never culls a target whose head/limb,
                // predicted entry point, or PreAim corridor peek window sits inside Stage 2 gates.
                const float coarseEnvelopeDeg = (std::min)(
                    180.0f,
                    (std::max)(activeFovDeg + entryOutsidePadDeg, peekExitCoarseDeg) + 6.0f);
                if (std::isfinite(coarseAngleDeg) && coarseAngleDeg > coarseEnvelopeDeg)
                    continue;
            }

            stage0.push_back(Stage0Entity{
                i, candidateKey, isLocked, initialDistance,
                coarseAngleDeg, errYawDeg, errPitchDeg, anchor, hasCoarseAngle
            });
        }

        // =====================================================================
        // Stage 1: Utility Scoring + Commitment + IntentEngine + Top-K Ranking
        // =====================================================================
        IntentFrameSnapshot intentSnapshot{};
        bool reverseLockBroken = false;
        uint64_t reverseBrokenEntityKey = 0;
        uint64_t reverseBrokenActorGeneration = 0;
        bool lockedTargetReversalActive = false;
        DWORD lockedReversalStarted = activeLock.reversalStartedTick;
        DWORD lockedReversalLatchedUntil = activeLock.reversalLatchedUntilTick;
        Vector3 lockedReversalRefVel = activeLock.reversalReferenceVelocity;
        if (acqCtx.useIntentEngine) {
            std::lock_guard<std::mutex> lock(TargetingDetail::RuntimeIntentEngineMutex());
            auto& intentEngine = TargetingDetail::RuntimeIntentEngineStorage();
            const Stage0Entity* lockedStage0 = nullptr;
            for (const auto& entry : stage0) {
                if (entry.isLocked) {
                    lockedStage0 = &entry;
                    break;
                }
            }
            if (lockedStage0 && lockedStage0->hasCoarseAngle) {
                const c_entity& lockedEntity = entities[lockedStage0->index];
                const Motion::EntityMotionEstimate est = Motion::EstimateEntityMotion(lockedEntity);
                const Vector3 currVel = est.valid ? est.effectiveVelocity : lockedEntity.velocity;
                const bool freshReversal = activeLock.hasLastVelocity &&
                    TargetPipeline::DetectTargetVelocityReversal(activeLock.lastVelocity, currVel);
                if (freshReversal) {
                    // Latch reversal protection across prediction-lag recovery (160 ms, Decision #1, R6 fix).
                    // For a complete sudden stop, preserve -previousVelocity as the expected correction direction.
                    constexpr DWORD kReversalLatchMs = 160;
                    lockedReversalStarted = now;
                    lockedReversalLatchedUntil = now + kReversalLatchMs;
                    const float currHorizSpeed = std::hypot(currVel.X, currVel.Z);
                    lockedReversalRefVel = currHorizSpeed >= 1.0f
                        ? currVel
                        : (activeLock.lastVelocity * -1.0f);
                }
                lockedTargetReversalActive = lockedReversalLatchedUntil != 0 &&
                    static_cast<int32_t>(lockedReversalLatchedUntil - now) > 0;
                float velYawRate = 0.0f;
                float velPitchRate = 0.0f;
                if (lockedTargetReversalActive && fovContext.valid) {
                    TargetPipeline::ProjectWorldVelocityToAngularRates(
                        fovContext.camera, lockedStage0->coarseAnchor,
                        lockedReversalRefVel, velYawRate, velPitchRate);
                }
                intentSnapshot = intentEngine.UpdateActiveTargetFeedback(
                    lockedStage0->coarseErrYawDeg,
                    lockedStage0->coarseErrPitchDeg,
                    true,
                    lockedTargetReversalActive,
                    velYawRate,
                    velPitchRate);
                if (intentSnapshot.reverseBreakTriggered && !intentSnapshot.reverseExemptByTargetReversal) {
                    reverseBrokenEntityKey = activeLock.entityKey;
                    reverseBrokenActorGeneration = activeLock.actorGeneration;
                    activeLock.active = false;
                    reverseLockBroken = true;
                    lockedTargetReversalActive = false;
                    lockedReversalStarted = 0;
                    lockedReversalLatchedUntil = 0;
                    intentEngine.ClearReverseBreakState();
                    intentSnapshot = intentEngine.Snapshot();
                }
            } else {
                intentSnapshot = intentEngine.UpdateActiveTargetFeedback(0.0f, 0.0f, false);
            }
        }
        const float lockedReversalElapsedMs = (lockedTargetReversalActive && lockedReversalStarted != 0)
            ? static_cast<float>(now - lockedReversalStarted)
            : 0.0f;
        if (reverseLockBroken) {
            // Unconditionally clear persistent shared lock regardless of UnlockMode (Finding #5),
            // and notify PreAimTracker specifically for the broken actor (F1 fix) so a different
            // candidate selected afterward in this tick is never falsely suppressed.
            TargetingDetail::ResetTargetLockRuntime();
            if (reverseBrokenEntityKey != 0) {
                std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                TargetingDetail::RuntimePreAimTrackerStorage()
                    .NotifyReverseIntentBreak(reverseBrokenEntityKey, reverseBrokenActorGeneration);
            }
        }

        struct Stage1Candidate {
            Stage0Entity s0{};
            float effectiveSelectionFovDeg = 15.0f;
            float peekGateDeg = 0.0f;
            bool hasPreAimOrPeek = false;
            float intentAlignment = 0.0f;
            float intentConfidence = 0.0f;
            float dwellCommitment = 0.0f;
            float firePhaseCommitment = 0.0f;
            float visibilityPrior = 0.4f;
            TargetPipeline::UtilityBreakdown utility{};
        };
        std::vector<Stage1Candidate> stage1;
        stage1.reserve(stage0.size());

        const auto& planSelection = Config::View_aimPlan().selection;
        const int priorityMode = Config::View_aimbotPriority();
        const float fireCommitValue = TargetPipeline::ComputeFirePhaseCommitment(
            acqCtx.firePhase, acqCtx.firePhaseActive);

        for (auto& entry : stage0) {
            const c_entity& entity = entities[entry.index];
            if (reverseLockBroken)
                entry.isLocked = false;

            AimCandidateConfig candidateConfig(entity.HeroID);
            const float dynamicFovDeg = Config::ResolveDynamicAimFovForDistance(
                Config::View_Fov(), entry.coarseDistance);
            const float peekGateDeg = PreAim::ComputeDynamicPeekAngleDeg(
                entry.coarseDistance,
                dynamicFovDeg,
                projectileSpec.projectileSpeed,
                PreAim::DefaultPeekSpeedMps(entity.HeroID));
            bool isTrackedPreAim = false;
            bool hasPreAimOrPeek = false;
            {
                std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                isTrackedPreAim = TargetingDetail::RuntimePreAimTrackerStorage()
                    .IsTrackingEntity(entry.key, entity.actor_generation);
                hasPreAimOrPeek = isTrackedPreAim ||
                    TargetingDetail::RuntimeReactionDelayGateStorage()
                        .HasRecentPreAimOrPeek(entry.key, entity.actor_generation, now64);
            }
            float selectionFovDeg = dynamicFovDeg;
            float align = 0.0f;
            float conf = 0.0f;
            if (acqCtx.useIntentEngine) {
                std::lock_guard<std::mutex> lock(TargetingDetail::RuntimeIntentEngineMutex());
                const auto& intentEngine = TargetingDetail::RuntimeIntentEngineStorage();
                selectionFovDeg = intentEngine.EffectiveSelectionFovDeg(
                    dynamicFovDeg, hasPreAimOrPeek, peekGateDeg);
                if (entry.hasCoarseAngle) {
                    align = intentEngine.EvaluateAlignment(
                        entry.coarseErrYawDeg, entry.coarseErrPitchDeg, &conf);
                }
            }
            const bool applyTrackingExitMargin = trackingContinuityEnabled && entry.isLocked;
            selectionFovDeg = TrackingContinuityPolicy::EffectiveFovDeg(
                selectionFovDeg, applyTrackingExitMargin);

            const float dwellMs = entry.isLocked
                ? static_cast<float>(now - activeLock.lockStartedTick)
                : 0.0f;
            const float dwellC = entry.isLocked
                ? TargetPipeline::ComputeDwellCommitment(dwellMs)
                : 0.0f;
            const float fireC = entry.isLocked ? fireCommitValue : 0.0f;

            // Cheap non-blocking visibility prior for Stage 1 ordering.
            float visPrior = isTrackedPreAim ? 0.30f : (entity.Vis ? 0.70f : 0.40f);
            if (hybridLiveWorldGate && fovContext.valid) {
                EngineTrace::Request probeReq{};
                probeReq.start = {fovContext.camera.X, fovContext.camera.Y, fovContext.camera.Z};
                probeReq.end = {entry.coarseAnchor.X, entry.coarseAnchor.Y, entry.coarseAnchor.Z};
                probeReq.mask = worldRayMask;
                probeReq.sceneEpoch = RayRuntime::SceneEpoch();
                const int priorBone = entry.isLocked && activeLock.boneId != Plexies20260609::kUnusedRenderSkeletonBone
                    ? activeLock.boneId : BONE_HEAD;
                if (const auto cached = RayRuntime::Broker().Latest(
                        probeReq, frame.sequence, entity.connection_epoch, probeReq.sceneEpoch,
                        entry.key, entity.actor_generation, priorBone, 100)) {
                    if (cached->world == RayDecisionStatus::Clear)
                        visPrior = 1.0f;
                    else if (cached->world == RayDecisionStatus::Occluded)
                        visPrior = 0.30f;
                }
            }

            TargetPipeline::UtilityInput uIn{};
            uIn.priorityMode = priorityMode;
            uIn.angleWeight = planSelection.angleWeight;
            uIn.distanceWeight = planSelection.distanceWeight;
            uIn.healthWeight = planSelection.healthWeight;
            uIn.angleDeg = entry.coarseAngleDeg;
            uIn.effectiveFovDeg = (std::max)(selectionFovDeg, commonFov);
            uIn.distance = entry.coarseDistance;
            uIn.maxDistance = commonDistance;
            uIn.hp = entity.PlayerHealth;
            uIn.maxHp = entity.PlayerHealthMax;
            uIn.isCurrentLocked = entry.isLocked;
            uIn.dwellCommitment = dwellC;
            uIn.firePhaseCommitment = fireC;
            uIn.retargetHysteresis = lockPolicy.retargetHysteresis;
            uIn.intentActiveDrag = acqCtx.useIntentEngine && intentSnapshot.activeDrag;
            uIn.intentAlignment = align;
            uIn.intentConfidence = conf;
            uIn.visibilityPrior = visPrior;
            uIn.planPreferred = candidateConfig.Preferred(entity, entry.coarseDistance, entry.coarseAngleDeg);
            uIn.roleUtility = evaluateRoleUtility(
                entity, entry.coarseAngleDeg, uIn.effectiveFovDeg, entry.coarseDistance);

            const TargetPipeline::UtilityBreakdown breakdown =
                TargetPipeline::ComputeCandidateUtility(uIn);
            if (breakdown.hardBlockedByIntent)
                continue;

            stage1.push_back(Stage1Candidate{
                entry, selectionFovDeg, peekGateDeg, hasPreAimOrPeek,
                align, conf, dwellC, fireC, visPrior, breakdown
            });
        }

        std::sort(stage1.begin(), stage1.end(), [](const Stage1Candidate& a, const Stage1Candidate& b) {
            return TargetPipeline::UtilityBetter(a.utility.total, a.s0.key, b.utility.total, b.s0.key);
        });

        // Unconditionally bound Stage 2 work per tick to Top-K non-locked candidates (plus
        // always evaluating the active locked candidate), while rotating a fallback cursor
        // across iterations whenever all evaluated candidates fail so no target is starved
        // and the 4-entry RayQueryBroker pending queue is never thrashed (R5 fix).
        const size_t topK = (std::max)(acqCtx.stage1TopK, static_cast<size_t>(3));
        static std::atomic<size_t> s_stage2FallbackCursor{0};
        std::vector<size_t> stage2Order;
        stage2Order.reserve((std::min)(stage1.size(), topK + 1));
        size_t lockedStage1Pos = stage1.size();
        for (size_t idx = 0; idx < stage1.size(); ++idx) {
            if (stage1[idx].s0.isLocked) {
                lockedStage1Pos = idx;
                stage2Order.push_back(idx);
                break;
            }
        }
        const size_t stage1Count = stage1.size();
        const size_t startCursor = stage1Count > 0 ? (s_stage2FallbackCursor.load(std::memory_order_relaxed) % stage1Count) : 0;
        size_t nonLockedBudgetUsed = 0;
        for (size_t offset = 0; offset < stage1Count && nonLockedBudgetUsed < topK; ++offset) {
            const size_t idx = (startCursor + offset) % stage1Count;
            if (idx == lockedStage1Pos)
                continue;
            stage2Order.push_back(idx);
            ++nonLockedBudgetUsed;
        }

        // =====================================================================
        // Stage 2: Part Solver (AimPriority x HitAcceptance) + Three-Tier
        //          Admission (Engageable / PreAim / Ignored) + 6-Slot Batch Plan
        // =====================================================================
        TargetSolution bestEngageable{};
        TargetSolution bestPreAim{};
        TargetSolution lockedPreAimSolution{};
        bool bestEngageableViaVacuumGrace = false;
        bool lockedViaVacuumGrace = false;
        bool preAimHardReleasedLock = false;
        uint64_t activePreAimKey = 0;
        uint64_t activePreAimGen = 0;
        bool activePreAimSeenInStage2 = false;
        {
            std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
            const auto& preTracker = TargetingDetail::RuntimePreAimTrackerStorage();
            if (preTracker.Active()) {
                activePreAimKey = preTracker.EntityKey();
                activePreAimGen = preTracker.ActorGeneration();
            }
        }
        float bestPreAimFovBaseDeg = Config::View_Fov();
        float lockedPreAimFovBaseDeg = Config::View_Fov();
        TrajectoryRay::RayBatchPlanInput batchInput{};
        batchInput.barrierPolicy = slotBarrierPolicy;
        static std::atomic<uint32_t> s_rayBatchRotationCursor{0};
        batchInput.rotationCursor = s_rayBatchRotationCursor.fetch_add(1, std::memory_order_relaxed);
        bool top1FallbackHasUnresolved = false;
        bool top2FallbackHasUnresolved = false;
        bool top1WorldClearForBatch = false;
        const uint64_t liveRayScene = hybridLiveWorldGate ? RayRuntime::SceneEpoch() : 0;

        auto makeBrokerQuery = [&](const c_entity& ent,
                                   uint64_t entKey,
                                   int bId,
                                   const Vector3& segStart,
                                   const Vector3& segEnd,
                                   RaySegmentKind segKind,
                                   uint8_t segIdx,
                                   uint64_t maskOverride = 0) {
            EngineTrace::RayQueryBroker::Query q{};
            q.request.start = {segStart.X, segStart.Y, segStart.Z};
            q.request.end = {segEnd.X, segEnd.Y, segEnd.Z};
            q.request.mask = maskOverride != 0 ? maskOverride : worldRayMask;
            q.request.sceneEpoch = liveRayScene;
            q.frame = frame.sequence;
            q.connectionEpoch = ent.connection_epoch;
            q.sceneEpoch = liveRayScene;
            q.entityKey = entKey;
            q.actorGeneration = ent.actor_generation;
            q.boneId = bId;
            q.sampleTick = ent.native_hitboxes.tick;
            q.segmentKind = segKind;
            q.segmentIndex = segIdx;
            return q;
        };

        // Fills a RayChordGroup separating urgent (missing/stale) segments from already-fresh
        // refresh segments so PlanRayBatch never starves GroundClip or secondary candidates (F2).
        auto populateChordGroup = [&](TrajectoryRay::RayChordGroup& dst,
                                      const c_entity& ent,
                                      uint64_t entKey,
                                      int bId,
                                      const TrajectoryRay::ParabolicChordPlan& plan,
                                      uint32_t maxAgeMs,
                                      bool includeFreshForRefresh) {
            dst.Clear();
            const uint8_t nSeg = (std::min<uint8_t>)(
                plan.segmentCount,
                static_cast<uint8_t>(TrajectoryRay::kMaxParabolicSegments));
            const RaySegmentKind segKind = nSeg > 1
                ? RaySegmentKind::ParabolicChord : RaySegmentKind::Direct;
            for (uint8_t s = 0; s < nSeg; ++s) {
                const auto& [segStart, segEnd] = plan.segments[s];
                EngineTrace::Request req{};
                req.start = {segStart.X, segStart.Y, segStart.Z};
                req.end = {segEnd.X, segEnd.Y, segEnd.Z};
                req.mask = worldRayMask;
                req.sceneEpoch = liveRayScene;
                const auto cached = RayRuntime::Broker().Latest(
                    req, frame.sequence, ent.connection_epoch, liveRayScene,
                    entKey, ent.actor_generation, bId, maxAgeMs,
                    EngineTrace::RayQueryBroker::kDefaultMaxEndpointDriftMeters,
                    segKind, s);
                const bool isUnresolved = !cached.has_value() || cached->IsObservationVacuum();
                if (isUnresolved) {
                    dst.PushUrgent(makeBrokerQuery(ent, entKey, bId, segStart, segEnd, segKind, s));
                } else if (includeFreshForRefresh) {
                    dst.PushRefresh(makeBrokerQuery(ent, entKey, bId, segStart, segEnd, segKind, s));
                }
            }
        };

        size_t stage2RankIdx = 0;
        for (const size_t stage1Idx : stage2Order) {
            const auto& ranked = stage1[stage1Idx];
            const bool currentLockedCandidate = ranked.s0.isLocked;
            const size_t currentRank = stage2RankIdx++;

            const size_t i = ranked.s0.index;
            const c_entity& entity = entities[i];
            const uint64_t candidateKey = ranked.s0.key;
            if (activePreAimKey != 0 && candidateKey == activePreAimKey &&
                (activePreAimGen == 0 || entity.actor_generation == activePreAimGen)) {
                activePreAimSeenInStage2 = true;
            }
            const bool applyTrackingExitMargin = trackingContinuityEnabled && currentLockedCandidate;
            AimCandidateConfig candidateConfig(entity.HeroID);

            const bool contourFallback = contour.enabled && !entity.Vis;
            const bool nativeBlockedVis = !contourFallback && ignoreInvisible && !hybridLiveWorldGate &&
                !entity.Vis && !TargetingDetail::CandidateCanBypassTrace(lockPolicy, activeLock, candidateKey);
            if (contourFallback && !contour.Visible(i)) {
                std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                TargetingDetail::RuntimeReactionDelayGateStorage()
                    .ObserveOccludedOrPreAim(candidateKey, entity.actor_generation, now64, false);
                continue;
            }
            ++selectableCandidates;

            const SkeletonBoneMask rawCandidateMask = Config::slotTargetRules.active && !entity.Team
                ? Config::slotTargetRules.allyBoneMask
                : Config::View_aimbotBoneMask();
            const int boneSelectionMode = Config::slotTargetRules.active
                ? Config::slotTargetRules.boneSelectionMode : Config::aimBoneSelectionMode;
            const int preferredBone = Config::slotTargetRules.active
                ? Config::slotTargetRules.preferredBone : Config::aimPreferredBone;
            const AimPriority aimPriority = ClampAimPriority(Config::slotTargetRules.active
                ? Config::slotTargetRules.aimPriority : Config::aimPriority);
            const HitAcceptance hitAcceptance = ClampHitAcceptance(Config::slotTargetRules.active
                ? Config::slotTargetRules.hitAcceptance : Config::aimHitAcceptance);
            const bool allowLimbExecute = (aimPriority == AimPriority::HeadThenBody) &&
                entity.Team && std::isfinite(entity.PlayerHealth) &&
                entity.PlayerHealth > 0.0f && entity.PlayerHealth <= 45.0f;

            const TargetPipeline::GuidanceBoneLadder ladder = TargetPipeline::BuildGuidanceBoneLadder(
                aimPriority,
                rawCandidateMask,
                ranked.s0.coarseDistance,
                Config::View_aimbotMaxHead(),
                allowLimbExecute);
            if (ladder.Empty())
                continue;

            const int retainedBoneId = currentLockedCandidate
                ? activeLock.boneId
                : Plexies20260609::kUnusedRenderSkeletonBone;
            const bool preferExplicitCritical =
                aimPriority == AimPriority::HeadOnly || aimPriority == AimPriority::HeadThenBody;
            // Enforce the guidance ladder for BOTH standard and contour fallback (Finding #6):
            // BodyCenter and HeadThenBody torso cutoff apply to contour as well.
            const size_t attemptCount = ladder.nearestMode ? 1 : ladder.count;
            // Tighten cache maxAge for fast-moving targets (§6.2: 50-65 ms vs 100 ms default).
            const uint32_t candidateRayMaxAgeMs =
                TargetPipeline::ResolveCandidateRayMaxAgeMs(entity.velocity.Size());

            bool isTrackedPreAimSnapshot = false;
            bool isSuppressedPreAimSnapshot = false;
            {
                std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                const auto& preAim = TargetingDetail::RuntimePreAimTrackerStorage();
                isTrackedPreAimSnapshot = preAim.IsTrackingEntity(candidateKey, entity.actor_generation);
                isSuppressedPreAimSnapshot = preAim.IsSuppressedForEntity(candidateKey, entity.actor_generation);
            }

            bool foundEngageableForCandidate = false;
            bool sawOccludedForCandidate = false;
            bool hasPreAimSeed = false;
            int preAimSeedBoneId = BONE_HEAD;
            Vector3 preAimSeedRoot{};
            Vector3 preAimSeedAim{};
            float preAimSeedDistance = ranked.s0.coarseDistance;
            float preAimSeedAngleDeg = ranked.s0.coarseAngleDeg;
            float preAimSeedFovBaseDeg = ranked.effectiveSelectionFovDeg;
            float preAimSeedPeekGateDeg = ranked.peekGateDeg;
            RayDecision preAimSeedRay{};
            TraceHitKind preAimSeedHitKind = TraceHitKind::World;
            uint8_t preAimSeedSegments = 1;

            for (size_t step = 0; step < attemptCount; ++step) {
                int candidateBoneId = BONE_HEAD;
                bool usedExplicitCritical = false;
                Vector3 rootPosition{};
                Contour::Observation contourObservation{};

                if (contourFallback) {
                    // Exposure-First within the permitted guidance-ladder group (Finding #6).
                    const SkeletonBoneMask stepMask = ladder.nearestMode
                        ? ladder.nearestMask
                        : ladder.groups[step];
                    contourObservation = contour.ResolveMask(i, stepMask);
                    if (!contourObservation.evidence.exposed || !contourObservation.evidence.fresh)
                        continue;
                    candidateBoneId = contourObservation.boneId;
                    rootPosition = contourObservation.aimWorld;
                } else if (ladder.nearestMode) {
                    if (!TargetingDetail::TrySelectClosestSelectedSkeletonPoint(
                            entity,
                            ladder.nearestMask,
                            fovContext,
                            aimViewMatrix,
                            CrossHair,
                            candidateBoneId,
                            rootPosition,
                            AimBoneToSkeletonBoneId(preferredBone),
                            boneSelectionMode == 1 && entity.velocity.Size() > 2.0f ? 1 : 2,
                            nullptr,
                            retainedBoneId)) {
                        break;
                    }
                } else {
                    if (!TargetingDetail::TryResolveBoneGroupPoint(
                            entity,
                            ladder.groups[step],
                            preferExplicitCritical,
                            fovContext,
                            aimViewMatrix,
                            CrossHair,
                            candidateBoneId,
                            rootPosition,
                            retainedBoneId,
                            &usedExplicitCritical)) {
                        continue;
                    }
                }

                LeadPredictionResult lead = TargetingDetail::ResolveLeadPrediction(
                    entity,
                    rootPosition,
                    projectileSpec,
                    !contourFallback && ResolvePredictionEnabled(
                        ClampPredictionOverride(Config::View_aimbotPredictionMode()),
                        weaponSpec, predit),
                    false);
                const float fovEntryHorizonMs =
                    Config::ClampFovEntryPredictionMs(Config::View_aimbotFovEntryPredictionMs());

                // §6.3 Slot 2 & F5/F6 fix: Closed-loop GroundClip + Ballistic Intercept solver.
                // Clips airborne pre-fire point and downward velocity against terrain at the ACTUAL
                // solved flight time BEFORE building chord rays and NativeHitWindowInput.
                if (hybridLiveWorldGate && liveRayScene != 0 && !contourFallback &&
                    lead.predictionEnabled &&
                    std::fabs(lead.targetVelocity.Y) > TrajectoryRay::kAirborneMinVerticalSpeedMps) {
                    const float gravityAccel = projectileSpec.gravity ? projectileSpec.gravityAcceleration : 0.0f;
                    float initialFlightSec = projectileSpec.projectileSpeed > 0.0f
                        ? (fovContext.camera.DistTo(rootPosition) / projectileSpec.projectileSpeed)
                        : 0.0f;
                    if (projectileSpec.projectileSpeed > 0.0f) {
                        const BallisticIntercept initialIntercept = SolveBallisticIntercept(
                            fovContext.camera, lead.preFireAimPoint, lead.targetVelocity,
                            projectileSpec.projectileSpeed, gravityAccel);
                        if (initialIntercept.solved && std::isfinite(initialIntercept.flightSeconds) &&
                            initialIntercept.flightSeconds > 0.0f) {
                            initialFlightSec = initialIntercept.flightSeconds;
                        }
                    }
                    const float airHorizonSec = (lead.timing.targetExtrapolationMs / 1000.0f) + initialFlightSec;
                    Vector3 gcStart{};
                    Vector3 gcEnd{};
                    if (TrajectoryRay::BuildAirborneGroundClipRay(
                            entity.pos, lead.targetVelocity, airHorizonSec, gcStart, gcEnd)) {
                        const auto gcQuery = makeBrokerQuery(
                            entity, candidateKey, candidateBoneId, gcStart, gcEnd,
                            RaySegmentKind::GroundClip, 0);
                        const auto gcCached = RayRuntime::Broker().Latest(
                            gcQuery.request, frame.sequence, entity.connection_epoch, liveRayScene,
                            candidateKey, entity.actor_generation, candidateBoneId, candidateRayMaxAgeMs,
                            EngineTrace::RayQueryBroker::kDefaultMaxEndpointDriftMeters,
                            RaySegmentKind::GroundClip, 0);
                        if (currentRank == 0 && !batchInput.top1GroundClip.has_value()) {
                            batchInput.top1GroundClip = gcQuery;
                            batchInput.top1GroundClipUrgent =
                                !gcCached.has_value() || gcCached->IsObservationVacuum();
                        }
                        if (gcCached.has_value()) {
                            BallisticIntercept clippedIntercept{};
                            if (TrajectoryRay::ApplyAirborneGroundClipAndSolveIntercept(
                                    fovContext.camera, rootPosition, entity.pos, gcStart, gcEnd,
                                    *gcCached, projectileSpec.projectileSpeed, gravityAccel,
                                    lead.preFireAimPoint, lead.targetVelocity, &clippedIntercept)) {
                                if (projectileSpec.projectileSpeed > 0.0f) {
                                    if (!clippedIntercept.solved) {
                                        // F6 fix: clipped motion made the ballistic shot unreachable;
                                        // reject this candidate step rather than using an obsolete
                                        // intercept or straight-ray fallback.
                                        continue;
                                    }
                                    lead.finalAimPoint = CompensateProjectileLaunchBias(
                                        fovContext.camera, clippedIntercept.aimPoint,
                                        projectileSpec.launchDirectionUpBias);
                                } else {
                                    lead.finalAimPoint = lead.preFireAimPoint;
                                }
                            }
                        }
                    }
                }

                const Vector3 resolvedAimPoint = contourFallback ? rootPosition : lead.finalAimPoint;

                // §6.4: Parabolic arch-height gate (h_max = 1/8 g T^2 > 0.15m -> 2-3 chord segments
                // along the physical ballistic arc without launchUpBias distortion).
                TrajectoryRay::ParabolicChordPlan chordPlan{};
                if (!contourFallback && projectileSpec.projectileSpeed > 0.0f &&
                    projectileSpec.gravity && projectileSpec.gravityAcceleration > 0.0f) {
                    chordPlan = TrajectoryRay::BuildParabolicChordPlan(
                        fovContext.camera, lead.preFireAimPoint, lead.targetVelocity,
                        projectileSpec.projectileSpeed, projectileSpec.gravityAcceleration,
                        resolvedAimPoint);
                } else {
                    chordPlan.segmentCount = 1;
                    chordPlan.segmentKind = RaySegmentKind::Direct;
                    chordPlan.impactPoint = resolvedAimPoint;
                    chordPlan.segments[0] = {fovContext.camera, resolvedAimPoint};
                }

                const float boneDistance = fovContext.valid
                    ? fovContext.camera.DistTo(rootPosition)
                    : ranked.s0.coarseDistance;
                const float rawDynamicFovDeg = Config::ResolveDynamicAimFovForDistance(
                    Config::View_Fov(), boneDistance);
                const float bonePeekGateDeg = PreAim::ComputeDynamicPeekAngleDeg(
                    boneDistance, rawDynamicFovDeg, projectileSpec.projectileSpeed,
                    PreAim::DefaultPeekSpeedMps(entity.HeroID));
                const float boneExitGateDeg = PreAim::ComputePeekExitAngleDeg(
                    bonePeekGateDeg, rawDynamicFovDeg);
                const float boneAngleDeg = fovContext.valid
                    ? TargetingDetail::FovScoreDeg(fovContext, resolvedAimPoint)
                    : ranked.s0.coarseAngleDeg;

                if (!hasPreAimSeed) {
                    hasPreAimSeed = true;
                    preAimSeedBoneId = candidateBoneId;
                    preAimSeedRoot = rootPosition;
                    preAimSeedAim = resolvedAimPoint;
                    preAimSeedDistance = boneDistance;
                    preAimSeedAngleDeg = boneAngleDeg;
                    preAimSeedFovBaseDeg = rawDynamicFovDeg;
                    preAimSeedPeekGateDeg = bonePeekGateDeg;
                    preAimSeedSegments = chordPlan.segmentCount;
                }

                // In NativeOnly with ignoreInvisible && !Vis, entity cannot be Engageable;
                // fall through after seeding PreAim so corridor peek hold still functions.
                if (nativeBlockedVis) {
                    sawOccludedForCandidate = true;
                    preAimSeedRay.world = RayDecisionStatus::Occluded;
                    break;
                }

                const Vector3 fovEntryAimPoint = resolvedAimPoint +
                    lead.targetVelocity * (fovEntryHorizonMs / 1000.0f);

                FovGeometry::PredictedEntryEvaluation fovEvaluation{};
                float stanceAdjustedFovDeg = rawDynamicFovDeg;
                if (acqCtx.useIntentEngine) {
                    std::lock_guard<std::mutex> lock(TargetingDetail::RuntimeIntentEngineMutex());
                    stanceAdjustedFovDeg = TargetingDetail::RuntimeIntentEngineStorage()
                        .EffectiveSelectionFovDeg(rawDynamicFovDeg, ranked.hasPreAimOrPeek, bonePeekGateDeg);
                }
                const float candidateFovDeg = TrackingContinuityPolicy::EffectiveFovDeg(
                    stanceAdjustedFovDeg, applyTrackingExitMargin);

                const bool passesEngageFov = FovGeometry::EvaluateCandidateFovWithPredictedEntry(
                    fovContext,
                    rootPosition,
                    resolvedAimPoint,
                    fovEntryAimPoint,
                    candidateFovDeg,
                    nullptr,
                    !contourFallback && Config::View_aimbotPredictFovEntry(),
                    Config::ClampFovEntryMaxOutsideDeg(Config::View_aimbotFovEntryMaxOutsideDeg()),
                    fovEvaluation);

                // Finding #7 fix: even if IdleHold shrunk candidateFovDeg below theta_peek(d),
                // evaluate non-blocking world occlusion when step 0 lies inside PreAim's peek window
                // so first-time Hybrid PreAim admission works during IdleHold!
                const float allowedPreAimWindowDeg = isTrackedPreAimSnapshot
                    ? boneExitGateDeg : bonePeekGateDeg;
                const bool withinPreAimWindow = !contourFallback && ignoreInvisible &&
                    std::isfinite(boneAngleDeg) && boneAngleDeg <= allowedPreAimWindowDeg &&
                    TargetingDetail::DistancePassesAimFilter(boneDistance, applyTrackingExitMargin);

                if (!passesEngageFov && !(step == 0 && withinPreAimWindow)) {
                    // Schedule primary/secondary warmup rays for step 0 if still within active dynamic FOV.
                    if (hybridLiveWorldGate && liveRayScene != 0 && !contourFallback && step == 0 &&
                        std::isfinite(boneAngleDeg) && boneAngleDeg <= rawDynamicFovDeg) {
                        if (currentRank == 0 && batchInput.top1Primary.count == 0) {
                            populateChordGroup(batchInput.top1Primary,
                                               entity, candidateKey, candidateBoneId, chordPlan,
                                               candidateRayMaxAgeMs, true);
                        } else if (currentRank == 1 && batchInput.top2Primary.count == 0) {
                            populateChordGroup(batchInput.top2Primary,
                                               entity, candidateKey, candidateBoneId, chordPlan,
                                               candidateRayMaxAgeMs, true);
                        } else if (currentRank == 2 && batchInput.top3Primary.count == 0) {
                            populateChordGroup(batchInput.top3Primary,
                                               entity, candidateKey, candidateBoneId, chordPlan,
                                               candidateRayMaxAgeMs, true);
                        }
                    }
                    continue;
                }

                const float distance = passesEngageFov ? fovEvaluation.distance : boneDistance;
                const float effectiveFovDeg = passesEngageFov ? fovEvaluation.effectiveFovDeg : candidateFovDeg;
                const float fovScoreDeg = passesEngageFov
                    ? (fovEvaluation.acceptedByPrediction
                        ? fovEvaluation.projectedScoreDeg
                        : fovEvaluation.currentScoreDeg)
                    : boneAngleDeg;
                if (!TargetingDetail::DistancePassesAimFilter(distance, applyTrackingExitMargin))
                    continue;

                NativeHitWindowInput nativeHitWindow{};
                RayDecision rayDecision{};
                if (!contourFallback) {
                    if (weaponSpec) {
                        nativeHitWindow = MakeNativeHitWindowInput(
                            lead, weaponSpec->aimClass, Config::View_hitbox());
                    }
                    rayDecision = MakeNativeRayDecision(
                        entity, candidateBoneId, fovContext.camera,
                        resolvedAimPoint - fovContext.camera,
                        resolvedAimPoint, nativeHitWindow, frame.sequence,
                        usedExplicitCritical, &chordPlan,
                        /*submitBrokerQuery=*/false, candidateRayMaxAgeMs);
                }

                // F2/F3 fix: schedule 6-slot batch AFTER reading cached verdicts so:
                // - Urgent (missing/stale) vs refresh chords are separated for starvation-free PlanRayBatch;
                // - Unresolved fallback bones (e.g. Chest after Neck is fresh Occluded) take priority
                //   over earlier blocked bones; and if a later fallback wins Engageable below, it
                //   overrides the fallback refresh slot so the winning bone stays continuously fresh.
                if (hybridLiveWorldGate && liveRayScene != 0 && !contourFallback) {
                    const bool stepUnresolved = rayDecision.IsObservationVacuum();
                    if (currentRank == 0) {
                        if (step == 0 && batchInput.top1Primary.count == 0) {
                            populateChordGroup(batchInput.top1Primary,
                                               entity, candidateKey, candidateBoneId, chordPlan,
                                               candidateRayMaxAgeMs, true);
                        } else if (step > 0) {
                            if (stepUnresolved && !top1FallbackHasUnresolved) {
                                populateChordGroup(batchInput.top1FallbackOrWarmup,
                                                   entity, candidateKey, candidateBoneId, chordPlan,
                                                   candidateRayMaxAgeMs, false);
                                if (batchInput.top1FallbackOrWarmup.count > 0)
                                    top1FallbackHasUnresolved = true;
                            } else if (batchInput.top1FallbackOrWarmup.count == 0) {
                                populateChordGroup(batchInput.top1FallbackOrWarmup,
                                                   entity, candidateKey, candidateBoneId, chordPlan,
                                                   candidateRayMaxAgeMs, true);
                            }
                        }
                    } else if (currentRank == 1) {
                        if (step == 0 && batchInput.top2Primary.count == 0) {
                            populateChordGroup(batchInput.top2Primary,
                                               entity, candidateKey, candidateBoneId, chordPlan,
                                               candidateRayMaxAgeMs, true);
                        } else if (step > 0) {
                            if (stepUnresolved && !top2FallbackHasUnresolved) {
                                populateChordGroup(batchInput.top2Fallback,
                                                   entity, candidateKey, candidateBoneId, chordPlan,
                                                   candidateRayMaxAgeMs, false);
                                if (batchInput.top2Fallback.count > 0)
                                    top2FallbackHasUnresolved = true;
                            } else if (batchInput.top2Fallback.count == 0) {
                                populateChordGroup(batchInput.top2Fallback,
                                                   entity, candidateKey, candidateBoneId, chordPlan,
                                                   candidateRayMaxAgeMs, true);
                            }
                        }
                    } else if (currentRank == 2 && step == 0 && batchInput.top3Primary.count == 0) {
                        populateChordGroup(batchInput.top3Primary,
                                           entity, candidateKey, candidateBoneId, chordPlan,
                                           candidateRayMaxAgeMs, true);
                    }
                }

                if (step == 0) {
                    preAimSeedRay = rayDecision;
                    preAimSeedDistance = distance;
                    preAimSeedAngleDeg = fovScoreDeg;
                    preAimSeedPeekGateDeg = PreAim::ComputeDynamicPeekAngleDeg(
                        distance, rawDynamicFovDeg, projectileSpec.projectileSpeed,
                        PreAim::DefaultPeekSpeedMps(entity.HeroID));
                }

                // If step 0 only entered this block via IdleHold PreAim window (failed shrunk Engage FOV),
                // record occlusion for PreAim and do NOT admit as Engageable!
                if (!passesEngageFov) {
                    if (rayDecision.world == RayDecisionStatus::Occluded ||
                        (hybridLiveWorldGate && rayDecision.IsObservationVacuum() && !entity.Vis)) {
                        sawOccludedForCandidate = true;
                    }
                    break;
                }

                bool allowedAsEngageable = RayDecisionAllowsSelection(
                    rayDecision, contourFallback, activeRayBackend, candidateRayMaxAgeMs);
                bool admittedViaVacuumGrace = false;
                TraceHitKind stepHitKind = rayDecision.world == RayDecisionStatus::Clear
                    ? TraceHitKind::None
                    : (rayDecision.world == RayDecisionStatus::Occluded ? TraceHitKind::World : TraceHitKind::Unknown);
                if (allowedAsEngageable && hybridLiveWorldGate && !contourFallback) {
                    std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                    TargetingDetail::RuntimeVacuumGraceTrackerStorage()
                        .RecordClear(candidateKey, entity.actor_generation, now64);
                    if (currentRank == 0 && rayDecision.WorldClear()) {
                        top1WorldClearForBatch = true;
                    }
                    // Phase 3 (§6.3 Slot 3, §8.1, §8.2, Decision #4): when VisibilityPolicy is
                    // Ray[World+Barrier] (WaitBarrierDrop + live-verified barrierMask != 0) and
                    // the world path is Clear, evaluate the dynamic barrier layer.
                    if (slotVisibilityPolicy == VisibilityPolicy::RayWorldAndBarrier &&
                        liveRayScene != 0 && rayDecision.WorldClear()) {
                        const auto barrierEval = TrajectoryRay::EvaluateBarrierLayerDecision(
                            RayRuntime::Broker(),
                            slotVisibilityPolicy,
                            slotBarrierPolicy,
                            barrierRayMask,
                            true,
                            fovContext.camera,
                            chordPlan.impactPoint,
                            frame.sequence,
                            entity.connection_epoch,
                            liveRayScene,
                            candidateKey,
                            entity.actor_generation,
                            candidateBoneId,
                            entity.native_hitboxes.tick,
                            candidateRayMaxAgeMs,
                            /*submitImmediately=*/false);
                        if (barrierEval.barrierActive) {
                            rayDecision.barrierCheckRequired = true;
                            rayDecision.barrier = barrierEval.barrierDecision.world;
                            rayDecision.barrierAgeMs = barrierEval.barrierDecision.ageMs;
                            if (currentRank == 0 && barrierEval.slot3Query.has_value() &&
                                (!batchInput.top1BarrierQuery.has_value() || barrierEval.slot3Urgent)) {
                                batchInput.top1BarrierQuery = barrierEval.slot3Query;
                                batchInput.top1BarrierUrgent = barrierEval.slot3Urgent;
                            }
                            if (barrierEval.barrierBlocked) {
                                allowedAsEngageable = false;
                                sawOccludedForCandidate = true;
                                stepHitKind = TraceHitKind::Barrier;
                                if (step == 0 || preAimSeedHitKind != TraceHitKind::Barrier) {
                                    preAimSeedBoneId = candidateBoneId;
                                    preAimSeedRoot = rootPosition;
                                    preAimSeedAim = resolvedAimPoint;
                                    preAimSeedDistance = distance;
                                    preAimSeedAngleDeg = fovScoreDeg;
                                    preAimSeedRay = rayDecision;
                                    preAimSeedHitKind = TraceHitKind::Barrier;
                                }
                            } else if (barrierEval.barrierClear) {
                                stepHitKind = TraceHitKind::None;
                            } else {
                                stepHitKind = TraceHitKind::Unknown;
                            }
                        }
                    }
                } else if (!allowedAsEngageable && hybridLiveWorldGate && !contourFallback &&
                           rayDecision.IsObservationVacuum()) {
                    // Decision #7: During 5-15ms ray vacuum after blink/move, if native Vis==true,
                    // allow <= 60ms tracking continuity grace while keeping ray.world in vacuum
                    // so auto-fire stays strictly blocked. If Vis==false, exit Engageable immediately!
                    uint32_t vacElapsedMs = UINT32_MAX;
                    {
                        std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                        vacElapsedMs = TargetingDetail::RuntimeVacuumGraceTrackerStorage()
                            .ObserveVacuum(candidateKey, entity.actor_generation, entity.Vis, now64);
                    }
                    if (RayDecisionAllowsTrackingGrace(
                            rayDecision, entity.Vis, contourFallback, activeRayBackend,
                            vacElapsedMs, RayDecision::kDefaultTrackingVacuumGraceMs, candidateRayMaxAgeMs)) {
                        allowedAsEngageable = true;
                        admittedViaVacuumGrace = true;
                    } else if (!entity.Vis) {
                        sawOccludedForCandidate = true;
                    }
                } else if (!allowedAsEngageable && rayDecision.world == RayDecisionStatus::Occluded) {
                    sawOccludedForCandidate = true;
                }

                if (!allowedAsEngageable) {
                    ++rayRejectedCandidates;
                    Diagnostics::Aim(
                        "target.primary.ray_reject index=%zu bone=%d vis=%d policy=%s visPolicy=%s actor=%s world=%s barrier=%s hitKind=%s staleReason=%s frac=%.3f segs=%u ageMs=%u latencyUs=%u",
                        i, candidateBoneId, entity.Vis ? 1 : 0,
                        RayDecisionPolicyName(activeRayBackend),
                        VisibilityPolicyName(slotVisibilityPolicy),
                        RayDecisionStatusName(rayDecision.actor),
                        RayDecisionStatusName(rayDecision.world),
                        RayDecisionStatusName(rayDecision.barrier),
                        TraceHitKindName(stepHitKind),
                        RayStaleReasonName(rayDecision.staleReason),
                        rayDecision.fraction,
                        static_cast<unsigned>(chordPlan.segmentCount),
                        rayDecision.ageMs, rayDecision.latencyUs);
                    // In Hybrid mode, if the top ladder bone is occluded (by world or partial barrier),
                    // try the next torso bone (§1.2, §2).
                    if (hybridLiveWorldGate && !contourFallback && !ladder.nearestMode &&
                        (rayDecision.world == RayDecisionStatus::Occluded ||
                         rayDecision.BarrierOccluded()) &&
                        step + 1 < attemptCount) {
                        continue;
                    }
                    break;
                }

                const bool stage2Visible = contourFallback
                    ? (contourObservation.evidence.exposed && contourObservation.evidence.fresh)
                    : (hybridLiveWorldGate
                        ? (rayDecision.WorldClear() || (admittedViaVacuumGrace && entity.Vis))
                        : TargetingDetail::SelectionVisible(entity));
                if (!candidateConfig.Passes(entity, distance, fovScoreDeg, stage2Visible))
                    continue;

                const bool preferred = candidateConfig.Preferred(entity, distance, fovScoreDeg);
                float boneErrYawDeg = ranked.s0.coarseErrYawDeg;
                float boneErrPitchDeg = ranked.s0.coarseErrPitchDeg;
                float boneAlign = ranked.intentAlignment;
                float boneConf = ranked.intentConfidence;
                if (acqCtx.useIntentEngine &&
                    TargetingDetail::ComputeAngularErrorDeg(
                        fovContext, fovEvaluation.acceptedAimPoint, boneErrYawDeg, boneErrPitchDeg)) {
                    std::lock_guard<std::mutex> lock(TargetingDetail::RuntimeIntentEngineMutex());
                    boneAlign = TargetingDetail::RuntimeIntentEngineStorage()
                        .EvaluateAlignment(boneErrYawDeg, boneErrPitchDeg, &boneConf);
                }

                TargetPipeline::UtilityInput finalUIn{};
                finalUIn.priorityMode = priorityMode;
                finalUIn.angleWeight = planSelection.angleWeight;
                finalUIn.distanceWeight = planSelection.distanceWeight;
                finalUIn.healthWeight = planSelection.healthWeight;
                finalUIn.angleDeg = fovScoreDeg;
                finalUIn.effectiveFovDeg = (std::max)(effectiveFovDeg, commonFov);
                finalUIn.distance = distance;
                finalUIn.maxDistance = commonDistance;
                finalUIn.hp = entity.PlayerHealth;
                finalUIn.maxHp = entity.PlayerHealthMax;
                finalUIn.isCurrentLocked = currentLockedCandidate;
                finalUIn.dwellCommitment = ranked.dwellCommitment;
                finalUIn.firePhaseCommitment = ranked.firePhaseCommitment;
                finalUIn.retargetHysteresis = lockPolicy.retargetHysteresis;
                finalUIn.intentActiveDrag = acqCtx.useIntentEngine && intentSnapshot.activeDrag;
                finalUIn.intentAlignment = boneAlign;
                finalUIn.intentConfidence = boneConf;
                finalUIn.visibilityPrior = rayDecision.WorldClear()
                    ? 1.0f : (contourFallback ? 0.85f : (entity.Vis ? 0.70f : 0.40f));
                finalUIn.planPreferred = preferred;
                finalUIn.roleUtility = evaluateRoleUtility(
                    entity, fovScoreDeg, finalUIn.effectiveFovDeg, distance);
                finalUIn.predictedFovEntry = fovEvaluation.acceptedByPrediction;

                const TargetPipeline::UtilityBreakdown finalUtility =
                    TargetPipeline::ComputeCandidateUtility(finalUIn);
                if (!std::isfinite(finalUtility.total) || finalUtility.hardBlockedByIntent)
                    break;

                if (currentLockedCandidate)
                    activeLockCandidateSelectable = true;

                Vector2 screenPt = CrossHair;
                aimViewMatrix.WorldToScreen(fovEvaluation.acceptedAimPoint, &screenPt, OW::ScreenSizeSnapshot());

                TargetSolution next{};
                next.valid = true;
                next.tier = VisibilityTier::Engageable;
                next.preAimPhase = PreAimPhase::None;
                next.preAimParkPointValid = false;
                next.targetReversalActive = currentLockedCandidate && lockedTargetReversalActive;
                next.targetReversalElapsedMs = currentLockedCandidate ? lockedReversalElapsedMs : 0.0f;
                next.entityIndex = static_cast<int>(i);
                next.entityKey = candidateKey;
                next.actorGeneration = entity.actor_generation;
                next.entitySnapshot = entity;
                next.aimPriority = aimPriority;
                next.hitAcceptance = hitAcceptance;
                next.guidanceBoneId = candidateBoneId;
                next.requireExplicitCritical = usedExplicitCritical;
                next.rawGuidancePoint = rootPosition;
                next.predictedGuidancePoint = resolvedAimPoint;
                next.acceptanceMask = TargetPipeline::ResolveAcceptanceMask(
                    hitAcceptance, rawCandidateMask, distance, Config::View_aimbotMaxHead(), candidateBoneId);
                next.worldClearSlotMask = rayDecision.WorldClear()
                    ? TargetPipeline::RenderSlotBitForBoneId(candidateBoneId)
                    : 0u;
                next.barrierClearSlotMask = rayDecision.BarrierClear(candidateRayMaxAgeMs)
                    ? TargetPipeline::RenderSlotBitForBoneId(candidateBoneId)
                    : 0u;
                // Under Hybrid world/barrier occlusion + AnyValidHitbox, populate per-render-slot
                // fresh WorldClear (and BarrierClear when WaitBarrierDrop is active) bits
                // non-blocking from TraceCache so off-guidance bones never fire through walls or shields.
                if (hybridLiveWorldGate && liveRayScene != 0 && !contourFallback &&
                    hitAcceptance == HitAcceptance::AnyValidHitbox &&
                    next.acceptanceMask != 0 && fovContext.valid) {
                    const bool checkOffGuidanceBarrier =
                        slotVisibilityPolicy == VisibilityPolicy::RayWorldAndBarrier &&
                        barrierRayMask != 0;
                    for (std::size_t slot = 0; slot < Plexies20260609::kSkeletonSlotCount; ++slot) {
                        const SkeletonBoneMask grpBit = kSkeletonBoneGroupForRenderSlot[slot];
                        const uint32_t slotBit = slot < 32 ? (1u << static_cast<uint32_t>(slot)) : 0u;
                        if ((next.acceptanceMask & grpBit) == 0 ||
                            ((next.worldClearSlotMask & slotBit) != 0 &&
                             (!checkOffGuidanceBarrier || (next.barrierClearSlotMask & slotBit) != 0)))
                            continue;
                        int slotBoneId = Plexies20260609::kUnusedRenderSkeletonBone;
                        Vector3 slotPoint{};
                        if (!TargetingDetail::TryResolveSkeletonBonePoint(entity, slot, slotBoneId, slotPoint))
                            continue;
                        const Vector3 predictedSlotPoint = TargetPipeline::ComputeAcceptanceRayEndpoint(
                            slotPoint, rootPosition, resolvedAimPoint, nativeHitWindow);
                        EngineTrace::Request slotReq{};
                        slotReq.start = {fovContext.camera.X, fovContext.camera.Y, fovContext.camera.Z};
                        slotReq.end = {predictedSlotPoint.X, predictedSlotPoint.Y, predictedSlotPoint.Z};
                        slotReq.mask = worldRayMask;
                        slotReq.sceneEpoch = liveRayScene;
                        if (const auto slotCached = RayRuntime::Broker().Latest(
                                slotReq, frame.sequence, entity.connection_epoch, liveRayScene,
                                candidateKey, entity.actor_generation, slotBoneId, candidateRayMaxAgeMs)) {
                            if (slotCached->world == RayDecisionStatus::Clear) {
                                next.worldClearSlotMask |= slotBit;
                                if (!checkOffGuidanceBarrier) {
                                    next.barrierClearSlotMask |= slotBit;
                                } else {
                                    EngineTrace::Request slotBarReq = slotReq;
                                    slotBarReq.mask = barrierRayMask;
                                    if (const auto slotBarCached = RayRuntime::Broker().Latest(
                                            slotBarReq, frame.sequence, entity.connection_epoch, liveRayScene,
                                            candidateKey, entity.actor_generation, slotBoneId, candidateRayMaxAgeMs,
                                            EngineTrace::RayQueryBroker::kDefaultMaxEndpointDriftMeters,
                                            RaySegmentKind::Barrier, 0)) {
                                        if (slotBarCached->world == RayDecisionStatus::Clear &&
                                            slotBarCached->Fresh(candidateRayMaxAgeMs)) {
                                            next.barrierClearSlotMask |= slotBit;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                next.effectiveHitWindow = ResolveEffectiveHitWindow(
                    entity,
                    next.guidanceBoneId,
                    weaponSpec,
                    Config::View_hitbox(),
                    Config::kLegacyDefaultHitboxRadius,
                    projectileSpec.projectileRadius);
                const Vector3 reverseLateralOffset =
                    (!contourFallback && !fovEvaluation.acceptedByPrediction && lead.predictionEnabled)
                        ? ComputeReverseLateralAimOffset(
                              fovContext.camera,
                              rootPosition,
                              resolvedAimPoint,
                              lead.targetVelocity,
                              next.effectiveHitWindow,
                              projectileSpec.projectileRadius,
                              Config::View_hitbox())
                        : Vector3{};
                next.guidanceAimPoint = fovEvaluation.acceptedAimPoint + reverseLateralOffset;
                if (reverseLateralOffset.Size() > 0.00001f)
                    aimViewMatrix.WorldToScreen(next.guidanceAimPoint, &screenPt, OW::ScreenSizeSnapshot());
                next.guidanceScreenPoint = screenPt;
                next.nativeHitWindow = nativeHitWindow;
                next.utilityScore = finalUtility.total;
                next.roleUtility = finalUIn.roleUtility;
                next.commitment = finalUtility.effectiveCommitment;
                next.intentAlignment = boneAlign;
                next.intentGain = acqCtx.useIntentEngine ? intentSnapshot.activeTargetGain : 1.0f;
                next.stance = acqCtx.useIntentEngine ? intentSnapshot.stance : InputStance::ActiveDrag;
                next.visibilityPolicy = slotVisibilityPolicy;
                next.barrierPolicy = slotBarrierPolicy;
                next.distance = distance;
                next.angularErrorDeg = fovScoreDeg;
                next.effectiveFovDeg = effectiveFovDeg;
                next.peekGateDeg = PreAim::ComputeDynamicPeekAngleDeg(
                    distance, rawDynamicFovDeg, projectileSpec.projectileSpeed,
                    PreAim::DefaultPeekSpeedMps(entity.HeroID));
                next.ray = rayDecision;
                next.hitKind = stepHitKind;
                next.trajectorySegments = chordPlan.segmentCount;
                next.contour = contourObservation;
                next.plan = candidateConfig.value;
                next.planPreferred = preferred;
                next.dynamicFov = Config::IsDynamicAimFovActive();
                next.dynamicFovPresetId = next.dynamicFov ? Config::View_aimbotDynamicFovPresetId() : -1;
                next.predictedFovEntry = fovEvaluation.acceptedByPrediction;
                next.fovEntryHorizonMs = fovEvaluation.acceptedByPrediction ? fovEntryHorizonMs : 0.0f;
                next.currentFovScore = fovEvaluation.currentScoreDeg;
                next.projectedFovScore = fovEvaluation.projectedScoreDeg;
                next.lockPolicy = lockPolicy;
                next.motion = TargetingDetail::EstimateMotionState(entity, screenPt);
                next.weaponSpec = weaponSpec;

                // F3 fix: when a later fallback bone (step > 0, e.g. Chest after Head & Neck are
                // fresh Occluded) wins Engageable, always prioritize refreshing the winning fallback
                // bone over earlier rejected fallback bones so its evidence never expires into gaps.
                if (hybridLiveWorldGate && liveRayScene != 0 && !contourFallback && step > 0) {
                    if (currentRank == 0) {
                        populateChordGroup(batchInput.top1FallbackOrWarmup,
                                           entity, candidateKey, candidateBoneId, chordPlan,
                                           candidateRayMaxAgeMs, true);
                    } else if (currentRank == 1) {
                        populateChordGroup(batchInput.top2Fallback,
                                           entity, candidateKey, candidateBoneId, chordPlan,
                                           candidateRayMaxAgeMs, true);
                    }
                }

                foundEngageableForCandidate = true;
                if (currentLockedCandidate) {
                    lockedSolution = next;
                    lockedViaVacuumGrace = admittedViaVacuumGrace;
                }
                if (!bestEngageable.valid ||
                    TargetPipeline::UtilityBetter(next.utilityScore, next.entityKey,
                                                  bestEngageable.utilityScore, bestEngageable.entityKey)) {
                    bestEngageable = std::move(next);
                    bestEngageableViaVacuumGrace = admittedViaVacuumGrace;
                }
                break; // First passing bone in the ladder satisfies this candidate.
            }

            // §5.1-§5.4: Occluded candidate evaluation for PreAim / ReactionDelayGate memory.
            // Note (Decision #4, §8.1): Barrier-blocked WaitBarrierDrop holds PreAim without
            // arming wall-peek ReactionDelayGate so shield drop re-engages immediately.
            if (!foundEngageableForCandidate && sawOccludedForCandidate) {
                if (preAimSeedHitKind != TraceHitKind::Barrier) {
                    std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                    TargetingDetail::RuntimeReactionDelayGateStorage()
                        .ObserveOccludedOrPreAim(candidateKey, entity.actor_generation, now64, false);
                }
                const float exitGateDeg = PreAim::ComputePeekExitAngleDeg(
                    preAimSeedPeekGateDeg, preAimSeedFovBaseDeg);
                if (isSuppressedPreAimSnapshot && std::isfinite(preAimSeedAngleDeg) &&
                    preAimSeedAngleDeg > exitGateDeg) {
                    std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                    TargetingDetail::RuntimePreAimTrackerStorage()
                        .ObserveOutsideWindow(candidateKey, entity.actor_generation);
                    isSuppressedPreAimSnapshot = false;
                }
                // F7 fix: evaluate AngularExit for the currently tracked PreAim actor BEFORE
                // rejecting it from PreAim admission so forced lock clearing always propagates.
                if (isTrackedPreAimSnapshot && std::isfinite(preAimSeedAngleDeg) &&
                    preAimSeedAngleDeg > exitGateDeg) {
                    bool releasedAngular = false;
                    {
                        std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                        releasedAngular = TargetingDetail::RuntimePreAimTrackerStorage()
                            .ReleaseAngularExit(candidateKey, entity.actor_generation);
                    }
                    if (releasedAngular && currentLockedCandidate) {
                        preAimHardReleasedLock = true;
                    }
                }
                // Finding #10 fix: enforce deferred spatial Only rules (Distance/Angle/Visible)
                // before admitting PreAim; Finding #8: honor released-episode suppression.
                if (ignoreInvisible && !contourFallback && hasPreAimSeed &&
                    !isSuppressedPreAimSnapshot &&
                    TargetingDetail::DistancePassesAimFilter(preAimSeedDistance, applyTrackingExitMargin) &&
                    candidateConfig.Passes(entity, preAimSeedDistance, preAimSeedAngleDeg, false)) {
                    const float allowedGateDeg = isTrackedPreAimSnapshot ? exitGateDeg : preAimSeedPeekGateDeg;
                    if (std::isfinite(preAimSeedAngleDeg) && preAimSeedAngleDeg <= allowedGateDeg) {
                        TargetPipeline::UtilityInput preUIn{};
                        preUIn.priorityMode = priorityMode;
                        preUIn.angleWeight = planSelection.angleWeight;
                        preUIn.distanceWeight = planSelection.distanceWeight;
                        preUIn.healthWeight = planSelection.healthWeight;
                        preUIn.angleDeg = preAimSeedAngleDeg;
                        preUIn.effectiveFovDeg = (std::max)(preAimSeedFovBaseDeg, commonFov);
                        preUIn.distance = preAimSeedDistance;
                        preUIn.maxDistance = commonDistance;
                        preUIn.hp = entity.PlayerHealth;
                        preUIn.maxHp = entity.PlayerHealthMax;
                        preUIn.isCurrentLocked = currentLockedCandidate;
                        preUIn.dwellCommitment = ranked.dwellCommitment;
                        preUIn.firePhaseCommitment = ranked.firePhaseCommitment;
                        preUIn.retargetHysteresis = lockPolicy.retargetHysteresis;
                        preUIn.intentActiveDrag = acqCtx.useIntentEngine && intentSnapshot.activeDrag;
                        preUIn.intentAlignment = ranked.intentAlignment;
                        preUIn.intentConfidence = ranked.intentConfidence;
                        preUIn.visibilityPrior = 0.30f;  // §4.2 PreAim visibility prior
                        preUIn.planPreferred = candidateConfig.Preferred(entity, preAimSeedDistance, preAimSeedAngleDeg);
                        preUIn.roleUtility = evaluateRoleUtility(
                            entity, preAimSeedAngleDeg, preUIn.effectiveFovDeg, preAimSeedDistance);
                        const auto preUtility = TargetPipeline::ComputeCandidateUtility(preUIn);
                        if (std::isfinite(preUtility.total) && !preUtility.hardBlockedByIntent) {
                            Vector2 screenPt = CrossHair;
                            aimViewMatrix.WorldToScreen(preAimSeedAim, &screenPt, OW::ScreenSizeSnapshot());
                            TargetSolution preSol{};
                            preSol.valid = true;
                            preSol.tier = VisibilityTier::PreAim;
                            preSol.preAimPhase = PreAimPhase::Parked;
                            preSol.preAimParkPointValid = false;
                            preSol.entityIndex = static_cast<int>(i);
                            preSol.entityKey = candidateKey;
                            preSol.actorGeneration = entity.actor_generation;
                            preSol.entitySnapshot = entity;
                            preSol.aimPriority = aimPriority;
                            preSol.hitAcceptance = hitAcceptance;
                            preSol.guidanceBoneId = preAimSeedBoneId;
                            preSol.rawGuidancePoint = preAimSeedRoot;
                            preSol.predictedGuidancePoint = preAimSeedAim;
                            preSol.guidanceAimPoint = preAimSeedAim;
                            preSol.guidanceScreenPoint = screenPt;
                            preSol.utilityScore = preUtility.total;
                            preSol.roleUtility = preUIn.roleUtility;
                            preSol.commitment = preUtility.effectiveCommitment;
                            preSol.intentAlignment = ranked.intentAlignment;
                            preSol.intentGain = acqCtx.useIntentEngine ? intentSnapshot.activeTargetGain : 1.0f;
                            preSol.stance = acqCtx.useIntentEngine ? intentSnapshot.stance : InputStance::ActiveDrag;
                            preSol.visibilityPolicy = slotVisibilityPolicy;
                            preSol.barrierPolicy = slotBarrierPolicy;
                            preSol.distance = preAimSeedDistance;
                            preSol.angularErrorDeg = preAimSeedAngleDeg;
                            preSol.effectiveFovDeg = preAimSeedFovBaseDeg;
                            preSol.peekGateDeg = preAimSeedPeekGateDeg;
                            preSol.ray = preAimSeedRay;
                            preSol.hitKind = preAimSeedHitKind;
                            preSol.trajectorySegments = preAimSeedSegments;
                            preSol.plan = candidateConfig.value;
                            preSol.planPreferred = preUIn.planPreferred;
                            preSol.lockPolicy = lockPolicy;
                            preSol.motion = TargetingDetail::EstimateMotionState(entity, screenPt);
                            preSol.weaponSpec = weaponSpec;

                            // Finding #15 fix: preserve locked PreAim separately so an unlocked
                            // PreAim with higher score cannot displace committed PreAim during arbitration.
                            if (currentLockedCandidate) {
                                lockedPreAimSolution = preSol;
                                lockedPreAimFovBaseDeg = preAimSeedFovBaseDeg;
                                activeLockCandidateSelectable = true;
                            }
                            if (!bestPreAim.valid ||
                                TargetPipeline::UtilityBetter(preSol.utilityScore, preSol.entityKey,
                                                              bestPreAim.utilityScore, bestPreAim.entityKey)) {
                                bestPreAim = std::move(preSol);
                                bestPreAimFovBaseDeg = preAimSeedFovBaseDeg;
                            }
                        }
                    }
                }
            } else if (!foundEngageableForCandidate && hasPreAimSeed && std::isfinite(preAimSeedAngleDeg)) {
                // F7 fix: if the tracked or suppressed PreAim actor moved beyond theta_exit(d),
                // step 0 exited before MakeNativeRayDecision (so sawOccludedForCandidate remained false).
                // Still propagate AngularExit / ObserveOutsideWindow cleanly here.
                const float exitGateDeg = PreAim::ComputePeekExitAngleDeg(
                    preAimSeedPeekGateDeg, preAimSeedFovBaseDeg);
                if (preAimSeedAngleDeg > exitGateDeg) {
                    if (isSuppressedPreAimSnapshot) {
                        std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                        TargetingDetail::RuntimePreAimTrackerStorage()
                            .ObserveOutsideWindow(candidateKey, entity.actor_generation);
                    }
                    if (isTrackedPreAimSnapshot) {
                        bool releasedAngular = false;
                        {
                            std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                            releasedAngular = TargetingDetail::RuntimePreAimTrackerStorage()
                                .ReleaseAngularExit(candidateKey, entity.actor_generation);
                        }
                        if (releasedAngular && currentLockedCandidate) {
                            preAimHardReleasedLock = true;
                        }
                    }
                }
            }
        }

        // F7 fix: if the active PreAim actor left Stage 0's angular envelope (or disappeared),
        // explicitly release AngularExit and clear lock occupancy instead of retaining via grace/OnDeath.
        if (activePreAimKey != 0 && !activePreAimSeenInStage2) {
            bool releasedAngular = false;
            {
                std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                releasedAngular = TargetingDetail::RuntimePreAimTrackerStorage()
                    .ReleaseAngularExit(activePreAimKey, activePreAimGen);
            }
            if (releasedAngular && activeLock.active && activeLock.entityKey == activePreAimKey) {
                preAimHardReleasedLock = true;
            }
        }

        // Arbitrate Engageable vs locked PreAim vs unlocked PreAim (§5.1, Finding #15):
        // - Committed locked PreAim competes directly against Engageable threats (and honors minLockMs).
        // - Unlocked PreAim never overrides an Engageable threat.
        float selectedPreAimFovBaseDeg = bestPreAimFovBaseDeg;
        bool selectedEngageableViaVacuumGrace = false;
        if (bestEngageable.valid) {
            best = bestEngageable;
            selectedEngageableViaVacuumGrace = bestEngageableViaVacuumGrace;
            if (!reverseLockBroken && lockedPreAimSolution.valid &&
                TargetPipeline::UtilityBetter(lockedPreAimSolution.utilityScore, lockedPreAimSolution.entityKey,
                                              bestEngageable.utilityScore, bestEngageable.entityKey)) {
                best = lockedPreAimSolution;
                selectedEngageableViaVacuumGrace = false;
                selectedPreAimFovBaseDeg = lockedPreAimFovBaseDeg;
            }
        } else if (!reverseLockBroken && lockedPreAimSolution.valid) {
            best = lockedPreAimSolution;
            selectedPreAimFovBaseDeg = lockedPreAimFovBaseDeg;
        } else if (bestPreAim.valid) {
            best = bestPreAim;
            selectedPreAimFovBaseDeg = bestPreAimFovBaseDeg;
        }

        // Explicit minLockMs respects configured lock duration while still allowing
        // reverse-intent break (redline #3) and high-value utility breakthrough (Decision #3).
        const TargetSolution& committedCandidate = lockedSolution.valid
            ? lockedSolution : lockedPreAimSolution;
        constexpr float kMinLockBreakthroughMargin = 0.55f;
        if (committedCandidate.valid && best.valid &&
            !reverseLockBroken &&
            TargetingDetail::CandidateBlockedByMinLock(lockPolicy, activeLock, best.entityKey, now) &&
            (best.utilityScore - committedCandidate.utilityScore) < kMinLockBreakthroughMargin) {
            best = committedCandidate;
            if (best.tier == VisibilityTier::PreAim) {
                selectedPreAimFovBaseDeg = lockedPreAimFovBaseDeg;
                selectedEngageableViaVacuumGrace = false;
            } else {
                selectedEngageableViaVacuumGrace = lockedViaVacuumGrace;
            }
        }

        // Finalize PreAim two-stage state machine (4 release exits + 6-ray EdgeFan borrow)
        // or Engageable ReactionDelayGate (§5.3, §5.4, §5.5, Decision #2, #5).
        bool peekEmergeTriggered = false;
        if (best.valid && best.tier == VisibilityTier::PreAim) {
            bool keepPreAim = false;
            bool needEdgeFanPass = false;
            bool isRefinePass = false;
            float fanLo = 0.0f;
            float fanHi = 0.0f;
            Vector3 fanAnchor{};
            Vector3 fanLateral{};
            float fanMaxWidth = 0.0f;
            bool hasParkPoint = false;
            Vector3 parkPoint{};
            PreAimPhase phase = PreAimPhase::Parked;
            // F1 fix: only pass reverseIntentBroken to the actor that actually triggered it,
            // never to a different PreAim candidate selected afterward in the same tick.
            const bool candidateReverseBroken = reverseLockBroken &&
                reverseBrokenEntityKey != 0 &&
                best.entityKey == reverseBrokenEntityKey &&
                (reverseBrokenActorGeneration == 0 || best.actorGeneration == reverseBrokenActorGeneration);
            {
                std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                auto& tracker = TargetingDetail::RuntimePreAimTrackerStorage();
                const bool barrierPreAim = best.hitKind == TraceHitKind::Barrier;
                keepPreAim = tracker.Update(
                    best.entityKey,
                    best.actorGeneration,
                    now64,
                    best.angularErrorDeg,
                    best.distance,
                    selectedPreAimFovBaseDeg,
                    best.peekGateDeg,
                    fovContext.camera,
                    best.rawGuidancePoint,
                    fovContext.forward,
                    best.motion.worldVelocity,
                    candidateReverseBroken,
                    !barrierPreAim && hybridLiveWorldGate && liveRayScene != 0);
                if (keepPreAim) {
                    if (!barrierPreAim) {
                        TargetingDetail::RuntimeReactionDelayGateStorage()
                            .ObserveOccludedOrPreAim(best.entityKey, best.actorGeneration, now64, true);
                    }
                    needEdgeFanPass = !barrierPreAim && hybridLiveWorldGate && liveRayScene != 0 &&
                        tracker.NeedsEdgeFanPass(isRefinePass, fanLo, fanHi);
                    fanAnchor = tracker.FanAnchorTargetPos();
                    fanLateral = tracker.LateralDir();
                    fanMaxWidth = tracker.MaxFanWidthMeters();
                    hasParkPoint = tracker.HasParkPoint();
                    parkPoint = tracker.ParkPoint();
                    phase = tracker.Phase();
                }
            }

            if (!keepPreAim) {
                if (activeLock.active && best.entityKey != 0 && best.entityKey == activeLock.entityKey)
                    preAimHardReleasedLock = true;
                best = bestEngageable.valid ? bestEngageable : TargetSolution{};
                selectedEngageableViaVacuumGrace = bestEngageable.valid ? bestEngageableViaVacuumGrace : false;
            } else {
                if (needEdgeFanPass && fovContext.valid) {
                    const auto fanRays = PreAim::BuildEdgeFanRays(
                        fovContext.camera, fanAnchor, fanLateral, fanLo, fanHi, isRefinePass);
                    std::array<std::optional<RayDecision>, PreAim::kEdgeFanRayCount> fanDecisions{};
                    for (size_t r = 0; r < PreAim::kEdgeFanRayCount; ++r) {
                        const auto q = makeBrokerQuery(
                            best.entitySnapshot, best.entityKey, best.guidanceBoneId,
                            fanRays[r].start, fanRays[r].end,
                            RaySegmentKind::EdgeFan, fanRays[r].segmentIndex);
                        fanDecisions[r] = RayRuntime::Broker().Latest(
                            q.request, frame.sequence, best.entitySnapshot.connection_epoch,
                            liveRayScene, best.entityKey, best.actorGeneration,
                            best.guidanceBoneId, 150,
                            EngineTrace::RayQueryBroker::kDefaultMaxEndpointDriftMeters,
                            RaySegmentKind::EdgeFan, fanRays[r].segmentIndex);
                    }
                    const auto analysis = PreAim::AnalyzeEdgeFanResults(
                        fovContext.camera, fanAnchor, fanLateral, fanRays, fanDecisions, fanMaxWidth);
                    std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                    auto& tracker = TargetingDetail::RuntimePreAimTrackerStorage();
                    tracker.ApplyEdgeFanAnalysis(analysis, isRefinePass);
                    hasParkPoint = tracker.HasParkPoint();
                    parkPoint = tracker.ParkPoint();
                    phase = tracker.Phase();
                    if (tracker.NeedsEdgeFanPass(isRefinePass, fanLo, fanHi)) {
                        const auto nextFanRays = PreAim::BuildEdgeFanRays(
                            fovContext.camera, tracker.FanAnchorTargetPos(), tracker.LateralDir(),
                            fanLo, fanHi, isRefinePass);
                        batchInput.edgeFanBorrowActive = true;
                        batchInput.edgeFanCount = PreAim::kEdgeFanRayCount;
                        for (size_t r = 0; r < PreAim::kEdgeFanRayCount; ++r) {
                            batchInput.edgeFanQueries[r] = makeBrokerQuery(
                                best.entitySnapshot, best.entityKey, best.guidanceBoneId,
                                nextFanRays[r].start, nextFanRays[r].end,
                                RaySegmentKind::EdgeFan, nextFanRays[r].segmentIndex);
                        }
                    }
                }
                best.preAimParkPointValid = hasParkPoint;
                if (hasParkPoint) {
                    best.guidanceAimPoint = parkPoint;
                    aimViewMatrix.WorldToScreen(best.guidanceAimPoint, &best.guidanceScreenPoint,
                                                OW::ScreenSizeSnapshot());
                    if (fovContext.valid && phase == PreAimPhase::Positioning) {
                        const float parkErrDeg = TargetingDetail::FovScoreDeg(fovContext, parkPoint);
                        std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                        auto& tracker = TargetingDetail::RuntimePreAimTrackerStorage();
                        tracker.ObserveParkCrosshairError(parkErrDeg);
                        phase = tracker.Phase();
                    }
                }
                best.preAimPhase = phase;
            }
        }

        if (best.valid && best.tier == VisibilityTier::Engageable) {
            std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
            auto& tracker = TargetingDetail::RuntimePreAimTrackerStorage();
            tracker.Reset();
            // F4 fix: only clear released-episode PreAim suppression on VERIFIED emergence,
            // never on tracking-only observation vacuum grace.
            if (!selectedEngageableViaVacuumGrace) {
                tracker.ClearSuppressionForEntity(best.entityKey, best.actorGeneration);
            }
            best.reactionRemainingMs = TargetingDetail::RuntimeReactionDelayGateStorage()
                .OnEngageable(best.entityKey, best.actorGeneration, best.entitySnapshot.HeroID,
                              best.motion.verticalVelocity, now64, peekEmergeTriggered);
        } else if (!best.valid) {
            std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
            TargetingDetail::RuntimePreAimTrackerStorage().Reset();
        }

        const bool holdAlternateDuringDropout = trackingContinuityEnabled &&
            !reverseLockBroken &&
            !preAimHardReleasedLock &&
            (!Config::targetAimPlanEnabled || (!AimPlan::HasOnlyFilter(planSelection) && planSelection.minHealth==0 && planSelection.maxHealth==0 && Config::View_aimPlan().groups.empty())) &&
            activeLock.active &&
            activeLock.entityKey != 0 &&
            !activeLockCandidateSelectable &&
            best.valid &&
            best.entityKey != activeLock.entityKey &&
            TrackingContinuityPolicy::IsWithinTargetDropoutGrace(
                now,
                activeLock.lastSeenTick,
                trackingContinuityGraceMs);
        if (holdAlternateDuringDropout) {
            best = TargetSolution{};
            peekEmergeTriggered = false;
        }

        if (best.valid) {
            if (peekEmergeTriggered && best.tier == VisibilityTier::Engageable) {
                best.transition = TransitionKind::PeekEmerge;
            } else if (!activeLock.active || best.entityKey != activeLock.entityKey) {
                best.transition = TransitionKind::TargetSwitch;
            } else if (activeLock.boneId != Plexies20260609::kUnusedRenderSkeletonBone &&
                       best.guidanceBoneId != activeLock.boneId) {
                best.transition = TransitionKind::BoneFallback;
            } else {
                best.transition = TransitionKind::Hold;
            }
        }

        Config::Targetenemyi = best.valid ? best.entityIndex : -1;
        if (best.valid && best.entityIndex >= 0 &&
            static_cast<size_t>(best.entityIndex) < entities.size()) {
            Config::health = entities[static_cast<size_t>(best.entityIndex)].PlayerHealth;
        }
        if (best.valid) {
            AimCandidateConfig selectedConfig(best.entitySnapshot.HeroID);
            if (best.effectiveFovDeg <= 0.0f) {
                best.effectiveFovDeg = Config::ResolveDynamicAimFovForDistance(
                    Config::View_Fov(), best.distance);
            }
            best.dynamicFov = Config::IsDynamicAimFovActive();
            best.dynamicFovPresetId = best.dynamicFov ? Config::View_aimbotDynamicFovPresetId() : -1;
            const Config::RuntimeDrawFovState drawState = Config::SnapshotRuntimeDrawFov();
            if (drawState.active &&
                drawState.slotKind == static_cast<int>(Config::FovRingSlotKind::Aim)) {
                Config::SetRuntimeDrawFov(best.effectiveFovDeg, drawState.slotKind, drawState.slotIndex);
            }
        }
        Config::aimbotEffectiveHitWindow = best.valid
            ? best.effectiveHitWindow
            : 0.0f;

        // Update Stage 2 rotating fallback cursor (R5 fix): reset to 0 when a valid target
        // is acquired so top-ranked candidates always take priority; advance by topK when
        // every evaluated candidate failed so remaining candidates get evaluated next tick.
        if (best.valid || stage1Count <= topK) {
            s_stage2FallbackCursor.store(0, std::memory_order_relaxed);
        } else {
            s_stage2FallbackCursor.store((startCursor + topK) % stage1Count, std::memory_order_relaxed);
        }

        // Bounded off-guidance acceptance ray warmup for the winning Hybrid + AnyValidHitbox
        // candidate (R4 + Phase 3 barrier fix): feed into Slot 1 of the 6-slot batch plan when
        // not already used, warming missing/stale world rays first and then missing/stale barrier
        // rays under Ray[World+Barrier] so off-guidance bones can recover after shield drop.
        if (best.valid && best.tier == VisibilityTier::Engageable &&
            hybridLiveWorldGate && liveRayScene != 0 &&
            !best.contour.evidence.exposed &&
            best.hitAcceptance == HitAcceptance::AnyValidHitbox &&
            best.acceptanceMask != 0 && fovContext.valid &&
            batchInput.top1FallbackOrWarmup.count == 0) {
            const bool warmOffGuidanceBarrier =
                slotVisibilityPolicy == VisibilityPolicy::RayWorldAndBarrier &&
                barrierRayMask != 0;
            const uint32_t warmupRayMaxAgeMs =
                TargetPipeline::ResolveCandidateRayMaxAgeMs(best.entitySnapshot.velocity.Size());
            int bestWarmupBoneId = Plexies20260609::kUnusedRenderSkeletonBone;
            Vector3 bestWarmupPoint{};
            RaySegmentKind bestWarmupSegKind = RaySegmentKind::Direct;
            uint64_t bestWarmupMask = worldRayMask;
            float bestWarmupCos = -2.0f;
            for (std::size_t slot = 0; slot < Plexies20260609::kSkeletonSlotCount; ++slot) {
                const SkeletonBoneMask grpBit = kSkeletonBoneGroupForRenderSlot[slot];
                const uint32_t slotBit = slot < 32 ? (1u << static_cast<uint32_t>(slot)) : 0u;
                if ((best.acceptanceMask & grpBit) == 0)
                    continue;
                const bool needWorldWarmup = (best.worldClearSlotMask & slotBit) == 0;
                const bool needBarrierWarmup = !needWorldWarmup && warmOffGuidanceBarrier &&
                    (best.barrierClearSlotMask & slotBit) == 0;
                if (!needWorldWarmup && !needBarrierWarmup)
                    continue;
                int slotBoneId = Plexies20260609::kUnusedRenderSkeletonBone;
                Vector3 slotPoint{};
                if (!TargetingDetail::TryResolveSkeletonBonePoint(best.entitySnapshot, slot, slotBoneId, slotPoint))
                    continue;
                if (slotBoneId == best.guidanceBoneId)
                    continue;
                const Vector3 predictedSlotPoint = TargetPipeline::ComputeAcceptanceRayEndpoint(
                    slotPoint, best.rawGuidancePoint, best.predictedGuidancePoint, best.nativeHitWindow);
                const RaySegmentKind slotSegKind = needBarrierWarmup
                    ? RaySegmentKind::Barrier : RaySegmentKind::Direct;
                const uint64_t slotMask = needBarrierWarmup ? barrierRayMask : worldRayMask;
                EngineTrace::Request slotReq{};
                slotReq.start = {fovContext.camera.X, fovContext.camera.Y, fovContext.camera.Z};
                slotReq.end = {predictedSlotPoint.X, predictedSlotPoint.Y, predictedSlotPoint.Z};
                slotReq.mask = slotMask;
                slotReq.sceneEpoch = liveRayScene;
                const auto slotCached = RayRuntime::Broker().Latest(
                    slotReq, frame.sequence, best.entitySnapshot.connection_epoch, liveRayScene,
                    best.entityKey, best.actorGeneration, slotBoneId, warmupRayMaxAgeMs,
                    EngineTrace::RayQueryBroker::kDefaultMaxEndpointDriftMeters,
                    slotSegKind, 0);
                if (slotCached && !slotCached->IsObservationVacuum() &&
                    slotCached->Fresh(warmupRayMaxAgeMs))
                    continue;
                const Vector3 toSlot = predictedSlotPoint - fovContext.camera;
                const float len = toSlot.Size();
                if (!std::isfinite(len) || len <= 1e-4f)
                    continue;
                const float cosAlign = (toSlot.X * fovContext.forward.X +
                    toSlot.Y * fovContext.forward.Y +
                    toSlot.Z * fovContext.forward.Z) / len;
                if (cosAlign > bestWarmupCos) {
                    bestWarmupCos = cosAlign;
                    bestWarmupBoneId = slotBoneId;
                    bestWarmupPoint = predictedSlotPoint;
                    bestWarmupSegKind = slotSegKind;
                    bestWarmupMask = slotMask;
                }
            }
            if (bestWarmupBoneId != Plexies20260609::kUnusedRenderSkeletonBone) {
                batchInput.top1FallbackOrWarmup.PushUrgent(makeBrokerQuery(
                    best.entitySnapshot, best.entityKey, bestWarmupBoneId,
                    fovContext.camera, bestWarmupPoint, bestWarmupSegKind, 0, bestWarmupMask));
            }
        }

        // §6.3: Atomically submit the planned 6-slot batch to RayQueryBroker so the worker
        // dispatches all slots concurrently in one 6-engine VisionProbe wave.
        if (hybridLiveWorldGate && liveRayScene != 0) {
            batchInput.top1WorldClear = top1WorldClearForBatch || (best.valid && best.ray.WorldClear());
            const TrajectoryRay::RayBatchPlan rayPlan = TrajectoryRay::PlanRayBatch(batchInput);
            if (rayPlan.count > 0) {
                RayRuntime::Broker().SubmitBatch(rayPlan.Span());
            }
        }

        const TargetCandidate legacyCandidate = best.ToCandidate();
        const bool forceClearLock = reverseLockBroken || preAimHardReleasedLock;
        TargetingDetail::CommitTargetLockRuntime(
            legacyCandidate,
            best.valid ? best.utilityScore : TargetingDetail::kNoTargetScore,
            lockPolicy,
            now,
            forceClearLock ? 0 : trackingContinuityGraceMs,
            forceClearLock,
            lockedReversalStarted,
            lockedReversalLatchedUntil,
            lockedReversalRefVel);

        if (!best.valid) {
            Diagnostics::Aim("target.primary result none reason=no_selectable_target entities=%zu stage0=%zu stage1=%zu candidates=%zu rayRejected=%zu rayPolicy=%s serviceBound=%d fovDeg=%.6f stance=%s",
                entities.size(),
                stage0.size(),
                stage1.size(),
                selectableCandidates,
                rayRejectedCandidates,
                RayDecisionPolicyName(activeRayBackend),
                RayRuntime::HasAttachedService() ? 1 : 0,
                Config::View_Fov(),
                InputStanceName(intentSnapshot.stance));
        } else if (best.entityIndex >= 0 && static_cast<size_t>(best.entityIndex) < entities.size()) {
            const c_entity& selected = entities[static_cast<size_t>(best.entityIndex)];
            const bool hasWorldVerdict = best.ray.world == RayDecisionStatus::Clear ||
                                         best.ray.world == RayDecisionStatus::Occluded;
            const bool visRayDiverge = hasWorldVerdict && (selected.Vis != best.ray.WorldClear());
            Diagnostics::Aim("target.primary result index=%d target=(%.9f,%.9f,%.9f) tier=%s preAimPhase=%d reactionMs=%.1f peekGate=%.2f segs=%u utility=%.6f commit=%.3f intentGain=%.3f align=%.3f stance=%s transition=%s priority=%s accept=%s fovScoreDeg=%.9f distance=%.3f bone=%d hitWindow=%.4f predictedEntry=%d health=%.3f hero=0x%llX vis=%d team=%d rayPolicy=%s rayActor=%s rayWorld=%s rayFrac=%.3f visRayDiverge=%d",
                best.entityIndex,
                best.guidanceAimPoint.X,
                best.guidanceAimPoint.Y,
                best.guidanceAimPoint.Z,
                VisibilityTierName(best.tier),
                static_cast<int>(best.preAimPhase),
                best.reactionRemainingMs,
                best.peekGateDeg,
                static_cast<unsigned>(best.trajectorySegments),
                best.utilityScore,
                best.commitment,
                best.intentGain,
                best.intentAlignment,
                InputStanceName(best.stance),
                TransitionKindName(best.transition),
                AimPriorityName(best.aimPriority),
                HitAcceptanceName(best.hitAcceptance),
                best.angularErrorDeg,
                best.distance,
                best.guidanceBoneId,
                best.effectiveHitWindow,
                best.predictedFovEntry ? 1 : 0,
                selected.PlayerHealth,
                static_cast<unsigned long long>(selected.HeroID),
                selected.Vis ? 1 : 0,
                selected.Team ? 1 : 0,
                RayDecisionPolicyName(activeRayBackend),
                RayDecisionStatusName(best.ray.actor),
                RayDecisionStatusName(best.ray.world),
                best.ray.fraction,
                visRayDiverge ? 1 : 0);
        }
        PublishSelectedAimPlan(legacyCandidate);
        Diagnostics::Aim("target.plan rule=%llu motionOverride=%d method=%d preferred=%d mode=%d priority=%d utility=%.6f",
            static_cast<unsigned long long>(best.plan.ruleId), best.plan.overrideMotion ? 1 : 0,
            best.plan.overrideMotion ? best.plan.motion.method : Config::AimBehaviorMethod(Config::View_aimBehavior()),
            best.planPreferred ? 1 : 0, Config::View_aimPlan().selection.mode, Config::View_aimbotPriority(), best.utilityScore);
        TargetingDetail::StoreLastTargetSolution(best);
        if(contour.enabled && best.valid && best.contour.evidence.fresh && best.contour.evidence.exposed &&
            (best.contour.evidence.identity!=TargetingDetail::lastContourObservation.evidence.identity ||
             best.contour.evidence.sequence>TargetingDetail::lastContourObservation.evidence.sequence))
            TargetingDetail::lastContourObservation=best.contour;
        return best;
    }

    inline TargetCandidate AcquireTarget(
        const ConsumptionFrame& frame,
        bool predit = false,
        bool ignoreInvisible = Config::View_aimbotIgnoreInvisible(),
        DWORD trackingContinuityGraceMs = 0) {
        return AcquireTargetSolution(frame, predit, ignoreInvisible, trackingContinuityGraceMs).ToCandidate();
    }

    inline TargetCandidate AcquireTriggerTarget(const ConsumptionFrame& frame, TriggerBoneMask rawBoneMask,
                                                bool predit = false,
                                                bool ignoreInvisible = Config::View_triggerbotIgnoreInvisible(),
                                                bool secondary = false) {
    OW::Config::ScopedConfigRead configRead;
        TargetCandidate best{};
        const TriggerBoneMask boneMask = NormalizeTriggerBoneMask(rawBoneMask);
        const auto& entities = frame.entities;
        const auto& localEntity = frame.local;
        const DWORD now = GetTickCount();
        const uint64_t now64 = GetTickCount64();
        if (!localEntity.Alive || localEntity.roster_state != EntityRosterState::Fresh ||
            !localEntity.HasUsableBody(now, EntitySceneGeneration()) ||
            !PresentationTiming::Fresh(localEntity.anchor_sample_tick_ms, now))
            return best;
        const Matrix& view = frame.view;
        const TargetingDetail::FovRuntimeContext fovContext =
            TargetingDetail::SnapshotFovRuntimeContext();
        const WeaponSpec* weaponSpec = ResolveWeaponSpec(localEntity.HeroID, Config::View_aimbotAttack());
        const bool resolvedPrediction = ResolvePredictionEnabled(
            ClampPredictionOverride(Config::View_aimbotPredictionMode()),
            weaponSpec,
            predit);
        const ProjectileRuntimeSpec projectileSpec =
            TargetingDetail::ResolveProjectileRuntimeSpec(weaponSpec, localEntity, secondary);
        const float hitboxScale = secondary ? Config::hitbox2 : Config::View_hitbox();
        const int activeRayBackend = UI::Preferences::ActiveRayBackend();
        const uint64_t barrierRayMask = RayRuntime::BarrierRayMask();
        const BarrierPolicy slotBarrierPolicy = ClampBarrierPolicy(Config::slotTargetRules.active
            ? Config::slotTargetRules.barrierPolicy : Config::aimBarrierPolicy);
        const VisibilityPolicy slotVisibilityPolicy = ResolveVisibilityPolicy(
            activeRayBackend, false, slotBarrierPolicy, barrierRayMask);
        const bool hybridLiveWorldGate =
            activeRayBackend == static_cast<int>(RayDecisionPolicy::Hybrid) &&
            RayRuntime::HasAttachedService() &&
            RayRuntime::SceneEpoch() != 0;
        const uint64_t liveTriggerRayScene = hybridLiveWorldGate ? RayRuntime::SceneEpoch() : 0;
        WarnIfHybridServiceUnbound(activeRayBackend, now);
        float bestNormalizedScore = (std::numeric_limits<float>::max)();
        size_t rayRejectedCandidates = 0;

        if (fovContext.valid) {
            for (std::size_t entityIndex = 0; entityIndex < entities.size(); ++entityIndex) {
                const c_entity& entity = entities[entityIndex];
                AimCandidateConfig candidateConfig(entity.HeroID);
                if (!candidateConfig.PassesStage0(entity)) continue;
                if (!TargetingDetail::IsRuntimeTargetValidStage0(entity) ||
                    !TargetingDetail::TargetTeamMatches(entity, Config::View_aimbotTeam(), localEntity))
                    continue;
                if (ignoreInvisible && !hybridLiveWorldGate && !entity.Vis) {
                    const uint64_t occKey = TargetingDetail::StableTargetKey(entity);
                    std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                    TargetingDetail::RuntimeReactionDelayGateStorage()
                        .ObserveOccludedOrPreAim(occKey, entity.actor_generation, now64, false);
                    continue;
                }
                if (entity.skill2act &&
                    entity.HeroID == eHero::HERO_GENJI &&
                    !entity.SameTeamAs(localEntity)) {
                    continue;
                }

                const Vector3 distanceAnchor = !TargetingDetail::IsZeroVector(entity.chest_pos)
                    ? entity.chest_pos
                    : entity.pos;
                const float initialDistance = fovContext.camera.DistTo(distanceAnchor);
                if (!TargetingDetail::DistancePassesAimFilter(initialDistance))
                    continue;

                // Finding #11 fix: aggregate clear vs occluded evidence per entity across all
                // accepted render slots before updating ReactionDelayGate so one blocked bone
                // never restarts an actor's reaction timer when another accepted bone is clear.
                bool entityHadClearBone = false;
                bool entityHadOccludedBone = false;
                float entityBestNormalizedScore = (std::numeric_limits<float>::max)();
                TargetCandidate entityBest{};
                float entityBestVelY = entity.velocity.Y;

                for (std::size_t renderSlot = 0;
                     renderSlot < kTriggerBoneGroupForRenderSlot.size();
                     ++renderSlot) {
                    const auto candidateMask=Config::slotTargetRules.active&&!entity.Team ? Config::slotTargetRules.allyBoneMask : boneMask;
                    if (!TriggerBoneMaskIncludesRenderSlot(candidateMask, renderSlot))
                        continue;

                    int boneId = Plexies20260609::kUnusedRenderSkeletonBone;
                    Vector3 rawAimPoint{};
                    if (!TargetingDetail::TryResolveSkeletonBonePoint(
                            entity,
                            renderSlot,
                            boneId,
                            rawAimPoint)) {
                        continue;
                    }

                    const LeadPredictionResult lead = TargetingDetail::ResolveLeadPrediction(
                        entity,
                        rawAimPoint,
                        projectileSpec,
                        resolvedPrediction,
                        secondary);
                    const Vector3 aimPoint = lead.finalAimPoint;
                    if (TargetingDetail::IsZeroVector(aimPoint) ||
                        !TargetingDetail::IsFiniteVector(aimPoint))
                        continue;

                    const float hitWindow = ResolveEffectiveHitWindow(
                        entity,
                        boneId,
                        weaponSpec,
                        hitboxScale,
                        Config::kLegacyDefaultHitboxRadius,
                        projectileSpec.projectileRadius);
                    const float distance = fovContext.camera.DistTo(aimPoint);
                    if (!std::isfinite(hitWindow) || hitWindow <= 0.0f ||
                        !std::isfinite(distance) || distance <= 0.0001f) {
                        continue;
                    }

                    const float angularWindowDeg = RAD2DEG(hitWindow / distance);
                    const float angularScoreDeg = TargetingDetail::FovScoreDeg(fovContext, aimPoint);
                    if (!std::isfinite(angularWindowDeg) || angularWindowDeg <= 0.0f ||
                        !std::isfinite(angularScoreDeg)) {
                        continue;
                    }

                    std::optional<float> nativeScore;
                    NativeHitWindowInput nativeInput{};
                    const bool nativeUsable = weaponSpec && NativeHitboxSampleUsable(entity, now);
                    if (nativeUsable) {
                        nativeInput = MakeNativeHitWindowInput(lead, weaponSpec->aimClass, hitboxScale);
                        // Use the crosshair direction and preserve range for projectile flight time.
                        nativeScore = NativeHitWindowScore(entity, boneId, now, fovContext.camera,
                            fovContext.forward * distance, nativeInput);
                    }
                    if (nativeUsable && !nativeScore)
                        continue;
                    const float fallbackScore = angularScoreDeg / (std::max)(angularWindowDeg, 0.0001f);
                    if (nativeScore ? *nativeScore > 1.0f : angularScoreDeg > angularWindowDeg + 0.0001f)
                        continue;
                    const float normalizedScore = nativeScore.value_or(fallbackScore);
                    const bool preferred=candidateConfig.Preferred(entity, distance, angularScoreDeg);
                    const uint64_t entityStableId = entity.address ? entity.address : entity.LinkBase;
                    if (entityBest.valid && !AimPlan::Better(preferred, normalizedScore, entityStableId,
                                                             entityBest.planPreferred, entityBestNormalizedScore, entityBest.entityKey))
                        continue;
                    if (best.valid && !AimPlan::Better(preferred, normalizedScore, entityStableId,
                                                       best.planPreferred, bestNormalizedScore, best.entityKey))
                        continue;

                    TrajectoryRay::ParabolicChordPlan trigChordPlan{};
                    if (projectileSpec.projectileSpeed > 0.0f &&
                        projectileSpec.gravity && projectileSpec.gravityAcceleration > 0.0f) {
                        trigChordPlan = TrajectoryRay::BuildParabolicChordPlan(
                            fovContext.camera, lead.preFireAimPoint, lead.targetVelocity,
                            projectileSpec.projectileSpeed, projectileSpec.gravityAcceleration,
                            aimPoint);
                    } else {
                        trigChordPlan.segmentCount = 1;
                        trigChordPlan.segmentKind = RaySegmentKind::Direct;
                        trigChordPlan.impactPoint = aimPoint;
                        trigChordPlan.segments[0] = {fovContext.camera, aimPoint};
                    }

                    RayDecision candidateRay = MakeNativeRayDecision(
                        entity, boneId, fovContext.camera,
                        fovContext.forward * distance, aimPoint,
                        nativeInput, frame.sequence, false, &trigChordPlan);
                    TraceHitKind triggerHitKind = candidateRay.world == RayDecisionStatus::Clear
                        ? TraceHitKind::None
                        : (candidateRay.world == RayDecisionStatus::Occluded ? TraceHitKind::World : TraceHitKind::Unknown);
                    // Phase 3 (§8.2, Decision #4): WaitBarrierDrop Trigger strictly checks Slot 3
                    // barrier layer before authorizing fire so shots are never wasted into shields.
                    if (slotVisibilityPolicy == VisibilityPolicy::RayWorldAndBarrier &&
                        hybridLiveWorldGate && liveTriggerRayScene != 0 &&
                        candidateRay.WorldClear()) {
                        const auto barrierEval = TrajectoryRay::EvaluateBarrierLayerDecision(
                            RayRuntime::Broker(),
                            slotVisibilityPolicy,
                            slotBarrierPolicy,
                            barrierRayMask,
                            true,
                            fovContext.camera,
                            trigChordPlan.impactPoint,
                            frame.sequence,
                            entity.connection_epoch,
                            liveTriggerRayScene,
                            entityStableId,
                            entity.actor_generation,
                            boneId,
                            entity.native_hitboxes.tick,
                            RayDecision::kDefaultMaxAgeMs,
                            /*submitImmediately=*/true);
                        if (barrierEval.barrierActive) {
                            candidateRay.barrierCheckRequired = true;
                            candidateRay.barrier = barrierEval.barrierDecision.world;
                            candidateRay.barrierAgeMs = barrierEval.barrierDecision.ageMs;
                            triggerHitKind = barrierEval.hitKind;
                        }
                    }
                    if (!RayDecisionAllowsAutoFire(candidateRay, false, activeRayBackend)) {
                        ++rayRejectedCandidates;
                        if (candidateRay.world == RayDecisionStatus::Occluded ||
                            (hybridLiveWorldGate && !entity.Vis && !candidateRay.BarrierOccluded())) {
                            entityHadOccludedBone = true;
                        }
                        Diagnostics::Aim(
                            "target.trigger.ray_reject index=%zu bone=%d vis=%d policy=%s visPolicy=%s actor=%s world=%s barrier=%s hitKind=%s segs=%u ageMs=%u latencyUs=%u",
                            entityIndex, boneId, entity.Vis ? 1 : 0,
                            RayDecisionPolicyName(activeRayBackend),
                            VisibilityPolicyName(slotVisibilityPolicy),
                            RayDecisionStatusName(candidateRay.actor),
                            RayDecisionStatusName(candidateRay.world),
                            RayDecisionStatusName(candidateRay.barrier),
                            TraceHitKindName(triggerHitKind),
                            static_cast<unsigned>(trigChordPlan.segmentCount),
                            candidateRay.ageMs, candidateRay.latencyUs);
                        continue;
                    }
                    const bool stage2Vis = hybridLiveWorldGate
                        ? candidateRay.WorldClear()
                        : TargetingDetail::SelectionVisible(entity);
                    if (!candidateConfig.Passes(entity, distance, angularScoreDeg, stage2Vis))
                        continue;

                    entityHadClearBone = true;
                    entityBestNormalizedScore = normalizedScore;
                    entityBestVelY = lead.targetVelocity.Y;
                    entityBest = TargetCandidate{};
                    entityBest.valid = true;
                    entityBest.tier = VisibilityTier::Engageable;
                    entityBest.reactionRemainingMs = 0.0f;
                    entityBest.trajectorySegments = trigChordPlan.segmentCount;
                    entityBest.hitAcceptance = HitAcceptance::AnyValidHitbox;
                    entityBest.acceptanceMask = candidateMask;
                    entityBest.worldClearSlotMask = candidateRay.WorldClear()
                        ? TargetPipeline::RenderSlotBitForBoneId(boneId)
                        : 0u;
                    entityBest.barrierClearSlotMask = candidateRay.BarrierClear()
                        ? TargetPipeline::RenderSlotBitForBoneId(boneId)
                        : 0u;
                    entityBest.plan = candidateConfig.value;
                    entityBest.planPreferred = preferred;
                    entityBest.entityIndex = static_cast<int>(entityIndex);
                    entityBest.entityKey = entityStableId;
                    entityBest.actorGeneration = entity.actor_generation;
                    entityBest.entitySnapshot = entity;
                    entityBest.boneId = boneId;
                    entityBest.rawAimPoint = rawAimPoint;
                    entityBest.predictedAimPoint = aimPoint;
                    entityBest.aimPoint = aimPoint;
                    entityBest.screenPoint = TargetingDetail::CrosshairCenter();
                    view.WorldToScreen(
                        aimPoint,
                        &entityBest.screenPoint,
                        OW::ScreenSizeSnapshot());
                    entityBest.distance = distance;
                    entityBest.fovScore = angularScoreDeg;
                    entityBest.effectiveFovDeg = secondary
                        ? Config::ResolveDynamicAimFovForDistance(Config::View_Fov2(), distance)
                        : Config::ResolveDynamicAimFovForDistance(Config::View_Fov(), distance);
                    entityBest.dynamicFov = Config::IsDynamicAimFovActive();
                    entityBest.dynamicFovPresetId = entityBest.dynamicFov
                        ? Config::View_aimbotDynamicFovPresetId()
                        : -1;
                    entityBest.effectiveHitWindow = hitWindow;
                    entityBest.motion = TargetingDetail::EstimateMotionState(entity, entityBest.screenPoint);
                    entityBest.weaponSpec = weaponSpec;
                    entityBest.nativeHitWindow = nativeInput;
                    entityBest.ray = candidateRay;
                    entityBest.visibilityPolicy = slotVisibilityPolicy;
                    entityBest.barrierPolicy = slotBarrierPolicy;
                    entityBest.hitKind = triggerHitKind;
                }

                const uint64_t trigStableKey = TargetingDetail::StableTargetKey(entity);
                if (entityHadClearBone && entityBest.valid) {
                    bool trigPeekEmerge = false;
                    float trigReactionRemainingMs = 0.0f;
                    {
                        std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                        trigReactionRemainingMs = TargetingDetail::RuntimeReactionDelayGateStorage()
                            .OnEngageable(trigStableKey, entity.actor_generation, entity.HeroID,
                                          entityBestVelY, now64, trigPeekEmerge);
                    }
                    if (trigReactionRemainingMs <= 0.0f) {
                        entityBest.transition = trigPeekEmerge ? TransitionKind::PeekEmerge : TransitionKind::Hold;
                        bestNormalizedScore = entityBestNormalizedScore;
                        best = std::move(entityBest);
                    }
                } else if (entityHadOccludedBone) {
                    std::lock_guard<std::mutex> preAimLock(TargetingDetail::RuntimePreAimMutex());
                    TargetingDetail::RuntimeReactionDelayGateStorage()
                        .ObserveOccludedOrPreAim(trigStableKey, entity.actor_generation, now64, false);
                }
            }
        }

        Config::Targetenemyi = best.valid ? best.entityIndex : -1;
        if (best.valid)
            Config::health = best.entitySnapshot.PlayerHealth;
        if (secondary)
            Config::aimbotEffectiveHitWindow2 = best.valid ? best.effectiveHitWindow : 0.0f;
        else
            Config::aimbotEffectiveHitWindow = best.valid ? best.effectiveHitWindow : 0.0f;

        TargetingDetail::StoreLastTargetCandidate(best);
        const bool hasTriggerWorldVerdict = best.ray.world == RayDecisionStatus::Clear ||
                                            best.ray.world == RayDecisionStatus::Occluded;
        const bool triggerVisRayDiverge = best.valid && hasTriggerWorldVerdict &&
                                          (best.entitySnapshot.Vis != best.ray.WorldClear());
        Diagnostics::Aim(
            "target.trigger result=%s secondary=%d boneMask=0x%03X entityIndex=%d bone=%d scoreDeg=%.6f hitWindow=%.6f rayPolicy=%s rayActor=%s rayWorld=%s rayAgeMs=%u rayLatencyUs=%u rayRejected=%zu visRayDiverge=%d",
            best.valid ? "hit" : "none",
            secondary ? 1 : 0,
            static_cast<unsigned int>(boneMask),
            best.entityIndex,
            best.boneId,
            best.fovScore,
            best.effectiveHitWindow,
            RayDecisionPolicyName(activeRayBackend),
            RayDecisionStatusName(best.ray.actor),
            RayDecisionStatusName(best.ray.world),
            best.ray.ageMs,
            best.ray.latencyUs,
            rayRejectedCandidates,
            triggerVisRayDiverge ? 1 : 0);
        return best;
    }

    inline TargetSolution AcquireCurrentAimSolution(bool predit, bool ignoreInvisible = Config::View_aimbotIgnoreInvisible()) {
        const auto frame=consumerFrame?consumerFrame:SnapshotFrame();
        return AcquireTargetSolution(*frame, predit, ignoreInvisible);
    }

    inline TargetCandidate AcquireCurrentAimTarget(bool predit, bool ignoreInvisible = Config::View_aimbotIgnoreInvisible()) {
        const auto frame=consumerFrame?consumerFrame:SnapshotFrame();
        return AcquireTarget(*frame, predit, ignoreInvisible);
    }

    inline Vector3 GetVector3(bool predit = false, bool ignoreInvisible = Config::View_aimbotIgnoreInvisible()) {
        const TargetCandidate candidate = AcquireCurrentAimTarget(predit, ignoreInvisible);
        return candidate.valid ? candidate.aimPoint : Vector3{};
    }

    // =========================================================================
    // GetVector3 for track back
    // =========================================================================

    inline Vector3 GetVector3fortrackback(bool predit = false) {
        int TarGetIndex = -1;
        Vector3 target{};
        auto local_entity = TargetingDetail::SnapshotLocalEntity();
        if (local_entity.HeroID == eHero::HERO_HANJO) predit = true;
        const WeaponSpec* weaponSpec = ResolveWeaponSpec(local_entity.HeroID, Config::View_aimbotAttack());
        const bool resolvedPrediction = ResolvePredictionEnabled(
            ClampPredictionOverride(Config::View_aimbotPredictionMode()),
            weaponSpec,
            predit);
        const ProjectileRuntimeSpec projectileSpec =
            TargetingDetail::ResolveProjectileRuntimeSpec(weaponSpec, local_entity, false);
        Vector2 CrossHair = TargetingDetail::CrosshairCenter();
        const Matrix aimViewMatrix = OW::SnapshotViewMatrix();
        Config::ScopedConfigRead configRead;
        const TargetingDetail::FovRuntimeContext fovContext = TargetingDetail::SnapshotFovRuntimeContext();
        auto entities = TargetingDetail::SnapshotEntities();
        float origin = 100000.f;
        if (entities.size() > 0) {
            Vector3 PreditPos, RootPos;
            for (size_t i = 0; i < entities.size(); i++) {
            AimCandidateConfig candidateConfig(entities[i].HeroID);
            if(!candidateConfig.Passes(entities[i])) continue;
                const bool teamPass = TargetingDetail::TargetTeamMatches(
                    entities[i], Config::View_aimbotTeam(), local_entity);
                if (TargetingDetail::IsRuntimeTargetValid(entities[i], false) && teamPass) {
                    RootPos = TargetingDetail::CoreBoneRawPosition(entities[i],
                        AimBoneToSkeletonBoneId(Config::View_Bone()));
                    if (TargetingDetail::IsZeroVector(RootPos))
                        continue;
                    const LeadPredictionResult lead = TargetingDetail::ResolveLeadPrediction(
                        entities[i],
                        RootPos,
                        projectileSpec,
                        resolvedPrediction,
                        false);
                    PreditPos = lead.finalAimPoint;
                    FovGeometry::CandidateFovEvaluation fovEvaluation{};
                    if (TargetingDetail::EvaluateCandidateFov(
                            fovContext,
                            RootPos,
                            lead.finalAimPoint,
                            Config::View_Fov(),
                            &Config::ResolveDynamicAimFovForDistance,
                            fovEvaluation)) {
                        const float distance = fovEvaluation.distance;
                        const float fovScoreDeg = fovEvaluation.scoreDeg;
                        float score;
                        if (Config::View_aimbotPriority() == 0) {
                            score = fovScoreDeg;
                        } else if (Config::View_aimbotPriority() == 1) {
                            score = entities[i].PlayerHealth;
                        } else {
                            score = distance;
                        }
                        if (score < origin) {
                            target = lead.finalAimPoint;
                            origin = score;
                            TarGetIndex = i;
                        }
                    }
                }
            }
            if (TarGetIndex != -1) {
                if ((Config::View_autobone() || Config::autobone2) && entities[TarGetIndex].HeroID != 0x16dd && entities[TarGetIndex].HeroID != 0x16ee) {
                    int closestBoneId = BONE_CHEST;
                    Vector3 closestRoot{};
                    if (TargetingDetail::TrySelectClosestCoreAimPoint(
                            entities[TarGetIndex],
                            fovContext,
                            aimViewMatrix,
                            CrossHair,
                            closestBoneId,
                            closestRoot)) {
                        RootPos = closestRoot;
                        PreditPos = RootPos;
                        target = RootPos;
                        if (resolvedPrediction) {
                            const LeadPredictionResult lead = TargetingDetail::ResolveLeadPrediction(
                                entities[TarGetIndex],
                                RootPos,
                                projectileSpec,
                                true,
                                false);
                            PreditPos = lead.finalAimPoint;
                            target = lead.finalAimPoint;
                        }
                    }
                }
                Config::health = entities[TarGetIndex].PlayerHealth;
            }
        }
        return target;
    }

    // =========================================================================
    // GetVector3 for Genji blade
    // =========================================================================

    inline Vector3 GetVector3forgenji(bool predit = false) {
        PublishSelectedAimPlan({});
        int TarGetIndex = -1;
        Vector3 target{};
        Vector2 CrossHair = TargetingDetail::CrosshairCenter();
        auto entities = TargetingDetail::SnapshotEntities();
        auto local_entity = TargetingDetail::SnapshotLocalEntity();
        const Vector3 camera = TargetingDetail::CameraPosition();
        float origin = 100000.f;
        if (entities.size() > 0) {
            for (size_t i = 0; i < entities.size(); i++) {
                if (entities[i].HeroID == 0x16dd || entities[i].HeroID == 0x16ee) continue;
                if (entities[i].HeroID == eHero::HERO_GENJI && entities[i].skill2act) continue;
                const bool teamPass = TargetingDetail::TargetTeamMatches(
                    entities[i], Config::View_aimbotTeam(), local_entity);
                if (TargetingDetail::IsRuntimeTargetValid(entities[i], false) && teamPass) {
                    const bool isBot = GameData::IsTrainingBotHeroId(entities[i].HeroID);
                    const auto targetSlot = !local_entity.skillcd1 && !isBot
                        ? SkeletonSemantics::Slot::FootRight
                        : SkeletonSemantics::Slot::Chest;
                    Vector3 PreditPos = TargetingDetail::SemanticSkeletonRawPosition(entities[i], targetSlot);
                    if (TargetingDetail::IsZeroVector(PreditPos))
                        continue;
                    if (isBot) {
                        if (!local_entity.skillcd1) PreditPos.Y -= 0.4f;
                    }
                    Vector3 RootPos = PreditPos;
                    if (entities[i].HeroID == eHero::HERO_WRECKINGBALL) {
                        PreditPos.Y -= 0.7f;
                        RootPos.Y -= 0.7f;
                    }
                    float score;
                    if (Config::View_aimbotPriority() == 0) {
                        score = camera.DistTo(PreditPos);
                        if (entities[i].PlayerHealth > 200.f) score = entities[i].PlayerHealth;
                        if (entities[i].HeroID == eHero::HERO_ZENYATTA && entities[i].ultimate == 0.f) score = 1000.f;
                    } else if (Config::View_aimbotPriority() == 1) {
                        score = entities[i].PlayerHealth;
                    } else {
                        score = camera.DistTo(PreditPos);
                    }
                    if (score < origin && score <= 6000.f) {
                        target = RootPos;
                        origin = score;
                        TarGetIndex = i;
                    }
                }
            }
            if (TarGetIndex != -1) {
                Config::health = entities[TarGetIndex].PlayerHealth;
                Config::Targetenemyi = TarGetIndex;
            }
        }
        return target;
    }

    // =========================================================================
    // GetVector3 for secondary aim
    // =========================================================================

    inline Vector3 GetVector3aim2(bool predit = false, bool ignoreInvisible = Config::View_aimbotIgnoreInvisible()) {
        int TarGetIndex = -1;
        Vector3 target{};
        Vector2 CrossHair = TargetingDetail::CrosshairCenter();
        const Matrix aimViewMatrix = OW::SnapshotViewMatrix();
        Config::ScopedConfigRead configRead;
        const TargetingDetail::FovRuntimeContext fovContext = TargetingDetail::SnapshotFovRuntimeContext();
        const Vector3 camera = TargetingDetail::CameraPosition();
        auto entities = TargetingDetail::SnapshotEntities();
        auto local_entity = TargetingDetail::SnapshotLocalEntity();
        const WeaponSpec* weaponSpec = ResolveWeaponSpec(local_entity.HeroID, Config::View_aimbotAttack());
        const bool resolvedPrediction = ResolvePredictionEnabled(
            ClampPredictionOverride(Config::View_aimbotPredictionMode()),
            weaponSpec,
            predit);
        const ProjectileRuntimeSpec projectileSpec =
            TargetingDetail::ResolveProjectileRuntimeSpec(weaponSpec, local_entity, true);
        float origin = 100000.f;
        int selectedBoneId = AimBoneToSkeletonBoneId(Config::Bone2);
        bool selectedPredictedEntry = false;
        if (entities.size() > 0) {
            for (size_t i = 0; i < entities.size(); i++) {
                const bool teamPass = TargetingDetail::TargetTeamMatches(
                    entities[i], Config::View_aimbotTeam(), local_entity);
                if (TargetingDetail::IsRuntimeTargetValid(entities[i], false) && teamPass) {
                    if (ignoreInvisible && !entities[i].Vis)
                        continue;
                    const float initialDistance = camera.DistTo(entities[i].chest_pos);
                    if (!TargetingDetail::DistancePassesAimFilter(initialDistance))
                        continue;
                    int candidateBoneId = AimBoneToSkeletonBoneId(
                        entities[i].Team ? Config::Bone2 : Config::View_Bone());
                    Vector3 rootPosition = TargetingDetail::CoreBoneRawPosition(entities[i], candidateBoneId);
                    if (Config::autobone2 &&
                        entities[i].HeroID != 0x16dd &&
                        entities[i].HeroID != 0x16ee) {
                        int closestBoneId = BONE_CHEST;
                        Vector3 closestRoot{};
                        if (TargetingDetail::TrySelectClosestCoreAimPoint(
                                entities[i],
                                fovContext,
                                aimViewMatrix,
                                CrossHair,
                                closestBoneId,
                                closestRoot)) {
                            candidateBoneId = closestBoneId;
                            rootPosition = closestRoot;
                        }
                    }
                    if (TargetingDetail::IsZeroVector(rootPosition))
                        continue;

                    const LeadPredictionResult lead = TargetingDetail::ResolveLeadPrediction(
                        entities[i],
                        rootPosition,
                        projectileSpec,
                        resolvedPrediction,
                        true);
                    const float fovEntryHorizonMs =
                        Config::ClampFovEntryPredictionMs(Config::View_aimbotFovEntryPredictionMs());
                    const Vector3 fovEntryAimPoint = lead.finalAimPoint +
                        lead.targetVelocity * (fovEntryHorizonMs / 1000.0f);
                    FovGeometry::PredictedEntryEvaluation fovEvaluation{};
                    if (FovGeometry::EvaluateCandidateFovWithPredictedEntry(
                             fovContext,
                             rootPosition,
                             lead.finalAimPoint,
                             fovEntryAimPoint,
                             Config::View_Fov2(),
                             &Config::ResolveDynamicAimFovForDistance,
                             Config::View_aimbotPredictFovEntry(),
                             Config::ClampFovEntryMaxOutsideDeg(Config::View_aimbotFovEntryMaxOutsideDeg()),
                             fovEvaluation)) {
                        const float distance = fovEvaluation.distance;
                        const float fovScoreDeg = fovEvaluation.acceptedByPrediction
                            ? fovEvaluation.projectedScoreDeg
                            : fovEvaluation.currentScoreDeg;
                        if (!TargetingDetail::DistancePassesAimFilter(distance))
                            continue;
                        float score;
                        if (Config::View_aimbotPriority() == 0) {
                            score = fovScoreDeg;
                        } else if (Config::View_aimbotPriority() == 1) {
                            score = entities[i].PlayerHealth;
                        } else {
                            score = distance;
                        }
                        const bool preferCurrentInside = TarGetIndex != -1 &&
                            selectedPredictedEntry &&
                            !fovEvaluation.acceptedByPrediction;
                        const bool predictedEntryLosesToCurrentInside = TarGetIndex != -1 &&
                            !selectedPredictedEntry &&
                            fovEvaluation.acceptedByPrediction;
                        if (!predictedEntryLosesToCurrentInside &&
                            (preferCurrentInside || score < origin)) {
                            target = fovEvaluation.acceptedAimPoint;
                            origin = score;
                            TarGetIndex = i;
                            selectedBoneId = candidateBoneId;
                            selectedPredictedEntry = fovEvaluation.acceptedByPrediction;
                        }
                    }
                }
            }
            if (TarGetIndex != -1) {
                Config::health = entities[TarGetIndex].PlayerHealth;
                Config::Targetenemyi = TarGetIndex;
                Config::aimbotEffectiveHitWindow2 = ResolveEffectiveHitWindow(
                    entities[TarGetIndex],
                    selectedBoneId,
                    weaponSpec,
                    Config::hitbox2,
                    Config::kLegacyDefaultHitboxRadius,
                    projectileSpec.projectileRadius);
            }
        } else {
            Config::aimbotEffectiveHitWindow2 = 0.0f;
        }
        if (TarGetIndex == -1)
            Config::aimbotEffectiveHitWindow2 = 0.0f;
        return target;
    }

    // =========================================================================
    // GetVector3 for auto-FOV target
    // =========================================================================

    inline Vector3 GetVector3forfov(bool predit = false) {
        int TarGetIndex = -1;
        Vector3 target{};
        Config::ScopedConfigRead configRead;
        const TargetingDetail::FovRuntimeContext fovContext = TargetingDetail::SnapshotFovRuntimeContext();
        const Vector3 camera = TargetingDetail::CameraPosition();
        auto entities = TargetingDetail::SnapshotEntities();
        auto local_entity = TargetingDetail::SnapshotLocalEntity();
        float origin = 100000.f;
        if (entities.size() > 0) {
            Vector3 Vel, PreditPos, RootPos;
            for (size_t i = 0; i < entities.size(); i++) {
                const bool teamPass = TargetingDetail::TargetTeamMatches(
                    entities[i], Config::View_aimbotTeam(), local_entity);
                if (TargetingDetail::IsRuntimeTargetValid(entities[i], false) && teamPass) {
                    const float distance = camera.DistTo(entities[i].chest_pos);
                    if (!TargetingDetail::DistancePassesAimFilter(distance))
                        continue;
                    RootPos = TargetingDetail::CoreBoneRawPosition(entities[i], BONE_HEAD);
                    if (TargetingDetail::IsZeroVector(RootPos))
                        continue;
                    PreditPos = RootPos;
                    float score;
                    if (Config::View_aimbotPriority() == 0) {
                        score = TargetingDetail::FovScoreDeg(fovContext, RootPos);
                    } else if (Config::View_aimbotPriority() == 1) {
                        score = entities[i].PlayerHealth;
                    } else {
                        score = distance;
                    }
                    if (score < origin) {
                        target = RootPos;
                        origin = score;
                        TarGetIndex = i;
                    }
                }
            }
            if (TarGetIndex != -1) {
                Config::health = entities[TarGetIndex].PlayerHealth;
                Config::Targetenemyifov = TarGetIndex;
            }
        }
        return target;
    }

} // namespace OW
