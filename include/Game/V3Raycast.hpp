#pragma once

#include "Game/EngineTrace.hpp"
#include "Game/ModelShapes.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace OW::V3Raycast {

enum class Backend : uint8_t {
    NativeHitbox = 0,
    UniCore,
};

enum class Status : uint8_t {
    Hit = 0,
    Miss,
    UnknownData,
    BackendUnavailable,
    InvalidInput,
    InvalidScene,
    Timeout,
    Error,
};

struct Query {
    Vector3 origin{};
    Vector3 direction{};
    float maxDistance = 0.0f;
    float projectileRadius = 0.0f;
    float hitboxScalePercent = 100.0f;
};

struct WorldShape {
    Vector3 a{};
    Vector3 b{};
    float radius = 0.0f;
    int mappedBone = -1;
    ModelShapes::CriticalState critical = ModelShapes::CriticalState::Unknown;
};

struct ShapeFrame {
    std::span<const ModelShapes::Shape> shapes{};
    std::span<const ModelShapes::Pose> poses{};
    std::span<const uint8_t> valid{};
    std::span<const int> shapeMapped{};
    std::span<const ModelShapes::CriticalState> critical{};
    Vector3 worldOffset{};
    float worldScale = 1.0f;
};

struct Hit {
    Status status = Status::UnknownData;
    int shapeIndex = -1;
    float distance = 0.0f;
    Vector3 point{};
    float effectiveRadius = 0.0f;
};

enum class VisualClass : uint8_t {
    Ordinary = 0,
    Critical,
    Unknown,
};

struct ShapeVisualState {
    VisualClass visual = VisualClass::Unknown;
    bool bound = false;
};

std::vector<WorldShape> BuildWorldShapes(const ShapeFrame& frame);
ShapeVisualState ClassifyVisual(int mappedBone, ModelShapes::CriticalState critical);
const char* BackendName(Backend backend);
const char* StatusName(Status status);

struct TraceCalls {
    unsigned analyticTests = 0;
    unsigned worldRequests = 0;
};

Hit TraceNative(const Query& query, std::span<const WorldShape> shapes, TraceCalls* calls = nullptr);
Hit TraceUniCore(const Query& query, EngineTrace::Service* service, uint64_t sceneEpoch,
                uint32_t budgetMs = 0, TraceCalls* calls = nullptr);
Hit TraceSelected(Backend backend, const Query& query, std::span<const WorldShape> shapes,
                  EngineTrace::Service* service = nullptr, uint64_t sceneEpoch = 0,
                  uint32_t budgetMs = 0, TraceCalls* calls = nullptr);

}  // namespace OW::V3Raycast
