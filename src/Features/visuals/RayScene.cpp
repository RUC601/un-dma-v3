#define NOMINMAX
#define _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING
#include "RayScene.hpp"
#include "ShapeWire.hpp"
#include "Game/V3Raycast.hpp"
#include "Game/snapshots/SnapshotAccess.hpp"
#include "config/UiPreferences.hpp"
#include "Utils/Diagnostics.hpp"
#include <rapidjson/document.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>

namespace V3::RayScene {
namespace {
using namespace OW;
using Json = rapidjson::Value;

const Json& Field(const Json& obj, const char* key) {
    static const Json missing;
    return obj.IsObject() && obj.HasMember(key) ? obj[key] : missing;
}
bool Number(const Json& value, float& out) {
    if (!value.IsNumber()) return false;
    out = value.GetFloat();
    return std::isfinite(out);
}
bool Vector(const Json& value, Vector3& out) {
    return value.IsArray() && value.Size() == 3 && Number(value[0], out.X) &&
        Number(value[1], out.Y) && Number(value[2], out.Z);
}
struct RecordedQuery final : EngineTrace::IBackend {
    uint64_t epoch = 0;
    uint64_t mask = 0;
    bool sameScene = false;
    Vector3 start{}, end{};
    EngineTrace::Result saved{};
    const char* reason = "missing-record";

