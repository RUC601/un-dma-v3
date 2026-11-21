#include "Game/V3Raycast.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace OW::V3Raycast {
namespace {

constexpr float kEpsilon = 1.0e-6f;

float Dot(Vector3 a, Vector3 b) {
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}

bool Finite(Vector3 value) {
    return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
}

bool ValidQuery(const Query& query, Vector3& direction) {
    if (!Finite(query.origin) || !Finite(query.direction) ||
        !std::isfinite(query.maxDistance) || query.maxDistance <= 0.0f ||
        !std::isfinite(query.projectileRadius) || query.projectileRadius < 0.0f ||
        !std::isfinite(query.hitboxScalePercent) || query.hitboxScalePercent < 0.0f ||
        query.hitboxScalePercent > 150.0f) {
        return false;
    }
    const float length = query.direction.Size();
    if (!std::isfinite(length) || length <= kEpsilon) return false;
    direction = query.direction / length;
    return true;
}

float EffectiveRadius(const Query& query, const WorldShape& shape) {
    return (shape.radius + query.projectileRadius) * (query.hitboxScalePercent / 100.0f);
}

bool ContainsOrigin(Vector3 origin, Vector3 a, Vector3 b, float radius) {
    const Vector3 axis = b-a;
    const float len2 = Dot(axis,axis);
    const float t = len2 > 0 ? std::clamp(Dot(origin-a,axis)/len2,0.f,1.f) : 0.f;
    const Vector3 delta = origin-(a+axis*t);
    return Dot(delta,delta) <= radius*radius;
}

bool ValidShape(const WorldShape& shape, float effectiveRadius) {
    return Finite(shape.a) && Finite(shape.b) && std::isfinite(shape.radius) &&
        shape.radius > 0.0f && std::isfinite(effectiveRadius) && effectiveRadius > 0.0f;
}

std::optional<float> IntersectCapsule(const Vector3& origin, const Vector3& direction,
                                      float maxDistance, Vector3 a, Vector3 b, float radius) {
    if (ContainsOrigin(origin,a,b,radius)) return 0.f;
    const Vector3 ba = b - a;
    const Vector3 oa = origin - a;
    const float baba = Dot(ba, ba);
    const float bard = Dot(ba, direction);
    const float baoa = Dot(ba, oa);
    const float rdoa = Dot(direction, oa);
    const float oaoa = Dot(oa, oa);
    const float radiusSquared = radius * radius;
    float best = std::numeric_limits<float>::infinity();

    if (baba > kEpsilon) {
        const float coefficientA = baba - bard * bard;
        const float coefficientB = baba * rdoa - baoa * bard;
        const float coefficientC = baba * oaoa - baoa * baoa - radiusSquared * baba;
        const float discriminant = coefficientB * coefficientB - coefficientA * coefficientC;
        if (discriminant >= 0.0f && std::fabs(coefficientA) > kEpsilon) {
            const float root = std::sqrt(discriminant);
            const float t = (-coefficientB - root) / coefficientA;
            const float y = baoa + t * bard;
            if (t >= 0.0f && t <= maxDistance && y > 0.0f && y < baba) best = t;
        }
    }

    const auto sphere = [&](Vector3 center) {
        const Vector3 offset = origin - center;
        const float projection = -Dot(offset, direction);
        const float discriminant = projection * projection - (Dot(offset, offset) - radiusSquared);
        if (discriminant < 0.0f) return;
        const float t = projection - std::sqrt(discriminant);
        if (t >= 0.0f && t <= maxDistance) best = (std::min)(best, t);
    };
    sphere(a);
    if (baba > kEpsilon) sphere(b);

    if (!std::isfinite(best)) return std::nullopt;
    return best;
}

Hit TraceAnalytic(const Query& query, std::span<const WorldShape> shapes, TraceCalls* calls) {
    Vector3 direction{};
    if (!ValidQuery(query, direction)) return {Status::InvalidInput};
    if (shapes.empty()) return {Status::UnknownData};

    Hit best{Status::Miss};
    best.distance = query.maxDistance;
    if (query.hitboxScalePercent == 0) return best;
    for (size_t index = 0; index < shapes.size(); ++index) {
        const auto& shape = shapes[index];
        const float radius = EffectiveRadius(query, shape);
        if (!ValidShape(shape, radius)) return {Status::UnknownData};
        if (calls) ++calls->analyticTests;
        const auto distance = IntersectCapsule(query.origin, direction, query.maxDistance,
                                               shape.a, shape.b, radius);
        if (!distance || (best.status == Status::Hit && *distance >= best.distance)) continue;
        best.status = Status::Hit;
        best.shapeIndex = static_cast<int>(index);
        best.distance = *distance;
        best.effectiveRadius = radius;
        best.point = query.origin + direction * *distance;
    }
    return best;
}

#if defined(_WIN32)
struct RTCDeviceTy;
struct RTCSceneTy;
struct RTCGeometryTy;
using RTCDevice = RTCDeviceTy*;
using RTCScene = RTCSceneTy*;
using RTCGeometry = RTCGeometryTy*;

enum RTCGeometryType : int { RTC_GEOMETRY_TYPE_ROUND_LINEAR_CURVE = 16, RTC_GEOMETRY_TYPE_SPHERE_POINT = 50 };
enum RTCBufferType : int { RTC_BUFFER_TYPE_INDEX = 0, RTC_BUFFER_TYPE_VERTEX = 1 };
enum RTCFormat : int { RTC_FORMAT_UINT = 0x5001, RTC_FORMAT_FLOAT4 = 0x9004 };

struct alignas(16) RTCRay {
    float org_x, org_y, org_z, tnear;
    float dir_x, dir_y, dir_z, time;
    float tfar;
    unsigned int mask, id, flags;
};
struct alignas(16) RTCHit {
    float Ng_x, Ng_y, Ng_z;
    float u, v;
    unsigned int primID, geomID;
    unsigned int instID[1];
    unsigned int instPrimID[1];
};
// The 4.3.3 build may reserve multiple instance levels. Only the stable prefix is read.
struct RTCRayHit { RTCRay ray; RTCHit hit; unsigned int instancePadding[256]; };

struct EmbreeApi {
    HMODULE module = nullptr;
    RTCDevice (*newDevice)(const char*) = nullptr;
    void (*releaseDevice)(RTCDevice) = nullptr;
    RTCScene (*newScene)(RTCDevice) = nullptr;
    void (*releaseScene)(RTCScene) = nullptr;
    RTCGeometry (*newGeometry)(RTCDevice, RTCGeometryType) = nullptr;
    void* (*setNewGeometryBuffer)(RTCGeometry, RTCBufferType, unsigned int, RTCFormat, size_t, size_t) = nullptr;
    void (*commitGeometry)(RTCGeometry) = nullptr;
    unsigned int (*attachGeometry)(RTCScene, RTCGeometry) = nullptr;
    void (*releaseGeometry)(RTCGeometry) = nullptr;
    void (*commitScene)(RTCScene) = nullptr;
    void (*intersect1)(RTCScene, RTCRayHit*, void*) = nullptr;

