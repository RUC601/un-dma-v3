#include "Game/V3Raycast.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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
        case Backend::UniCore:
            return TraceUniCore(query, service, sceneEpoch, budgetMs, calls);
    }
    return {Status::Error};
}

}  // namespace OW::V3Raycast