    const char* Name() const override { return "saved-unicore-result"; }
    bool BindScene(uint64_t sceneEpoch) override { return sameScene && epoch == sceneEpoch; }
    void ReleaseScene() override {}
    EngineTrace::Status Trace(const EngineTrace::Request& request, EngineTrace::Result& result) override {
        // Endpoints reconstructed from a normalized direction can differ by one float ULP.
        auto matches = [](EngineTrace::Vec3 a, Vector3 b) {
            return (Vector3{a.x,a.y,a.z} - b).Size() <= 0.00001f;
        };
        if (mask != request.mask || !matches(request.start, start) || !matches(request.end, end)) {
            reason = "unrecorded-segment-or-mask";
            return EngineTrace::Status::Unavailable;
        }
        result = saved;
        reason = "matched-saved-query";
        return saved.status;
    }
};
struct Scene {
    std::string id;
    uint64_t epoch = 0;
    uint64_t frame = 0;
    std::vector<V3Raycast::WorldShape> shapes;
    V3Raycast::Query query;
    RecordedQuery record;
    EngineTrace::Service service;
    bool hasQuery = false;
    int lastSelection = -1;
    unsigned revision = 0;
};
std::unique_ptr<Scene> scene;
uint64_t lastRuntimeSequence = 0;
DWORD lastRuntimeProbeTick = 0;
int lastRuntimeBackend = -1;

bool ReadMetadata(const Json& binding, const Json& marker, V3Raycast::WorldShape& shape) {
    if (!binding.IsNull()) {
        if (!binding.IsInt() || binding.GetInt() < -1) return false;
        shape.mappedBone = binding.GetInt();
    }
    if (!marker.IsNull()) {
        if (!marker.IsInt64() && !marker.IsUint64()) return false;
        shape.critical = marker.GetDouble() == 0 ? ModelShapes::CriticalState::Ordinary :
            ModelShapes::CriticalState::Critical;
    }
    return true;
}

bool ReadComparison(const Json& doc, Scene& out) {
    const auto& time = Field(Field(doc,"capture_metadata"),"time");
    const auto& actors = Field(doc,"actors");
    if (!time.IsString() || !actors.IsArray()) return false;
    out.id = time.GetString();
    size_t omitted = 0;
    for (const auto& actor : actors.GetArray()) {
        const auto& items = Field(actor,"native_shapes");
        if (items.IsNull()) continue;
        if (!items.IsArray()) return false;
        for (const auto& item : items.GetArray()) {
            const auto& radius = Field(item,"radius_transform");
            const auto& usable = Field(radius,"single_world_radius_valid");
            V3Raycast::WorldShape shape;
            if (!usable.IsTrue() || !Number(Field(radius,"world_radius"),shape.radius) || shape.radius <= 0 ||
                !Vector(Field(item,"full_qst_world_a"),shape.a) || !Vector(Field(item,"full_qst_world_b"),shape.b)) {
                ++omitted;
                continue;
            }
            if (!ReadMetadata(Field(item,"bone_index"),Field(item,"critical"),shape)) return false;
            if (out.shapes.size() >= 2048) return false;
            out.shapes.push_back(shape);
        }
    }
    Diagnostics::Info("[RAY-IMPORT] time=%s shapes=%zu unusableGeometry=%zu source=model-hitbox-offline-comparison/v1",
                      out.id.c_str(),out.shapes.size(),omitted);
    return true;
}

bool ParseShapes(const Json& items, Scene& out) {
    if (!items.IsArray() || items.Size() > 2048) return false;
    for (const auto& item : items.GetArray()) {
        V3Raycast::WorldShape shape;
        if (!Vector(Field(item,"a"),shape.a) || !Vector(Field(item,"b"),shape.b) ||
            !Number(Field(item,"radius"),shape.radius) || shape.radius <= 0) return false;
        if (!ReadMetadata(Field(item,"shapeMapped"),Field(item,"critical"),shape)) return false;
        out.shapes.push_back(shape);
    }
    return true;
}

bool ParseRecord(const Json& item, Scene& out) {
    auto& r = out.record;
    const auto& id = Field(item,"scene");
    if (!id.IsString() || !Field(item,"epoch").IsUint64() ||
        !Field(item,"mask").IsUint64() || !Vector(Field(item,"start"),r.start) ||
        !Vector(Field(item,"end"),r.end) || !Number(Field(item,"fraction"),r.saved.fraction))
        return false;
    Vector3 hit{};
    if (!Vector(Field(item,"hitPosition"),hit)) return false;
    r.saved.hitPosition = {hit.X,hit.Y,hit.Z};
    r.sameScene = out.id == id.GetString();
    r.saved.status = EngineTrace::Status::Ok;
    r.epoch = item["epoch"].GetUint64();
    r.mask = item["mask"].GetUint64();
    out.service.SetBackend(&r);
    if (!out.service.OnSceneChanged(out.epoch)) r.reason = r.sameScene ? "record-epoch-mismatch" : "record-scene-mismatch";
    return true;
}

bool ReadScene(const Json& doc, Scene& out) {
    if (!Field(doc,"scene").IsString() || !Field(doc,"epoch").IsUint64() ||
        !Field(doc,"frame").IsUint64() || !ParseShapes(Field(doc,"shapes"),out)) return false;
    out.id = doc["scene"].GetString();
    out.epoch = doc["epoch"].GetUint64();
    out.frame = doc["frame"].GetUint64();
    const auto& query = Field(doc,"query");
    auto& q = out.query;
    Vector3 end;
    if (!Vector(Field(query,"start"),q.origin) || !Vector(Field(query,"end"),end)) return false;
    q.direction = end-q.origin;
    q.maxDistance = q.direction.Size();
    if (query.HasMember("projectileRadius") && !Number(query["projectileRadius"],q.projectileRadius)) return false;
    if (query.HasMember("scalePercent") && !Number(query["scalePercent"],q.hitboxScalePercent)) return false;
    const auto& record = Field(doc,"record");
    if (!record.IsNull() && !ParseRecord(record,out)) return false;
    out.hasQuery = true;
    return true;
}

bool DrawRuntimeProbe() {
    OW::ScopedConsumptionFrame scope;
    const auto& frame = scope.Get();
    const int selectedValue = (std::clamp)(UI::Preferences::values.rayBackend,0,1);
    if (frame.sequence == 0 ||
        (frame.sequence == lastRuntimeSequence && selectedValue == lastRuntimeBackend) ||
        GetTickCount() - lastRuntimeProbeTick < 100) return false;
    lastRuntimeSequence = frame.sequence;
    lastRuntimeProbeTick = GetTickCount();
    const OW::c_entity* target = nullptr;
    for (const auto& candidate : frame.entities) {
        if (!candidate.native_hitboxes.Matches(candidate.VelocityBase, candidate.skeleton_revision))
            continue;
        if (candidate.pos == frame.local.pos) continue;
        target = &candidate;
        break;
    }
    if (!target) return false;
    std::vector<V3Raycast::WorldShape> shapes;
    const auto& sample = target->native_hitboxes;
    const auto basis = [&](int column) {
        return Vector3{sample.world.matrix[0][column],sample.world.matrix[1][column],sample.world.matrix[2][column]}.Size();
    };
    const float worldScale = (std::min)({basis(0),basis(1),basis(2)});
    if (!std::isfinite(worldScale) || worldScale <= 0) return false;
    shapes.reserve(sample.shapes.size());
    for (size_t i=0; i<sample.shapes.size() && i<sample.poses.size(); ++i) {
        const auto& source = sample.shapes[i];
        const auto& pose = sample.poses[i];
        const float poseScale = (std::min)({pose.scale[0],pose.scale[1],pose.scale[2]});
        if (!pose.Valid() || !std::isfinite(poseScale) || poseScale <= 0) continue;
        V3Raycast::WorldShape shape{};
        shape.a = sample.world.TransformPoint(ModelShapes::Transform(source.a,pose));
        shape.b = sample.world.TransformPoint(ModelShapes::Transform(source.b,pose));
        shape.radius = source.radius * poseScale * worldScale;
        shape.mappedBone = source.boneId;
        shape.critical = source.critical;
        shapes.push_back(shape);
    }
    const Vector3 delta = target->pos - frame.local.pos;
    const float distance = delta.Size();
    if (!std::isfinite(distance) || distance <= 0) return false;
    V3Raycast::Query query{frame.local.pos,delta,distance,0,100};
    V3Raycast::TraceCalls calls{};
    const auto selected = static_cast<V3Raycast::Backend>(selectedValue);
    const auto hit = V3Raycast::TraceSelected(selected,query,shapes,nullptr,0,0,&calls);
    lastRuntimeBackend = selectedValue;
    Diagnostics::Info("[RAY-RUNTIME] sequence=%llu backend=%s analytic=%u serviceTrace=%u result=%s shape=%d source=native-hitbox.sample",
        static_cast<unsigned long long>(frame.sequence),V3Raycast::BackendName(selected),
        calls.analyticTests,calls.worldRequests,V3Raycast::StatusName(hit.status),hit.shapeIndex);
    return false;
}

void DrawShapes(const Scene& input) {
    if (input.shapes.empty()) return;
    // Orthographic view of the supplied world coordinates. Preserve the model's proportions.
    auto horizontal = [](Vector3 p) { return p.X * 0.966f + p.Z * 0.259f; };
    auto vertical = [](Vector3 p) { return p.Y * 0.985f + (p.X * 0.259f - p.Z * 0.966f) * 0.174f; };
    float left = FLT_MAX, right = -FLT_MAX, top = -FLT_MAX, bottom = FLT_MAX;
    for (const auto& shape : input.shapes) {
        for (auto p : {shape.a,shape.b}) {
            left = (std::min)(left,horizontal(p)-shape.radius);
            right = (std::max)(right,horizontal(p)+shape.radius);
            bottom = (std::min)(bottom,vertical(p)-shape.radius);
            top = (std::max)(top,vertical(p)+shape.radius);
        }
    }
    const float width = OW::ScreenWidth(), height = OW::ScreenHeight();
    const float scale = (std::min)(width * .72f / (std::max)(.01f,right-left),
                                  height * .62f / (std::max)(.01f,top-bottom));
    auto project = [&](Vector3 p, Vector2& screen) {
        screen = {width*.5f + (horizontal(p)-(left+right)*.5f)*scale,
                  height*.5f - (vertical(p)-(top+bottom)*.5f)*scale};
        return true;
    };
    for (const auto& shape : input.shapes) {
        const ModelShapes::Shape wire{shape.mappedBone,shape.a,shape.b,shape.radius,shape.critical};
        const auto color = V3::Visuals::ClassColor(V2::VisualSettings::HitboxPalette{1.f}, wire.critical, 1.f);
        V3::Visuals::DrawShapeWire(wire, [](Vector3 p){ return p; }, project, color, 2.0f);
    }
}
}
bool Open(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0 || file.tellg() > 4*1024*1024) return false;
    std::string data(static_cast<size_t>(file.tellg()),'\0');
    file.seekg(0);
    if (!file.read(data.data(),data.size())) return false;
    rapidjson::Document doc;
    doc.Parse(data.data(),data.size());
    auto next = std::make_unique<Scene>();
    if (doc.HasParseError() || !doc.IsObject()) return false;
    const auto& schema = Field(doc,"schema");
    if (schema.IsString() && std::string_view(schema.GetString()) == "model-hitbox-offline-comparison/v1") {
        if (!ReadComparison(doc,*next)) return false;
    } else if (!ReadScene(doc,*next)) return false;
    for (size_t i=0;i<next->shapes.size();++i) {
        const auto& shape=next->shapes[i];
        Diagnostics::Info("[RAY-SHAPE] scene=%s frame=%llu index=%zu shapeMapped=%d critical=%s",
            next->id.c_str(),next->frame,i,shape.mappedBone,ModelShapes::CriticalStateName(shape.critical));
    }
    Diagnostics::Info("[RAY-OPEN] scene=%s frame=%llu source=%s",next->id.c_str(),next->frame,path.filename().string().c_str());
    scene = std::move(next);
    return true;
}