    int (*deviceError)(RTCDevice) = nullptr;
    intptr_t (*deviceProperty)(RTCDevice,int) = nullptr;
    bool ready = false;
    bool Load() {
        if (module) return ready;
        std::vector<std::wstring> paths;
        if (const wchar_t* configured = _wgetenv(L"UNLEASHED_EMBREE_DLL")) paths.emplace_back(configured);
        paths.emplace_back(L"C:\\EdenDMA\\embree4.dll");
        paths.emplace_back(L"embree4.dll");
        for (const auto& path : paths) {
            module = path.find(L'\\') != std::wstring::npos
                ? LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)
                : LoadLibraryW(path.c_str());
            if (module) break;
        }
        if (!module) return false;
#define LOAD_EMBREE(member, symbol) \
        member = reinterpret_cast<decltype(member)>(GetProcAddress(module, #symbol))
        LOAD_EMBREE(deviceError, rtcGetDeviceError);
        LOAD_EMBREE(deviceProperty, rtcGetDeviceProperty);
        LOAD_EMBREE(newDevice, rtcNewDevice);
        LOAD_EMBREE(releaseDevice, rtcReleaseDevice);
        LOAD_EMBREE(newScene, rtcNewScene);
        LOAD_EMBREE(releaseScene, rtcReleaseScene);
        LOAD_EMBREE(newGeometry, rtcNewGeometry);
        LOAD_EMBREE(setNewGeometryBuffer, rtcSetNewGeometryBuffer);
        LOAD_EMBREE(commitGeometry, rtcCommitGeometry);
        LOAD_EMBREE(attachGeometry, rtcAttachGeometry);
        LOAD_EMBREE(releaseGeometry, rtcReleaseGeometry);
        LOAD_EMBREE(commitScene, rtcCommitScene);
        LOAD_EMBREE(intersect1, rtcIntersect1);
#undef LOAD_EMBREE
        ready = deviceError && deviceProperty && newDevice && releaseDevice && newScene && releaseScene && newGeometry &&
            setNewGeometryBuffer && commitGeometry && attachGeometry && releaseGeometry &&
            commitScene && intersect1;
        return ready;
    }
};

EmbreeApi& Embree() {
    static EmbreeApi api;
    return api;
}
#endif

}  // namespace

std::vector<WorldShape> BuildWorldShapes(const ShapeFrame& frame) {
    std::vector<WorldShape> output;
    const size_t count = frame.shapes.size();
    if (frame.poses.size() < count) return output;
    output.reserve(count);
    if (!std::isfinite(frame.worldScale) || frame.worldScale <= 0 || !Finite(frame.worldOffset)) return {};
    const float worldScale = frame.worldScale;
    for (size_t index = 0; index < count; ++index) {
        if (!frame.valid.empty() && (index >= frame.valid.size() || !frame.valid[index])) return {};
        const auto& source = frame.shapes[index];
        const auto& pose = frame.poses[index];
        if (!pose.Valid() || !std::isfinite(source.radius) || source.radius <= 0.0f) return {};
        WorldShape shape{};
        shape.a = ModelShapes::Transform(source.a, pose) * worldScale + frame.worldOffset;
        shape.b = ModelShapes::Transform(source.b, pose) * worldScale + frame.worldOffset;
        const float poseScale = (std::min)({pose.scale[0], pose.scale[1], pose.scale[2]});
        shape.radius = source.radius * poseScale * worldScale;
        shape.mappedBone = index < frame.shapeMapped.size() ? frame.shapeMapped[index] : -1;
        shape.critical = index < frame.critical.size()
            ? frame.critical[index] : source.critical;
        if (!ValidShape(shape, shape.radius)) return {};
        output.push_back(shape);
    }
    return output;
}

ShapeVisualState ClassifyVisual(int mappedBone, ModelShapes::CriticalState critical) {
    ShapeVisualState result{};
    result.bound = mappedBone >= 0;
    if (critical == ModelShapes::CriticalState::Critical) result.visual = VisualClass::Critical;
    else if (critical == ModelShapes::CriticalState::Ordinary) result.visual = VisualClass::Ordinary;
    else result.visual = VisualClass::Unknown;
    return result;
}

const char* BackendName(Backend backend) {
    switch (backend) {
        case Backend::NativeHitbox: return "native-hitbox";
        case Backend::Embree: return "embree";
        case Backend::UniCore: return "unicore";
    }
    return "unknown";
}

const char* StatusName(Status status) {
    switch (status) {
        case Status::Hit: return "hit";
        case Status::Miss: return "miss";
        case Status::UnknownData: return "unknown-data";
        case Status::BackendUnavailable: return "backend-unavailable";
        case Status::InvalidInput: return "invalid-input";
        case Status::InvalidScene: return "invalid-scene";
        case Status::Timeout: return "timeout";
        case Status::Error: return "error";
    }
    return "error";
}

Hit TraceNative(const Query& query, std::span<const WorldShape> shapes, TraceCalls* calls) {
    return TraceAnalytic(query, shapes, calls);
}

Hit TraceEmbree(const Query& query, std::span<const WorldShape> shapes, TraceCalls* calls) {
#if !defined(_WIN32)
    (void)query; (void)shapes;
    return {Status::BackendUnavailable};
#else
    Vector3 direction{};
    if (!ValidQuery(query, direction)) return {Status::InvalidInput};
    if (shapes.empty()) return {Status::UnknownData};
    auto& api = Embree();
    if (!api.Load()) return {Status::BackendUnavailable};
    RTCDevice device = api.newDevice(nullptr);
    if (!device) return {Status::BackendUnavailable};
    if (api.deviceProperty(device,0) != 40303) {
        api.releaseDevice(device);
        return {Status::BackendUnavailable};
    }
    RTCScene scene = api.newScene(device);
    if (!scene) { api.releaseDevice(device); return {Status::Error}; }

    const auto finish = [&](Hit hit) {
        api.releaseScene(scene);
        api.releaseDevice(device);
        return hit;
    };
    if (query.hitboxScalePercent == 0) {
        Hit miss{Status::Miss}; miss.distance = query.maxDistance;
        return finish(miss);
    }
    int inside = -1;
    std::vector<unsigned int> geometryToShape;
    for (size_t index = 0; index < shapes.size(); ++index) {
        const auto& shape = shapes[index];
        const float radius = EffectiveRadius(query, shape);
        if (!ValidShape(shape, radius)) return finish({Status::UnknownData});
        if (inside < 0 && ContainsOrigin(query.origin, shape.a, shape.b, radius))
            inside = static_cast<int>(index);
        const bool sphere = (shape.a - shape.b).Size() <= 1.0e-5f;
        RTCGeometry geometry = api.newGeometry(device, sphere
            ? RTC_GEOMETRY_TYPE_SPHERE_POINT : RTC_GEOMETRY_TYPE_ROUND_LINEAR_CURVE);
        if (!geometry) { api.releaseScene(scene); api.releaseDevice(device); return {Status::Error}; }
        const size_t vertexCount = sphere ? 1 : 2;
        auto* vertices = static_cast<float*>(api.setNewGeometryBuffer(
            geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, sizeof(float) * 4, vertexCount));
        if (!vertices) {
            api.releaseGeometry(geometry); api.releaseScene(scene); api.releaseDevice(device);
            return {Status::Error};
        }
        vertices[0] = shape.a.X; vertices[1] = shape.a.Y; vertices[2] = shape.a.Z; vertices[3] = radius;
        if (!sphere) {
            vertices[4] = shape.b.X; vertices[5] = shape.b.Y; vertices[6] = shape.b.Z; vertices[7] = radius;
            auto* indices = static_cast<unsigned int*>(api.setNewGeometryBuffer(
                geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT, sizeof(unsigned int), 1));
            if (!indices) {
                api.releaseGeometry(geometry); api.releaseScene(scene); api.releaseDevice(device);
                return {Status::Error};
            }
            indices[0] = 0;
        }
        api.commitGeometry(geometry);
        const unsigned int geometryId = api.attachGeometry(scene, geometry);
        api.releaseGeometry(geometry);
        if (geometryId == 0xFFFFFFFFu || api.deviceError(device) != 0) return finish({Status::Error});
        if (geometryId >= geometryToShape.size()) geometryToShape.resize(size_t(geometryId)+1,0xFFFFFFFFu);
        geometryToShape[geometryId] = static_cast<unsigned int>(index);
    }
    if (geometryToShape.empty()) {
        api.releaseScene(scene); api.releaseDevice(device); return {Status::UnknownData};
    }
    api.commitScene(scene);
    if (api.deviceError(device) != 0) return finish({Status::Error});
    RTCRayHit rayhit{};
    rayhit.ray.org_x = query.origin.X; rayhit.ray.org_y = query.origin.Y; rayhit.ray.org_z = query.origin.Z;
    rayhit.ray.tnear = 0.0f;
    rayhit.ray.dir_x = direction.X; rayhit.ray.dir_y = direction.Y; rayhit.ray.dir_z = direction.Z;
    rayhit.ray.time = 0.0f; rayhit.ray.tfar = query.maxDistance; rayhit.ray.mask = 0xFFFFFFFFu;
    rayhit.ray.id = 0; rayhit.ray.flags = 0;
    rayhit.hit.geomID = 0xFFFFFFFFu; rayhit.hit.primID = 0xFFFFFFFFu;
    if (calls) ++calls->embreeIntersections;
    api.intersect1(scene, &rayhit, nullptr);
    if (api.deviceError(device) != 0) return finish({Status::Error});
    if (inside >= 0) return finish({Status::Hit,inside,0,query.origin,EffectiveRadius(query,shapes[inside])});
    Hit result{Status::Miss};
    result.distance = query.maxDistance;
    if (rayhit.hit.geomID != 0xFFFFFFFFu && rayhit.ray.tfar >= 0.0f && rayhit.ray.tfar <= query.maxDistance) {
        const size_t geometryIndex = rayhit.hit.geomID;
        if (geometryIndex >= geometryToShape.size() || geometryToShape[geometryIndex] >= shapes.size())
            return finish({Status::Error});
        {
            result.status = Status::Hit;
            result.shapeIndex = static_cast<int>(geometryToShape[geometryIndex]);
            result.distance = rayhit.ray.tfar;
            result.point = query.origin + direction * result.distance;
            result.effectiveRadius = EffectiveRadius(query, shapes[result.shapeIndex]);
        }
    }
    return finish(result);
#endif
}

Hit TraceUniCore(const Query& query, EngineTrace::Service* service, uint64_t sceneEpoch,
                uint32_t budgetMs, TraceCalls* calls) {
    Vector3 direction{};
    if (!ValidQuery(query, direction)) return {Status::InvalidInput};
    if (!service) return {Status::BackendUnavailable};
    EngineTrace::Request request{};
    request.start = {query.origin.X, query.origin.Y, query.origin.Z};
    const Vector3 end = query.origin + direction * query.maxDistance;
    request.end = {end.X, end.Y, end.Z};
    request.sceneEpoch = sceneEpoch;
    request.budgetMs = budgetMs;
    if (!Finite(end)) return {Status::InvalidInput};
    if (calls) ++calls->worldRequests;
    const auto result = service->Trace(request);
    switch (result.status) {
        case EngineTrace::Status::Ok: {
            Hit hit{};
            if (!std::isfinite(result.fraction) || result.fraction < 0 || result.fraction > 1 ||
                !Finite({result.hitPosition.x,result.hitPosition.y,result.hitPosition.z})) return {Status::Error};
            hit.status = result.fraction < 1.0f ? Status::Hit : Status::Miss;
            hit.distance = result.fraction * query.maxDistance;
            hit.point = {result.hitPosition.x, result.hitPosition.y, result.hitPosition.z};
            return hit;
        }
        case EngineTrace::Status::InvalidScene: return {Status::InvalidScene};
        case EngineTrace::Status::Timeout: return {Status::Timeout};
        case EngineTrace::Status::Unavailable: return {Status::BackendUnavailable};
        case EngineTrace::Status::Error: return {Status::Error};
    }
    return {Status::Error};
}

Hit TraceSelected(Backend backend, const Query& query, std::span<const WorldShape> shapes,
                  EngineTrace::Service* service, uint64_t sceneEpoch, uint32_t budgetMs, TraceCalls* calls) {
    switch (backend) {
        case Backend::NativeHitbox:
            return TraceNative(query, shapes, calls);
        case Backend::Embree:
            return TraceEmbree(query, shapes, calls);
        case Backend::UniCore:
            return TraceUniCore(query, service, sceneEpoch, budgetMs, calls);
    }
    return {Status::Error};
}

}  // namespace OW::V3Raycast