void Close() {
    if (scene)
        Diagnostics::Info("[RAY-CLOSE] scene=%s frame=%llu", scene->id.c_str(), scene->frame);
    scene.reset();
    lastRuntimeSequence = 0;
    lastRuntimeProbeTick = 0;
    lastRuntimeBackend = -1;
}

bool IsOpen() {
    return scene != nullptr;
}

bool Draw() {
    if (!scene) {
        DrawRuntimeProbe();
        return false;
    }
    const int selected = (std::clamp)(UI::Preferences::values.rayBackend,0,1);
    if (scene->lastSelection != selected && !scene->hasQuery) {
        scene->lastSelection = selected;
        Diagnostics::Info("[RAY] scene=%s selected=%s call=none result=unknown-data reason=no-recorded-segment",
            scene->id.c_str(),V3Raycast::BackendName(static_cast<V3Raycast::Backend>(selected)));
    }
    if (scene->lastSelection != selected) {
        const auto backend = static_cast<V3Raycast::Backend>(selected);
        V3Raycast::TraceCalls calls{};
        const auto hit = V3Raycast::TraceSelected(backend,scene->query,scene->shapes,&scene->service,scene->epoch,0,&calls);
        scene->lastSelection = selected;
        ++scene->revision;
        Diagnostics::Info("[RAY] scene=%s frame=%llu epoch=%llu revision=%u selected=%s analytic=%u serviceTrace=%u result=%s shape=%d distance=%.8f radius=%.8f reason=%s",
            scene->id.c_str(),scene->frame,scene->epoch,scene->revision,V3Raycast::BackendName(backend),
            calls.analyticTests,calls.worldRequests,V3Raycast::StatusName(hit.status),hit.shapeIndex,hit.distance,hit.effectiveRadius,
            backend == V3Raycast::Backend::UniCore ? scene->record.reason :
            hit.status == V3Raycast::Status::UnknownData ? "missing-or-invalid-shapes" : "local-geometry");
    }
    DrawShapes(*scene);
    return true;
}
}
