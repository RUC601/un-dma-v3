#include "Game/V3Raycast.hpp"
#include "Game/Offsets.hpp"
#include "Game/RayDecision.hpp"
#include "Game/RayQueryBroker.hpp"
#include "Game/TrajectoryRayPlanner.hpp"
#include "Game/GroundClip.hpp"
#include "config/RayBackendMigration.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using OW::ModelShapes::CriticalState;
using OW::V3Raycast::Backend;
using OW::V3Raycast::Query;
using OW::V3Raycast::ShapeFrame;
using OW::V3Raycast::Status;
using OW::V3Raycast::WorldShape;

bool Near(float lhs, float rhs, float epsilon = 0.05f) {
    return std::fabs(lhs - rhs) <= epsilon;
}

int Fail(const char* message) {
    std::cerr << "FAIL " << message << '\n';
    return EXIT_FAILURE;
}

const char* CriticalText(CriticalState state) {
    return OW::ModelShapes::CriticalStateName(state);
}

void WriteSvg(const std::string& path, const std::vector<WorldShape>& shapes) {
    std::ofstream svg(path, std::ios::binary);
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"640\" viewBox=\"0 0 1000 640\">\n";
    svg << "<rect width=\"1000\" height=\"640\" fill=\"#10151c\"/>\n";
    svg << "<text x=\"40\" y=\"46\" fill=\"#f5f7fa\" font-family=\"Segoe UI\" font-size=\"26\">V3 shapeMapped / critical replay fixture</text>\n";
    svg << "<text x=\"40\" y=\"76\" fill=\"#9aa9b8\" font-family=\"Segoe UI\" font-size=\"15\">Only the shape with an explicit critical value is emphasized</text>\n";
    const float originX = 500.0f;
    const float originY = 355.0f;
    const float scale = 160.0f;
    svg << "<line x1=\"80\" y1=\"355\" x2=\"920\" y2=\"355\" stroke=\"#344352\"/>\n";
    svg << "<line x1=\"500\" y1=\"120\" x2=\"500\" y2=\"560\" stroke=\"#344352\"/>\n";
    for (size_t index = 0; index < shapes.size(); ++index) {
        const auto& shape = shapes[index];
        const auto visual = OW::V3Raycast::ClassifyVisual(shape.mappedBone, shape.critical);
        const char* color = visual.visual == OW::V3Raycast::VisualClass::Critical ? "#ff503b"
            : visual.visual == OW::V3Raycast::VisualClass::Ordinary ? "#66b3ff" : "#a5afba";
        const auto project = [&](OW::Vector3 value) {
            return std::pair<float, float>{originX + value.X * scale, originY - value.Y * scale};
        };
        const auto pa = project(shape.a);
        const auto pb = project(shape.b);
        const auto center = project((shape.a + shape.b) * .5f);
        const float radius = shape.radius * scale;
        if ((shape.a - shape.b).Size() <= 1.0e-5f) {
            svg << "<circle cx=\"" << center.first << "\" cy=\"" << center.second
                << "\" r=\"" << radius << "\" fill=\"none\" stroke=\"" << color
                << "\" stroke-width=\"4\"/>\n";
        } else {
            svg << "<line x1=\"" << pa.first << "\" y1=\"" << pa.second << "\" x2=\""
                << pb.first << "\" y2=\"" << pb.second << "\" stroke=\"" << color
                << "\" stroke-width=\"" << radius * 2.0f << "\" stroke-linecap=\"round\"/>\n";
            svg << "<circle cx=\"" << pa.first << "\" cy=\"" << pa.second << "\" r=\""
                << radius << "\" fill=\"none\" stroke=\"" << color << "\" stroke-width=\"4\"/>\n";
            svg << "<circle cx=\"" << pb.first << "\" cy=\"" << pb.second << "\" r=\""
                << radius << "\" fill=\"none\" stroke=\"" << color << "\" stroke-width=\"4\"/>\n";
        }
        svg << "<text x=\"" << center.first + 18.0f << "\" y=\"" << center.second - 12.0f
            << "\" fill=\"" << color << "\" font-family=\"Segoe UI\" font-size=\"15\">shape "
            << index << " · " << (shape.mappedBone >= 0 ? "bound" : "unbound") << " · "
            << CriticalText(shape.critical) << "</text>\n";
    }
    svg << "<rect x=\"70\" y=\"540\" width=\"18\" height=\"18\" fill=\"#ff503b\"/><text x=\"98\" y=\"555\" fill=\"#f5f7fa\" font-family=\"Segoe UI\" font-size=\"15\">explicit critical marker</text>\n";
    svg << "<rect x=\"300\" y=\"540\" width=\"18\" height=\"18\" fill=\"#66b3ff\"/><text x=\"328\" y=\"555\" fill=\"#f5f7fa\" font-family=\"Segoe UI\" font-size=\"15\">critical = 0</text>\n";
    svg << "<rect x=\"480\" y=\"540\" width=\"18\" height=\"18\" fill=\"#a5afba\"/><text x=\"508\" y=\"555\" fill=\"#f5f7fa\" font-family=\"Segoe UI\" font-size=\"15\">critical field missing</text>\n";
    svg << "</svg>\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<OW::ModelShapes::Shape> shapes{
        {17, {-0.85f, 0.10f, 5.0f}, {-0.85f, 0.10f, 5.0f}, 0.22f},
        {17, {0.0f, 0.05f, 5.0f}, {0.0f, 0.05f, 5.0f}, 0.18f},
        {17, {0.85f, 0.10f, 5.0f}, {0.85f, 0.10f, 5.0f}, 0.22f},
        {17, {-0.10f, -0.55f, 5.0f}, {0.25f, -0.55f, 5.0f}, 0.12f},
    };
    std::vector<OW::ModelShapes::Pose> poses(shapes.size());
    for (auto& pose : poses) {
        pose.q[3] = 1.0f;
        pose.scale[0] = pose.scale[1] = pose.scale[2] = 1.0f;
    }
    const std::vector<uint8_t> valid(shapes.size(), 1);
    const std::vector<int> mapped{4, -1, 8, 9};
    const std::vector<CriticalState> critical{
        CriticalState::Unknown, CriticalState::Critical, CriticalState::Ordinary, CriticalState::Ordinary};
    const ShapeFrame frame{shapes, poses, valid, mapped, critical, {}, 1.0f};
    const auto worldShapes = OW::V3Raycast::BuildWorldShapes(frame);
    if (worldShapes.size() != shapes.size()) return Fail("shape frame was not preserved");
    if (OW::V3Raycast::ClassifyVisual(mapped[0], critical[0]).visual != OW::V3Raycast::VisualClass::Unknown ||
        OW::V3Raycast::ClassifyVisual(mapped[1], critical[1]).visual != OW::V3Raycast::VisualClass::Critical ||
        OW::V3Raycast::ClassifyVisual(mapped[2], critical[2]).visual != OW::V3Raycast::VisualClass::Ordinary ||
        OW::V3Raycast::ClassifyVisual(mapped[1], critical[1]).bound) {
        return Fail("binding and critical metadata were conflated");
    }

    const Query straight{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 10.0f, 0.05f, 100.0f};
    const auto native = OW::V3Raycast::TraceNative(straight, worldShapes);
    if (native.status != Status::Hit || native.shapeIndex != 1 || !Near(native.distance, 4.77f))
        return Fail("native capsule/sphere query");
    const auto selectedNative = OW::V3Raycast::TraceSelected(Backend::NativeHitbox, straight, worldShapes);
    if (selectedNative.status != native.status || selectedNative.shapeIndex != native.shapeIndex ||
        !Near(selectedNative.distance, native.distance)) {
        return Fail("native selector dispatch");
    }
    std::cout << "nativeDistance=" << std::fixed << std::setprecision(5) << native.distance << '\n';

    const ShapeFrame scaledFrame{shapes, poses, valid, mapped, critical, {}, 1.4512421f};
    const auto scaledShapes = OW::V3Raycast::BuildWorldShapes(scaledFrame);
    const Query scaledQuery{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 10.0f, 0.05f, 150.0f};
    const auto scaledNative = OW::V3Raycast::TraceNative(scaledQuery, scaledShapes);
    if (scaledNative.status != Status::Hit || scaledNative.shapeIndex != 1 ||
        !(scaledNative.effectiveRadius > native.effectiveRadius)) {
        return Fail("non-unit scale or hitbox multiplier");
    }
    if (OW::V3Raycast::TraceNative(straight, {}).status != Status::UnknownData ||
        OW::V3Raycast::TraceNative(Query{straight.origin, straight.direction, straight.maxDistance, straight.projectileRadius, 151.0f}, worldShapes).status != Status::InvalidInput ||
        OW::V3Raycast::TraceSelected(Backend::UniCore, straight, worldShapes).status != Status::BackendUnavailable) {
        return Fail("unknown and selector states");
    }

    // Exercise each primitive independently, including the closed end of the segment.
    const WorldShape capsule{{-1,0,5},{1,0,5},0.5f};
    const WorldShape sphere{{0,0,5},{0,0,5},0.5f};
    {
        const auto backend = Backend::NativeHitbox;
        for (const auto& primitive : {sphere,capsule}) {
            const bool isSphere = primitive.a == primitive.b;
            Query q{{0,0,0},{0,0,1},10,.125f,120};
            auto check = [&](Status expected, float distance) {
                const auto result = OW::V3Raycast::TraceSelected(backend,q,{&primitive,1});
                std::cout << OW::V3Raycast::BackendName(backend) << ' ' << (isSphere ? "sphere" : "capsule")
                          << ' ' << OW::V3Raycast::StatusName(result.status) << " distance=" << result.distance << '\n';
                return result.status == expected && Near(result.distance,distance,0.0001f);
            };
            if (!check(Status::Hit,4.25f)) return Fail("independent primitive contact");
            q.maxDistance = 4.25f;
            if (!check(Status::Hit,4.25f)) return Fail("closed segment endpoint");
            q.maxDistance = 4.f;
            if (!check(Status::Miss,4.f)) return Fail("shape beyond endpoint");
            q.origin = {0,0,5.1f};
            if (!check(Status::Hit,0.f)) return Fail("inside primitive directed away from center");
        }
        const auto scaledHit = OW::V3Raycast::TraceSelected(backend,scaledQuery,scaledShapes);
        if (scaledHit.status != Status::Hit || scaledHit.shapeIndex != scaledNative.shapeIndex ||
            !Near(scaledHit.distance,scaledNative.distance,0.0001f)) return Fail("scaled backend parity");
    }
    class SavedResult final : public OW::EngineTrace::IBackend {
    public:
        OW::EngineTrace::Status status = OW::EngineTrace::Status::Ok;
        float fraction = .304232895f;
        const char* Name() const override { return "fixture"; }
        bool BindScene(uint64_t epoch) override { return epoch == 7; }
        void ReleaseScene() override {}
        OW::EngineTrace::Status Trace(const OW::EngineTrace::Request&, OW::EngineTrace::Result& out) override {
            out.fraction = fraction;
            return status;
        }
    } saved;
    OW::EngineTrace::Service service(&saved);
    if (!service.OnSceneChanged(7)) return Fail("scene bind");
    auto consume = [&](uint64_t epoch) { return OW::V3Raycast::TraceSelected(Backend::UniCore,straight,worldShapes,&service,epoch); };
    if (consume(8).status != Status::InvalidScene || !Near(consume(7).distance,3.04232895f,0.00001f))
        return Fail("scene epoch and saved fraction");
    for (float invalid : {NAN,-.01f,1.01f}) {
        saved.fraction = invalid;
        if (consume(7).status != Status::Error) return Fail("invalid successful fraction");
    }
    saved.fraction = 1.f;
    if (consume(7).status != Status::Miss) return Fail("clear recorded segment");
    for (const auto [input,expected] : {std::pair{OW::EngineTrace::Status::Timeout,Status::Timeout},
            std::pair{OW::EngineTrace::Status::Unavailable,Status::BackendUnavailable},
            std::pair{OW::EngineTrace::Status::Error,Status::Error}}) {
        saved.status = input;
        if (consume(7).status != expected) return Fail("failed world query");
    }
    if (!OW::RayDecision{OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Clear}.AllowsHybrid() ||
        OW::RayDecision{OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Occluded}.AllowsHybrid() ||
        OW::RayDecision{OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Unknown}.AllowsHybrid() ||
        OW::RayDecision{OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Stale}.AllowsHybrid() ||
        OW::RayDecision{OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Unavailable}.AllowsHybrid() ||
        OW::RayDecision{OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Timeout}.AllowsHybrid() ||
        OW::RayDecision{OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Invalid}.AllowsHybrid() ||
        OW::RayDecision{OW::RayDecisionStatus::Miss, OW::RayDecisionStatus::Clear}.AllowsNativeOnly() ||
        !OW::RayDecisionAllows({OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Unknown},
                               static_cast<int>(OW::RayDecisionPolicy::NativeOnly)) ||
        OW::RayDecisionAllows({OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Unknown},
                              static_cast<int>(OW::RayDecisionPolicy::Hybrid)) ||
        !OW::RayDecisionAllowsSelection({OW::RayDecisionStatus::Unknown, OW::RayDecisionStatus::Unknown},
                                        true, static_cast<int>(OW::RayDecisionPolicy::Hybrid)) ||
        !OW::RayDecisionAllowsSelection({OW::RayDecisionStatus::Unknown, OW::RayDecisionStatus::Clear},
                                        false, static_cast<int>(OW::RayDecisionPolicy::Hybrid)) ||
        OW::RayDecisionAllowsSelection({OW::RayDecisionStatus::Unknown, OW::RayDecisionStatus::Occluded},
                                       false, static_cast<int>(OW::RayDecisionPolicy::Hybrid)) ||
        OW::RayDecisionAllowsSelection({OW::RayDecisionStatus::Miss, OW::RayDecisionStatus::Clear},
                                       false, static_cast<int>(OW::RayDecisionPolicy::Hybrid)) ||
        !OW::RayDecisionAllowsSelection({OW::RayDecisionStatus::Unknown, OW::RayDecisionStatus::Unknown},
                                        false, static_cast<int>(OW::RayDecisionPolicy::NativeOnly)) ||
        OW::RayDecisionAllowsSelection({OW::RayDecisionStatus::Miss, OW::RayDecisionStatus::Clear},
                                       false, static_cast<int>(OW::RayDecisionPolicy::NativeOnly)))
        return Fail("ray decision readiness");
    // Hybrid without a bound service or without a live scene can never obtain world occlusion,
    // so the settings page must announce it; Native-only never needs the notice.
    {
        const int native = static_cast<int>(OW::RayDecisionPolicy::NativeOnly);
        const int hybrid = static_cast<int>(OW::RayDecisionPolicy::Hybrid);
        if (OW::RayPolicyNeedsServiceNotice(native, false, 0) ||
            OW::RayPolicyNeedsServiceNotice(native, true, 7) ||
            OW::RayPolicyNeedsServiceNotice(hybrid, true, 7) ||
            !OW::RayPolicyNeedsServiceNotice(hybrid, false, 7) ||
            !OW::RayPolicyNeedsServiceNotice(hybrid, true, 0) ||
            !OW::RayPolicyNeedsServiceNotice(hybrid, false, 0))
            return Fail("ray policy service notice");
    }
    // Ground-plane clipping. World Y grows upward, so "below the ground" is Y < groundY.
    {
        const float ground = 0.0f;
        // Entirely above: untouched.
        if (OW::ClipSegmentToGround({0, 1, 0}, {0, 2, 0}, ground).clipped)
            return Fail("ground clip above");
        // Ending exactly on the plane is not below it.
        if (OW::ClipSegmentToGround({0, 1, 0}, {0, 0, 0}, ground).clipped)
            return Fail("ground clip on plane");
        // Horizontal segment above the plane.
        if (OW::ClipSegmentToGround({0, 1, 0}, {4, 1, 0}, ground).clipped)
            return Fail("ground clip horizontal");
        // Crossing: clipped at the mid-point.
        const auto cross = OW::ClipSegmentToGround({0, 2, 0}, {0, -2, 0}, ground);
        if (!cross.clipped || !Near(cross.point.X, 0.f) || !Near(cross.point.Y, 0.f) ||
            !Near(cross.point.Z, 0.f))
            return Fail("ground clip crossing");
        // Origin already under the plane.
        const auto under = OW::ClipSegmentToGround({1, -1, 2}, {1, -2, 2}, ground);
        if (!under.clipped || !Near(under.point.X, 1.f) || !Near(under.point.Y, -1.f) ||
            !Near(under.point.Z, 2.f))
            return Fail("ground clip origin below");
    }
    {
        // Straight down with no gravity: t = drop / speed.
        const auto drop = OW::SolveGroundLanding({0, 5, 0}, {0, -1, 0}, 10.f, 0.f, 0.f);
        if (!drop.solved || !Near(drop.seconds, 0.5f, 0.001f) || !Near(drop.point.Y, 0.f, 0.001f))
            return Fail("ground landing no gravity");
        // Straight up with no gravity never returns to the plane.
        if (OW::SolveGroundLanding({0, 5, 0}, {0, 1, 0}, 10.f, 0.f, 0.f).solved)
            return Fail("ground landing upward no gravity");
        // Launched straight up at g: ballistic flight from and back to the same height.
        const auto arc = OW::SolveGroundLanding({0, 0, 0}, {0, 1, 0}, 9.8f, 9.8f, 0.f);
        if (!arc.solved || !Near(arc.seconds, 2.0f, 0.01f) || !Near(arc.point.Y, 0.f, 0.01f))
            return Fail("ground landing arc");
        // Origin under the plane resolves to the origin itself.
        const auto below = OW::SolveGroundLanding({0, -1, 0}, {0, 1, 0}, 5.f, 9.8f, 0.f);
        if (!below.solved || !Near(below.seconds, 0.f) || !Near(below.point.Y, -1.f))
            return Fail("ground landing origin below");
        // Exceeding the time budget leaves the landing unsolved.
        if (OW::SolveGroundLanding({0, 100, 0}, {0, -1, 0}, 1.f, 0.f, 0.f, 5.f).solved)
            return Fail("ground landing budget");
    }
    {
        OW::RayDecision aged{OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Clear};
        aged.ageMs = 150;
        if (aged.Fresh(100) || aged.AllowsHybrid(100) || !aged.Fresh(200))
            return Fail("ray decision age freshness");
    }

    // RayQueryBroker: async worker-only Service ownership, multi-target/multi-bone cache,
    // realistic movement drift tolerance, staleness, scene invalidation, and UAF-free Detach.
    {
        class ThreadedBackend final : public OW::EngineTrace::IBackend {
        public:
            std::atomic<int> activeCalls{0};
            std::atomic<bool> concurrentViolation{false};
            std::atomic<bool> traceEntered{false};
            std::atomic<int> traceDelayMs{0};
            std::atomic<uint64_t> bindCount{0};
            std::atomic<size_t> maxBatchSeen{0};
            std::thread::id bindThreadId{};
            std::thread::id traceThreadId{};

            const char* Name() const override { return "threaded-fixture"; }
            bool BindScene(uint64_t epoch) override {
                if (activeCalls.fetch_add(1) != 0) concurrentViolation.store(true);
                bindThreadId = std::this_thread::get_id();
                bindCount.fetch_add(1);
                activeCalls.fetch_sub(1);
                return epoch != 0;
            }
            void ReleaseScene() override {}
            OW::EngineTrace::Status Trace(const OW::EngineTrace::Request& req,
                                          OW::EngineTrace::Result& out) override {
                if (activeCalls.fetch_add(1) != 0) concurrentViolation.store(true);
                traceThreadId = std::this_thread::get_id();
                traceEntered.store(true);
                const int delay = traceDelayMs.load();
                if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                // Entity B (end.x > 5) is occluded; Entity A is clear.
                out.fraction = req.end.x > 5.0f ? 0.4f : 1.0f;
                out.hitFlags = req.end.x > 5.0f ? 0x24u : 0u;
                out.hitbox = req.end.x > 5.0f ? 7u : 0xFFFFu;
                out.entityId = req.end.x > 5.0f ? 0x3Au : 0xFFFFFFFFu;
                activeCalls.fetch_sub(1);
                return OW::EngineTrace::Status::Ok;
            }
            void TraceBatch(std::span<const OW::EngineTrace::Request> reqs,
                            std::span<OW::EngineTrace::Result> outs) override {
                if (activeCalls.fetch_add(1) != 0) concurrentViolation.store(true);
                traceThreadId = std::this_thread::get_id();
                traceEntered.store(true);
                size_t prev = maxBatchSeen.load();
                while (reqs.size() > prev && !maxBatchSeen.compare_exchange_weak(prev, reqs.size())) {}
                const int delay = traceDelayMs.load();
                if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                const size_t n = (std::min)(reqs.size(), outs.size());
                for (size_t i = 0; i < n; ++i) {
                    outs[i] = OW::EngineTrace::Result{};
                    outs[i].status = OW::EngineTrace::Status::Ok;
                    outs[i].fraction = reqs[i].end.x > 5.0f ? 0.4f : 1.0f;
                    outs[i].hitFlags = reqs[i].end.x > 5.0f ? 0x24u : 0u;
                    outs[i].hitbox = reqs[i].end.x > 5.0f ? 7u : 0xFFFFu;
                    outs[i].entityId = reqs[i].end.x > 5.0f ? 0x3Au : 0xFFFFFFFFu;
                }
                activeCalls.fetch_sub(1);
            }
        } backend;

        OW::EngineTrace::Service brokerService(&backend);
        OW::EngineTrace::RayQueryBroker broker;
        const std::thread::id callerThread = std::this_thread::get_id();
        broker.Start();
        if (broker.HasService()) return Fail("broker reported service before Attach");
        broker.Attach(&brokerService);
        if (!broker.HasService()) return Fail("broker did not report service after Attach");

        // sceneEpoch == 0 must be rejected.
        {
            OW::EngineTrace::RayQueryBroker::Query zeroScene{};
            zeroScene.request.start = {0.f, 0.f, 0.f};
            zeroScene.request.end = {0.f, 0.f, 10.f};
            zeroScene.request.sceneEpoch = 0;
            zeroScene.frame = 1;
            zeroScene.connectionEpoch = 1;
            zeroScene.sceneEpoch = 0;
            zeroScene.entityKey = 100;
            zeroScene.actorGeneration = 1;
            zeroScene.boneId = 17;
            broker.Submit(zeroScene);
            if (broker.Latest(zeroScene.request, 1, 1, 0, 100, 1, 17, 100).has_value())
                return Fail("broker accepted sceneEpoch 0");
        }

        broker.SetScene(7);
        OW::EngineTrace::RayQueryBroker::Query qA{};
        qA.request.start = {0.f, 1.f, 0.f};
        qA.request.end = {1.f, 1.f, 10.f};
        qA.request.sceneEpoch = 7;
        qA.frame = 10;
        qA.connectionEpoch = 1;
        qA.sceneEpoch = 7;
        qA.entityKey = 0xAA;
        qA.actorGeneration = 2;
        qA.boneId = 17;

        OW::EngineTrace::RayQueryBroker::Query qB = qA;
        qB.request.end = {8.f, 1.f, 10.f};
        qB.entityKey = 0xBB;
        qB.boneId = 17;

        OW::EngineTrace::RayQueryBroker::Query qAChest = qA;
        qAChest.boneId = 5;

        broker.Submit(qA);
        broker.Submit(qB);
        broker.Submit(qAChest);

        auto waitResult = [&](const OW::EngineTrace::RayQueryBroker::Query& q,
                              const OW::EngineTrace::Request& lookupReq,
                              uint64_t frame, uint32_t maxAgeMs = 500) {
            for (int attempt = 0; attempt < 100; ++attempt) {
                if (auto got = broker.Latest(lookupReq, frame, q.connectionEpoch, q.sceneEpoch,
                                             q.entityKey, q.actorGeneration, q.boneId, maxAgeMs))
                    return got;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return std::optional<OW::RayDecision>{};
        };

        // Multi-target and multi-bone results must all coexist in cache with full hit metadata (§6.1).
        const auto gotA = waitResult(qA, qA.request, 10);
        const auto gotB = waitResult(qB, qB.request, 10);
        const auto gotAChest = waitResult(qAChest, qAChest.request, 10);
        if (!gotA || gotA->world != OW::RayDecisionStatus::Clear || !Near(gotA->fraction, 1.0f, 1e-4f) ||
            !gotB || gotB->world != OW::RayDecisionStatus::Occluded ||
            !Near(gotB->fraction, 0.4f, 1e-4f) || gotB->hitFlags != 0x24u ||
            gotB->hitbox != 7u || gotB->hitEntityId != 0x3Au ||
            !gotAChest || gotAChest->world != OW::RayDecisionStatus::Clear)
            return Fail("broker multi-target/multi-bone cache and metadata passthrough");

        // BindScene and Trace must both have run on the worker thread, never on caller thread.
        if (backend.bindThreadId == callerThread || backend.traceThreadId == callerThread ||
            backend.bindThreadId != backend.traceThreadId)
            return Fail("broker did not confine Service calls to worker thread");

        // Realistic movement drift (~15cm) within 0.35m tolerance must still hit cache,
        // while a blink/teleport (~2m) must return explicit Stale(SpatialDrift) (§6.2).
        OW::EngineTrace::Request drifted = qA.request;
        drifted.start.x += 0.12f;
        drifted.end.z += 0.15f;
        const auto gotDrifted = broker.Latest(drifted, 11, 1, 7, 0xAA, 2, 17, 500);
        if (!gotDrifted || gotDrifted->world != OW::RayDecisionStatus::Clear ||
            gotDrifted->staleReason != OW::RayStaleReason::None)
            return Fail("broker rejected small movement drift");
        OW::EngineTrace::Request teleported = qA.request;
        teleported.end.x += 2.0f;
        const auto gotTeleported = broker.Latest(teleported, 11, 1, 7, 0xAA, 2, 17, 500);
        if (!gotTeleported || gotTeleported->world != OW::RayDecisionStatus::Stale ||
            gotTeleported->staleReason != OW::RayStaleReason::SpatialDrift)
            return Fail("broker failed to mark spatial drift as explicit Stale(SpatialDrift)");

        // Expired age must mark world status Stale(AgeExpired).
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        const auto stale = broker.Latest(qA.request, 12, 1, 7, 0xAA, 2, 17, 5);
        if (!stale || stale->world != OW::RayDecisionStatus::Stale ||
            stale->staleReason != OW::RayStaleReason::AgeExpired)
            return Fail("broker failed to mark expired result stale(AgeExpired)");

        // Phase 2 (§6.1-§6.3): SubmitBatch of 6 distinct slots (3 ParabolicChords + 1 GroundClip + 2 EdgeFan)
        // must execute in a single 6-ray batch without slot collision and coexist in cache.
        {
            std::vector<OW::EngineTrace::RayQueryBroker::Query> sixBatch;
            for (uint8_t s = 0; s < 3; ++s) {
                auto q = qA;
                q.frame = 15;
                q.segmentKind = OW::RaySegmentKind::ParabolicChord;
                q.segmentIndex = s;
                q.request.end.x = (s == 1) ? 8.0f : 1.0f; // middle chord blocked, others clear
                sixBatch.push_back(q);
            }
            {
                auto q = qA;
                q.frame = 15;
                q.segmentKind = OW::RaySegmentKind::GroundClip;
                q.segmentIndex = 0;
                q.request.end.x = 7.0f; // ground hit at fraction 0.4
                sixBatch.push_back(q);
            }
            for (uint8_t f = 0; f < 2; ++f) {
                auto q = qA;
                q.frame = 15;
                q.segmentKind = OW::RaySegmentKind::EdgeFan;
                q.segmentIndex = f;
                q.request.end.x = (f == 0) ? 9.0f : 1.0f;
                sixBatch.push_back(q);
            }
            backend.maxBatchSeen.store(0);
            broker.SubmitBatch(sixBatch);
            bool allReady = false;
            for (int attempt = 0; attempt < 100; ++attempt) {
                const auto p0 = broker.Latest(sixBatch[0].request, 15, 1, 7, 0xAA, 2, 17, 500, 0.35f,
                                              OW::RaySegmentKind::ParabolicChord, 0);
                const auto p1 = broker.Latest(sixBatch[1].request, 15, 1, 7, 0xAA, 2, 17, 500, 0.35f,
                                              OW::RaySegmentKind::ParabolicChord, 1);
                const auto p2 = broker.Latest(sixBatch[2].request, 15, 1, 7, 0xAA, 2, 17, 500, 0.35f,
                                              OW::RaySegmentKind::ParabolicChord, 2);
                const auto gc = broker.Latest(sixBatch[3].request, 15, 1, 7, 0xAA, 2, 17, 500, 0.35f,
                                              OW::RaySegmentKind::GroundClip, 0);
                const auto ef0 = broker.Latest(sixBatch[4].request, 15, 1, 7, 0xAA, 2, 17, 500, 0.35f,
                                               OW::RaySegmentKind::EdgeFan, 0);
                const auto ef1 = broker.Latest(sixBatch[5].request, 15, 1, 7, 0xAA, 2, 17, 500, 0.35f,
                                               OW::RaySegmentKind::EdgeFan, 1);
                if (p0 && p1 && p2 && gc && ef0 && ef1) {
                    if (p0->world != OW::RayDecisionStatus::Clear ||
                        p1->world != OW::RayDecisionStatus::Occluded ||
                        p2->world != OW::RayDecisionStatus::Clear ||
                        gc->world != OW::RayDecisionStatus::Occluded ||
                        ef0->world != OW::RayDecisionStatus::Occluded ||
                        ef1->world != OW::RayDecisionStatus::Clear) {
                        return Fail("broker 6-slot segmentKind/segmentIndex coexistence mismatch");
                    }
                    allReady = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!allReady || backend.maxBatchSeen.load() != 6)
                return Fail("broker failed to dispatch 6-slot batch concurrently");
        }

        // Scene change must immediately invalidate cached entries.
        broker.SetScene(8);
        if (broker.Latest(qA.request, 12, 1, 7, 0xAA, 2, 17, 500).has_value())
            return Fail("broker retained cache across scene change");

        // Scene change during an in-flight broker batch must discard old-scene completions
        // so stale results never pollute the new scene cache (Finding #16).
        {
            backend.traceDelayMs.store(40);
            backend.traceEntered.store(false);
            auto qInFlight = qA;
            qInFlight.frame = 18;
            qInFlight.request.sceneEpoch = 8;
            qInFlight.sceneEpoch = 8;
            broker.Submit(qInFlight);
            for (int attempt = 0; attempt < 100 && !backend.traceEntered.load(); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!backend.traceEntered.load()) return Fail("in-flight batch did not start");
            broker.SetScene(9);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            if (broker.Latest(qInFlight.request, 19, 1, 8, 0xAA, 2, 17, 500).has_value() ||
                broker.Latest(qInFlight.request, 19, 1, 9, 0xAA, 2, 17, 500).has_value()) {
                return Fail("in-flight batch leaked across SetScene boundary");
            }
            backend.traceDelayMs.store(0);
            auto qNewScene = qA;
            qNewScene.frame = 19;
            qNewScene.request.sceneEpoch = 9;
            qNewScene.sceneEpoch = 9;
            broker.Submit(qNewScene);
            const auto gotNewScene = waitResult(qNewScene, qNewScene.request, 19);
            if (!gotNewScene || gotNewScene->world != OW::RayDecisionStatus::Clear)
                return Fail("broker failed to resume cleanly on new scene after in-flight SetScene");
        }

        // F4/F9 production seam test: EvaluateParabolicBrokerDecision end-to-end on real broker
        // for 1-segment Hanzo 30m low-arch (impactPoint vs gravity-lifted aimPoint would drift 0.365m
        // if lookup diverged from submit) AND 3-segment parabolic chord aggregation.
        {
            const OW::Vector3 hOrigin{0.0f, 1.5f, 0.0f};
            const OW::Vector3 hTarget{0.0f, 1.5f, 30.0f};
            const auto hanzo30Plan = OW::TrajectoryRay::BuildParabolicChordPlan(
                hOrigin, hTarget, {0.0f, 0.0f, 0.0f}, 110.0f, 9.8f, {0.0f, 1.9f, 30.0f});
            if (hanzo30Plan.segmentCount != 1)
                return Fail("Hanzo 30m expected 1-segment plan for seam test");
            // Submit through EvaluateParabolicBrokerDecision with divergent fallbackEnd (Y=1.9 vs impact Y=1.5).
            (void)OW::TrajectoryRay::EvaluateParabolicBrokerDecision(
                broker, &hanzo30Plan, hOrigin, {0.0f, 1.9f, 30.0f},
                21, 1, 9, 0xCC, 1, 17, 100, OW::EngineTrace::Request{}.mask, 500, true);
            OW::RayDecision got1Chord{};
            for (int attempt = 0; attempt < 200; ++attempt) {
                got1Chord = OW::TrajectoryRay::EvaluateParabolicBrokerDecision(
                    broker, &hanzo30Plan, hOrigin, {0.0f, 1.9f, 30.0f},
                    21, 1, 9, 0xCC, 1, 17, 100, OW::EngineTrace::Request{}.mask, 500, false);
                if (got1Chord.world == OW::RayDecisionStatus::Clear)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (got1Chord.world != OW::RayDecisionStatus::Clear)
                return Fail("EvaluateParabolicBrokerDecision 1-chord Hanzo 30m submit/lookup diverged");

            // 3-chord parabolic submit + aggregate through real broker.
            const auto junkPlan = OW::TrajectoryRay::BuildParabolicChordPlan(
                hOrigin, {0.0f, 1.5f, 25.0f}, {0.0f, 0.0f, 0.0f}, 25.0f, 9.8f, {0.0f, 4.0f, 25.0f});
            if (junkPlan.segmentCount != 3)
                return Fail("Junkrat 25m expected 3-segment plan for seam test");
            (void)OW::TrajectoryRay::EvaluateParabolicBrokerDecision(
                broker, &junkPlan, hOrigin, {0.0f, 4.0f, 25.0f},
                22, 1, 9, 0xCD, 1, 17, 101, OW::EngineTrace::Request{}.mask, 500, true);
            OW::RayDecision got3Chord{};
            for (int attempt = 0; attempt < 200; ++attempt) {
                got3Chord = OW::TrajectoryRay::EvaluateParabolicBrokerDecision(
                    broker, &junkPlan, hOrigin, {0.0f, 4.0f, 25.0f},
                    22, 1, 9, 0xCD, 1, 17, 101, OW::EngineTrace::Request{}.mask, 500, false);
                if (got3Chord.world == OW::RayDecisionStatus::Clear)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (got3Chord.world != OW::RayDecisionStatus::Clear ||
                got3Chord.segmentKind != OW::RaySegmentKind::ParabolicChord)
                return Fail("EvaluateParabolicBrokerDecision 3-chord round-trip failed");
        }

        // Detach must synchronously wait for an in-flight Trace so destroying Service is UAF-free.
        backend.traceDelayMs.store(60);
        backend.traceEntered.store(false);
        OW::EngineTrace::RayQueryBroker::Query qSlow = qA;
        qSlow.request.sceneEpoch = 9;
        qSlow.sceneEpoch = 9;
        qSlow.frame = 20;
        broker.Submit(qSlow);
        for (int attempt = 0; attempt < 100 && !backend.traceEntered.load(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!backend.traceEntered.load()) return Fail("slow trace did not start");
        broker.Detach();
        if (broker.HasService()) return Fail("broker still reported service after Detach");
        if (backend.activeCalls.load() != 0 || backend.concurrentViolation.load())
            return Fail("broker Detach returned while Service was still in flight");

        broker.Stop();
    }

    // Phase 2 (§6.4, Decision #8): Parabolic arch-height gate, 1/2/3 chord residual & aggregation.
    {
        using namespace OW::TrajectoryRay;
        const OW::Vector3 origin{0.0f, 1.5f, 0.0f};
        const OW::Vector3 zeroVel{0.0f, 0.0f, 0.0f};

        // Hanzo full draw (110 m/s, 9.82 m/s^2): 30m -> 1 seg (h_max~0.09m), 40m -> 2 segs (h_max~0.16m, res~0.04m).
        const auto hanzo30 = BuildParabolicChordPlan(origin, {0.0f, 1.5f, 30.0f}, zeroVel, 110.0f, 9.82f, {0.0f, 1.5f, 30.0f});
        const auto hanzo40 = BuildParabolicChordPlan(origin, {0.0f, 1.5f, 40.0f}, zeroVel, 110.0f, 9.82f, {0.0f, 1.5f, 40.0f});
        if (hanzo30.segmentCount != 1 || !Near(hanzo30.archHeightMeters, 0.09f, 0.02f) ||
            hanzo40.segmentCount != 2 || !Near(hanzo40.archHeightMeters, 0.16f, 0.02f) ||
            !Near(hanzo40.maxChordDeviationMeters, 0.04f, 0.01f)) {
            return Fail("parabolic chord table: Hanzo 30m/40m");
        }

        // Torbjorn primary (70 m/s, 9.81 m/s^2): 20m -> 1 seg, 30m -> 2 segs (h_max~0.23m).
        const auto torb20 = BuildParabolicChordPlan(origin, {0.0f, 1.5f, 20.0f}, zeroVel, 70.0f, 9.81f, {0.0f, 1.5f, 20.0f});
        const auto torb30 = BuildParabolicChordPlan(origin, {0.0f, 1.5f, 30.0f}, zeroVel, 70.0f, 9.81f, {0.0f, 1.5f, 30.0f});
        if (torb20.segmentCount != 1 || torb30.segmentCount != 2 || !Near(torb30.archHeightMeters, 0.23f, 0.02f)) {
            return Fail("parabolic chord table: Torbjorn 20m/30m");
        }

        // Junkrat primary (30 m/s, 12.35 m/s^2): 10m -> 2 segs (h_max~0.17m), 20m -> 3 segs (h_max~0.70m, res~0.08m).
        const auto junk10 = BuildParabolicChordPlan(origin, {0.0f, 1.5f, 10.0f}, zeroVel, 30.0f, 12.35f, {0.0f, 1.5f, 10.0f});
        const auto junk20 = BuildParabolicChordPlan(origin, {0.0f, 1.5f, 20.0f}, zeroVel, 30.0f, 12.35f, {0.0f, 1.5f, 20.0f});
        if (junk10.segmentCount != 2 || !Near(junk10.archHeightMeters, 0.17f, 0.02f) ||
            junk20.segmentCount != 3 || !Near(junk20.archHeightMeters, 0.70f, 0.03f) ||
            !Near(junk20.maxChordDeviationMeters, 0.08f, 0.02f)) {
            return Fail("parabolic chord table: Junkrat 10m/20m");
        }
        // Chords must be continuous end-to-end from origin to physical impact point.
        if ((junk20.segments[0].first - origin).Size() > 1e-4f ||
            (junk20.segments[0].second - junk20.segments[1].first).Size() > 1e-4f ||
            (junk20.segments[1].second - junk20.segments[2].first).Size() > 1e-4f ||
            (junk20.segments[2].second - OW::Vector3(0.0f, 1.5f, 20.0f)).Size() > 1e-3f) {
            return Fail("parabolic 3-segment chord continuity");
        }

        // Zarya alt (25 m/s, 9.84 m/s^2): 10m -> 2 segs, 20m -> 3 segs (h_max~0.81m).
        const auto zarya10 = BuildParabolicChordPlan(origin, {0.0f, 1.5f, 10.0f}, zeroVel, 25.0f, 9.84f, {0.0f, 1.5f, 10.0f});
        const auto zarya20 = BuildParabolicChordPlan(origin, {0.0f, 1.5f, 20.0f}, zeroVel, 25.0f, 9.84f, {0.0f, 1.5f, 20.0f});
        if (zarya10.segmentCount != 2 || zarya20.segmentCount != 3 || !Near(zarya20.archHeightMeters, 0.81f, 0.03f)) {
            return Fail("parabolic chord table: Zarya 10m/20m");
        }

        // Decision #8: 3/3 Clear -> Clear; any Occluded -> Occluded with metadata; partial Stale -> Stale.
        OW::RayDecision c0{OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Clear};
        c0.fraction = 1.0f; c0.segmentKind = OW::RaySegmentKind::ParabolicChord; c0.segmentIndex = 0;
        OW::RayDecision c1 = c0; c1.segmentIndex = 1;
        OW::RayDecision c2 = c0; c2.segmentIndex = 2;
        const auto aggAllClear = AggregateParabolicDecisions({c0, c1, c2}, 3);
        if (aggAllClear.world != OW::RayDecisionStatus::Clear)
            return Fail("parabolic 3/3 Clear aggregation");

        OW::RayDecision occ = c1;
        occ.world = OW::RayDecisionStatus::Occluded;
        occ.fraction = 0.35f;
        occ.hitFlags = 0x24u;
        occ.hitEntityId = 0x11u;
        const auto aggBlocked = AggregateParabolicDecisions({c0, occ, c2}, 3);
        if (aggBlocked.world != OW::RayDecisionStatus::Occluded ||
            !Near(aggBlocked.fraction, 0.35f, 1e-4f) || aggBlocked.hitEntityId != 0x11u) {
            return Fail("parabolic occluded chord aggregation");
        }

        OW::RayDecision staleSeg = c2;
        staleSeg.world = OW::RayDecisionStatus::Stale;
        staleSeg.staleReason = OW::RayStaleReason::SpatialDrift;
        const auto aggVacuum = AggregateParabolicDecisions({c0, c1, staleSeg}, 3);
        if (aggVacuum.world != OW::RayDecisionStatus::Stale ||
            aggVacuum.staleReason != OW::RayStaleReason::SpatialDrift) {
            return Fail("parabolic vacuum chord must not turn into Clear");
        }

        // Finding #1 regression: missing (std::nullopt) chord segments MUST NEVER produce Clear!
        const auto aggMissingTail = AggregateParabolicDecisions({c0, std::nullopt, std::nullopt}, 3);
        const auto aggMissingLast = AggregateParabolicDecisions({c0, c1, std::nullopt}, 3);
        const auto aggMissingWithOcc = AggregateParabolicDecisions({std::nullopt, occ, std::nullopt}, 3);
        if (aggMissingTail.world != OW::RayDecisionStatus::Unknown ||
            aggMissingTail.segmentIndex != 1 ||
            aggMissingLast.world != OW::RayDecisionStatus::Unknown ||
            aggMissingLast.segmentIndex != 2 ||
            aggMissingWithOcc.world != OW::RayDecisionStatus::Occluded) {
            return Fail("parabolic missing chord must stay Unknown, never Clear");
        }

        // Finding #4 regression: 1-segment ballistic chord (Hanzo 30m) ends at physical impactPoint,
        // so chordPlan.segments[0] is identical for both batch submit and MakeNativeRayDecision lookup.
        if ((hanzo30.segments[0].first - origin).Size() > 1e-4f ||
            (hanzo30.segments[0].second - hanzo30.impactPoint).Size() > 1e-4f) {
            return Fail("1-segment ballistic chord must end at physical impactPoint");
        }

        // Airborne GroundClip ray (§6.3 Slot 2) & Finding #9 motion-consistent clip.
        OW::Vector3 gcStart{}, gcEnd{};
        if (BuildAirborneGroundClipRay({0.0f, 1.0f, 10.0f}, {5.0f, 0.05f, 0.0f}, 0.2f, gcStart, gcEnd))
            return Fail("grounded actor must not emit airborne GroundClip ray");
        if (!BuildAirborneGroundClipRay({0.0f, 2.0f, 10.0f}, {0.0f, -6.0f, 0.0f}, 0.3f, gcStart, gcEnd))
            return Fail("falling actor must emit airborne GroundClip ray");
        OW::RayDecision groundHit{OW::RayDecisionStatus::Unknown, OW::RayDecisionStatus::Occluded};
        groundHit.fraction = 0.5f;
        const float expectedGroundY = gcStart.Y + (gcEnd.Y - gcStart.Y) * 0.5f;
        OW::Vector3 predHead{0.0f, -1.0f, 10.0f}; // Overshot below terrain
        const bool clipped = ApplyAirborneGroundClip({0.0f, 3.6f, 10.0f}, {0.0f, 2.0f, 10.0f},
                                                     gcStart, gcEnd, groundHit, predHead);
        if (!clipped || !Near(predHead.Y, expectedGroundY + 1.6f, 1e-3f))
            return Fail("airborne GroundClip failed to clamp bone height above terrain");
        OW::Vector3 motionPreFire{0.0f, expectedGroundY + 2.0f, 10.0f};
        OW::Vector3 motionVel{0.0f, -10.0f, 0.0f};
        if (!ApplyAirborneGroundClipMotion({0.0f, 3.6f, 10.0f}, {0.0f, 2.0f, 10.0f},
                                           gcStart, gcEnd, groundHit, 0.2f, motionPreFire, motionVel) ||
            motionPreFire.Y + motionVel.Y * 0.2f < expectedGroundY + 1.6f - 1e-3f) {
            return Fail("airborne GroundClipMotion failed to keep projected impact above terrain");
        }

        // F6 exact regression: retreating airborne target at 20m, speed=25m/s, retreat=5.5m/s
        // has actual solved flight T > 1.025s (> approx 0.8s). GroundClip must guarantee terrain
        // clearance (>= 1.6m) at the ACTUAL solved flight time and in BuildParabolicChordPlan.
        {
            OW::RayDecision floorHit{OW::RayDecisionStatus::Unknown, OW::RayDecisionStatus::Occluded};
            floorHit.fraction = 0.5f; // groundY = 0.0, rawBone=3.6, root=2.0 -> minBoneY = 1.6
            OW::Vector3 f6Origin{0.0f, 1.5f, 0.0f};
            OW::Vector3 f6PreFire{0.0f, 2.0f, 20.0f};
            OW::Vector3 f6Vel{0.0f, -2.0f, 5.5f}; // Retreating +Z at 5.5 m/s while falling
            OW::BallisticIntercept f6Solved{};
            if (!ApplyAirborneGroundClipAndSolveIntercept(
                    f6Origin, {0.0f, 3.6f, 20.0f}, {0.0f, 2.0f, 20.0f},
                    {0.0f, 2.0f, 20.0f}, {0.0f, -2.0f, 20.0f}, floorHit,
                    25.0f, 9.8f, f6PreFire, f6Vel, &f6Solved)) {
                return Fail("F6 GroundClipAndSolveIntercept failed to cap descending retreating target");
            }
            if (!f6Solved.solved || f6Solved.flightSeconds <= 1.025f) {
                return Fail("F6 solved flight time should exceed 1.025s for retreating target");
            }
            const float solvedImpactY = f6PreFire.Y + f6Vel.Y * f6Solved.flightSeconds;
            const auto f6Chords = BuildParabolicChordPlan(
                f6Origin, f6PreFire, f6Vel, 25.0f, 9.8f, f6Solved.aimPoint);
            if (solvedImpactY < 1.6f - 1e-3f || f6Chords.impactPoint.Y < 1.6f - 1e-3f) {
                return Fail("F6 solved impact or parabolic chord sank below minBoneY=1.6m");
            }

            // F6 reachable-to-unreachable regression (Codex Round 3 exact numerical case):
            // Launch (0,0,0), pre-fire (60,10,0), speed 25 m/s, gravity 9.84 m/s^2, initial v_y=-10 m/s,
            // minBoneY=9.6 m. Initial intercept succeeds (~2.478s), but after clipping v_y (~-0.161 m/s)
            // the shot becomes unreachable -> MUST return clipped=true and outIntercept.solved=false!
            const OW::Vector3 uOrigin{0.0f, 0.0f, 0.0f};
            OW::Vector3 uPreFire{60.0f, 10.0f, 0.0f};
            OW::Vector3 uVel{0.0f, -10.0f, 0.0f};
            const auto initialReachable = OW::SolveBallisticIntercept(uOrigin, uPreFire, uVel, 25.0f, 9.84f);
            if (!initialReachable.solved) {
                return Fail("F6 reachable-to-unreachable fixture precondition failed");
            }
            OW::RayDecision highFloorHit{OW::RayDecisionStatus::Unknown, OW::RayDecisionStatus::Occluded};
            highFloorHit.fraction = 0.5f; // groundRayStart.Y=10, groundRayEnd.Y=6 -> groundY=8.0; boneHeight=10.0-8.4=1.6 -> minBoneY=9.6
            OW::BallisticIntercept uSolved{};
            uSolved.solved = true; // Pre-seed to verify helper explicitly invalidates it!
            const bool uClipped = ApplyAirborneGroundClipAndSolveIntercept(
                uOrigin, {60.0f, 10.0f, 0.0f}, {60.0f, 8.4f, 0.0f},
                {60.0f, 10.0f, 0.0f}, {60.0f, 6.0f, 0.0f}, highFloorHit,
                25.0f, 9.84f, uPreFire, uVel, &uSolved);
            if (!uClipped || uSolved.solved) {
                return Fail("F6 reachable-to-unreachable clip must invalidate stale intercept (solved=false)");
            }
        }

        // 6-slot RayBatchPlanner quota, Barrier yield, EdgeFan borrow, AND F2 sustained saturation (§6.3).
        auto makeQ = [](uint64_t key, int bone, OW::RaySegmentKind kind, uint8_t idx) {
            OW::EngineTrace::RayQueryBroker::Query q{};
            q.sceneEpoch = 7;
            q.entityKey = key;
            q.actorGeneration = 1;
            q.boneId = bone;
            q.segmentKind = kind;
            q.segmentIndex = idx;
            return q;
        };
        RayBatchPlanInput in{};
        in.top1Primary.PushUrgent(makeQ(1, 17, OW::RaySegmentKind::ParabolicChord, 0));
        in.top1Primary.PushUrgent(makeQ(1, 17, OW::RaySegmentKind::ParabolicChord, 1));
        in.top1FallbackOrWarmup.PushUrgent(makeQ(1, 5, OW::RaySegmentKind::Direct, 0));
        in.top1GroundClip = makeQ(1, 17, OW::RaySegmentKind::GroundClip, 0);
        in.top1GroundClipUrgent = true;
        in.barrierPolicy = OW::BarrierPolicy::IgnoreBarrier;
        in.top1WorldClear = true;
        in.top1BarrierQuery = makeQ(1, 17, OW::RaySegmentKind::Barrier, 0);
        in.top1BarrierUrgent = true;
        in.top2Primary.PushUrgent(makeQ(2, 17, OW::RaySegmentKind::ParabolicChord, 0));
        in.top2Primary.PushUrgent(makeQ(2, 17, OW::RaySegmentKind::ParabolicChord, 1));
        const auto planIgnoreBarrier = PlanRayBatch(in);
        // IgnoreBarrier yields Slot 3 -> 2 primary + 1 groundClip + 1 fallback + 2 Top2 parabolic chords = 6!
        if (planIgnoreBarrier.count != 6 || planIgnoreBarrier.edgeFanBorrowed)
            return Fail("RayBatchPlanner 6-slot normal quota");
        int top2ChordsScheduled = 0;
        for (size_t k = 0; k < planIgnoreBarrier.count; ++k) {
            if (planIgnoreBarrier.queries[k].segmentKind == OW::RaySegmentKind::Barrier)
                return Fail("IgnoreBarrier policy must not schedule Barrier slot");
            if (planIgnoreBarrier.queries[k].entityKey == 2 &&
                planIgnoreBarrier.queries[k].segmentKind == OW::RaySegmentKind::ParabolicChord)
                ++top2ChordsScheduled;
        }
        if (top2ChordsScheduled != 2)
            return Fail("RayBatchPlanner must schedule complete multi-chord set for Top-2 when slots fit");
        in.barrierPolicy = OW::BarrierPolicy::WaitBarrierDrop;
        const auto planWaitBarrier = PlanRayBatch(in);
        bool hasBarrier = false;
        for (size_t k = 0; k < planWaitBarrier.count; ++k) {
            if (planWaitBarrier.queries[k].segmentKind == OW::RaySegmentKind::Barrier)
                hasBarrier = true;
        }
        if (planWaitBarrier.count != 6 || !hasBarrier)
            return Fail("WaitBarrierDrop policy must schedule Barrier slot when world Clear");

        // F2 persistent urgent-saturation & steady-state refresh regression (Codex Round 3 Finding 1):
        // Even when ALL 7 sources (3-chord Top-1 primary, GroundClip, 3-chord Top-1 fallback, Barrier,
        // 3-chord Top-2 primary, 3-chord Top-2 fallback, 3-chord Top-3 primary = 17 urgent queries)
        // stay PERMANENTLY urgent across 6 ticks, every single source and every chord index {0,1,2}
        // must make sustained progress (ZERO starvation!).
        {
            RayBatchPlanInput persistentUrgent{};
            for (uint8_t s = 0; s < 3; ++s) {
                persistentUrgent.top1Primary.PushUrgent(makeQ(1, 17, OW::RaySegmentKind::ParabolicChord, s));
                persistentUrgent.top1FallbackOrWarmup.PushUrgent(makeQ(1, 5, OW::RaySegmentKind::ParabolicChord, s));
                persistentUrgent.top2Primary.PushUrgent(makeQ(2, 17, OW::RaySegmentKind::ParabolicChord, s));
                persistentUrgent.top2Fallback.PushUrgent(makeQ(2, 5, OW::RaySegmentKind::ParabolicChord, s));
                persistentUrgent.top3Primary.PushUrgent(makeQ(3, 17, OW::RaySegmentKind::ParabolicChord, s));
            }
            persistentUrgent.top1GroundClip = makeQ(1, 17, OW::RaySegmentKind::GroundClip, 0);
            persistentUrgent.top1GroundClipUrgent = true;
            persistentUrgent.barrierPolicy = OW::BarrierPolicy::WaitBarrierDrop;
            persistentUrgent.top1WorldClear = true;
            persistentUrgent.top1BarrierQuery = makeQ(1, 17, OW::RaySegmentKind::Barrier, 0);
            persistentUrgent.top1BarrierUrgent = true;

            uint32_t uT1P = 0, uT1F = 0, uT2P = 0, uT2F = 0, uT3P = 0;
            int uGc = 0, uBarrier = 0;
            for (uint32_t tick = 0; tick < 6; ++tick) {
                persistentUrgent.rotationCursor = tick;
                const auto p = PlanRayBatch(persistentUrgent);
                if (p.count != 6)
                    return Fail("F2 persistent urgent saturation must utilize all 6 slots");
                for (size_t k = 0; k < p.count; ++k) {
                    const auto& q = p.queries[k];
                    if (q.segmentKind == OW::RaySegmentKind::GroundClip) ++uGc;
                    else if (q.segmentKind == OW::RaySegmentKind::Barrier) ++uBarrier;
                    else if (q.entityKey == 1 && q.boneId == 17) uT1P |= (1u << q.segmentIndex);
                    else if (q.entityKey == 1 && q.boneId == 5) uT1F |= (1u << q.segmentIndex);
                    else if (q.entityKey == 2 && q.boneId == 17) uT2P |= (1u << q.segmentIndex);
                    else if (q.entityKey == 2 && q.boneId == 5) uT2F |= (1u << q.segmentIndex);
                    else if (q.entityKey == 3 && q.boneId == 17) uT3P |= (1u << q.segmentIndex);
                }
            }
            if (uT1P != 0x7u || uT1F != 0x7u || uT2P != 0x7u || uT2F != 0x7u || uT3P != 0x7u ||
                uGc < 5 || uBarrier < 5) {
                return Fail("F2 persistent urgent saturation starved GroundClip, Barrier, Top-2, or Top-3");
            }

            // Steady-state refresh saturation (all 7 sources fresh): across 6 ticks, zero starvation.
            RayBatchPlanInput steady{};
            for (uint8_t s = 0; s < 3; ++s) {
                steady.top1Primary.PushRefresh(makeQ(1, 17, OW::RaySegmentKind::ParabolicChord, s));
                steady.top1FallbackOrWarmup.PushRefresh(makeQ(1, 5, OW::RaySegmentKind::ParabolicChord, s));
                steady.top2Primary.PushRefresh(makeQ(2, 17, OW::RaySegmentKind::ParabolicChord, s));
                steady.top2Fallback.PushRefresh(makeQ(2, 5, OW::RaySegmentKind::ParabolicChord, s));
                steady.top3Primary.PushRefresh(makeQ(3, 17, OW::RaySegmentKind::ParabolicChord, s));
            }
            steady.top1GroundClip = makeQ(1, 17, OW::RaySegmentKind::GroundClip, 0);
            steady.top1GroundClipUrgent = false;

            uint32_t maskT1P = 0, maskT1F = 0, maskT2P = 0, maskT2F = 0, maskT3P = 0;
            int gcCount = 0;
            for (uint32_t tick = 0; tick < 6; ++tick) {
                steady.rotationCursor = tick;
                const auto p = PlanRayBatch(steady);
                if (p.count != 6)
                    return Fail("F2 steady-state saturation must utilize all 6 slots");
                for (size_t k = 0; k < p.count; ++k) {
                    const auto& q = p.queries[k];
                    if (q.segmentKind == OW::RaySegmentKind::GroundClip) ++gcCount;
                    else if (q.entityKey == 1 && q.boneId == 17) maskT1P |= (1u << q.segmentIndex);
                    else if (q.entityKey == 1 && q.boneId == 5) maskT1F |= (1u << q.segmentIndex);
                    else if (q.entityKey == 2 && q.boneId == 17) maskT2P |= (1u << q.segmentIndex);
                    else if (q.entityKey == 2 && q.boneId == 5) maskT2F |= (1u << q.segmentIndex);
                    else if (q.entityKey == 3 && q.boneId == 17) maskT3P |= (1u << q.segmentIndex);
                }
            }
            if (maskT1P != 0x7u || maskT1F != 0x7u || maskT2P != 0x7u || maskT2F != 0x7u ||
                maskT3P != 0x7u || gcCount < 5) {
                return Fail("F2 steady-state saturation starved a chord group or GroundClip");
            }
        }

        // EdgeFan borrow claims all 6 slots exclusively.
        in.edgeFanBorrowActive = true;
        in.edgeFanCount = 6;
        for (uint8_t f = 0; f < 6; ++f) {
            in.edgeFanQueries[f] = makeQ(1, 17, OW::RaySegmentKind::EdgeFan, f);
        }
        const auto planEdgeFan = PlanRayBatch(in);
        if (planEdgeFan.count != 6 || !planEdgeFan.edgeFanBorrowed)
            return Fail("RayBatchPlanner EdgeFan 6-slot borrow");
        for (size_t k = 0; k < 6; ++k) {
            if (planEdgeFan.queries[k].segmentKind != OW::RaySegmentKind::EdgeFan ||
                planEdgeFan.queries[k].segmentIndex != k)
                return Fail("EdgeFan borrowed slot mismatch");
        }

        // Decision #7: Hybrid vacuum tracking grace (<=60ms with Vis==true) vs strict auto-fire block.
        OW::RayDecision vacuum{OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Stale};
        vacuum.staleReason = OW::RayStaleReason::SpatialDrift;
        const int hybrid = static_cast<int>(OW::RayDecisionPolicy::Hybrid);
        if (OW::RayDecisionAllowsAutoFire(vacuum, false, hybrid))
            return Fail("Hybrid auto-fire must reject Stale vacuum");
        if (!OW::RayDecisionAllowsTrackingGrace(vacuum, true, false, hybrid, 25, 60))
            return Fail("Hybrid tracking grace must allow <=60ms vacuum when Vis==true");
        if (OW::RayDecisionAllowsTrackingGrace(vacuum, true, false, hybrid, 65, 60))
            return Fail("Hybrid tracking grace must reject >60ms vacuum");
        if (OW::RayDecisionAllowsTrackingGrace(vacuum, false, false, hybrid, 0, 60))
            return Fail("Hybrid tracking grace must immediately exit when Vis==false");
    }

    // ui-preferences.json rayBackend migration: legacy v1 was 0=Native/1=Embree/2=UniCore,
    // current v2 is 0=Native-only/1=Hybrid. Legacy Embree must land on Native-only.
    {
        using OW_Backend = OW::V3Raycast::Backend;
        struct Case { int version; unsigned saved; bool accepted; int expected; };
        const Case cases[] = {
            {1, 0, true, static_cast<int>(OW_Backend::NativeHitbox)},  // legacy Native
            {1, 1, true, static_cast<int>(OW_Backend::NativeHitbox)},  // legacy Embree -> Native
            {1, 2, true, 1},                                          // legacy UniCore -> Hybrid
            {1, 3, false, -1},                                         // legacy out of range
            {2, 0, true, static_cast<int>(OW_Backend::NativeHitbox)},  // current Native
            {2, 1, true, 1},                                          // current Hybrid
            {2, 2, false, -1},                                         // current out of range
        };
        for (const auto& c : cases) {
            int out = -1;
            const bool ok = UI::Preferences::MigrateRayBackend(c.version, c.saved, out);
            if (ok != c.accepted || (ok && out != c.expected)) return Fail("rayBackend preference migration");
        }
        if (UI::Preferences::kCurrentVersion < 3) return Fail("preference version not bumped to v3");

        // v3 visibilityMode migration: 0=Lite, 1=Vision, 2=Ray; legacy v1/v2 rayBackend maps 0->Lite, 1->Ray.
        struct VisCase {
            int version;
            bool hasVis;
            unsigned savedVis;
            bool hasRay;
            unsigned savedRay;
            bool accepted;
            int expected;
        };
        const VisCase visCases[] = {
            {1, false, 0, true, 0, true, 0},  // v1 Native -> Lite
            {1, false, 0, true, 1, true, 0},  // v1 Embree -> Lite
            {1, false, 0, true, 2, true, 2},  // v1 UniCore -> Ray
            {1, false, 0, true, 3, false, -1},
            {2, false, 0, true, 0, true, 0},  // v2 Native-only -> Lite
            {2, false, 0, true, 1, true, 2},  // v2 Hybrid -> Ray
            {3, true, 0, true, 1, true, 0},   // v3 explicit Lite wins over stale rayBackend
            {3, true, 1, true, 0, true, 1},   // v3 explicit Vision
            {3, true, 2, true, 1, true, 2},   // v3 explicit Ray
            {3, true, 3, true, 0, false, -1}, // v3 out of range
            {3, false, 0, false, 0, true, 0}, // defaults to Lite
        };
        for (const auto& vc : visCases) {
            int outMode = -1;
            const bool ok = UI::Preferences::MigrateVisibilityMode(
                vc.version, vc.hasVis, vc.savedVis, vc.hasRay, vc.savedRay, outMode);
            if (ok != vc.accepted || (ok && outMode != vc.expected))
                return Fail("v3 visibilityMode preference migration");
        }

        // ResolveVisibilityPolicyFromMode strict mutual exclusion.
        constexpr uint64_t kMask = 0x0690000000000055ull;
        if (OW::ResolveVisibilityPolicyFromMode(0, OW::BarrierPolicy::WaitBarrierDrop, kMask) != OW::VisibilityPolicy::Lite ||
            OW::ResolveVisibilityPolicyFromMode(1, OW::BarrierPolicy::WaitBarrierDrop, kMask) != OW::VisibilityPolicy::Vision ||
            OW::ResolveVisibilityPolicyFromMode(2, OW::BarrierPolicy::IgnoreBarrier, kMask) != OW::VisibilityPolicy::RayWorld ||
            OW::ResolveVisibilityPolicyFromMode(2, OW::BarrierPolicy::WaitBarrierDrop, 0) != OW::VisibilityPolicy::RayWorld ||
            OW::ResolveVisibilityPolicyFromMode(2, OW::BarrierPolicy::WaitBarrierDrop, kMask) != OW::VisibilityPolicy::RayWorldAndBarrier) {
            return Fail("ResolveVisibilityPolicyFromMode mutual exclusion mismatch");
        }
    }

    // Phase 3: Three-region profile contract (CN 155284N / World 155284 / Nexon 155284KB),
    // Nexon copy-inheritance guard, hitNormal broker propagation, and Slot 3 barrier layer.
    {
        const auto& cn = OW::offset::kCnRuntimeProfile;
        const auto& world = OW::offset::kWorld155284RuntimeProfile;
        const auto& nexon = OW::offset::kNexon155284RuntimeProfile;
        if (cn.rayEntryRva != 0x10222E0 || world.rayEntryRva != 0xD0E030 || nexon.rayEntryRva != 0x100C5E0)
            return Fail("three-region rayEntryRva mismatch");
        if (cn.rayContextVtableRva == nexon.rayContextVtableRva ||
            cn.raySceneSub60VtableRva == nexon.raySceneSub60VtableRva ||
            cn.clientGameRva == nexon.clientGameRva)
            return Fail("Nexon profile must not inherit CN ray/ClientGame RVAs");
        if (nexon.rayAuxCallbackRva != 0 || world.rayAuxCallbackRva != 0)
            return Fail("World/Nexon must keep unverified rayAuxCallbackRva=0");
        if (cn.rayWorldMask != 0x0690000000000002ull ||
            world.rayWorldMask != 0x0690000000000002ull ||
            nexon.rayWorldMask != 0x0690000000000002ull)
            return Fail("three-region rayWorldMask mismatch");
        if (cn.rayBarrierMask != 0 || world.rayBarrierMask != 0 || nexon.rayBarrierMask != 0)
            return Fail("unverified rayBarrierMask must stay 0 (no fabricated legacy 149004 masks)");

        // RayPolicyNeedsBarrierNotice truth table.
        const int native = static_cast<int>(OW::RayDecisionPolicy::NativeOnly);
        const int hybrid = static_cast<int>(OW::RayDecisionPolicy::Hybrid);
        if (OW::RayPolicyNeedsBarrierNotice(native, OW::BarrierPolicy::IgnoreBarrier, 0) ||
            OW::RayPolicyNeedsBarrierNotice(hybrid, OW::BarrierPolicy::IgnoreBarrier, 0) ||
            OW::RayPolicyNeedsBarrierNotice(hybrid, OW::BarrierPolicy::WaitBarrierDrop, 0x0690000000000055ull) ||
            !OW::RayPolicyNeedsBarrierNotice(native, OW::BarrierPolicy::WaitBarrierDrop, 0x0690000000000055ull) ||
            !OW::RayPolicyNeedsBarrierNotice(hybrid, OW::BarrierPolicy::WaitBarrierDrop, 0)) {
            return Fail("RayPolicyNeedsBarrierNotice truth table");
        }

        // RayDecisionAllowsAutoFire barrier enforcement truth table.
        OW::RayDecision d{OW::RayDecisionStatus::Hit, OW::RayDecisionStatus::Clear};
        d.barrierCheckRequired = true;
        d.barrier = OW::RayDecisionStatus::Unknown;
        if (OW::RayDecisionAllowsAutoFire(d, false, hybrid))
            return Fail("WaitBarrierDrop auto-fire must block barrier Unknown vacuum");
        d.barrier = OW::RayDecisionStatus::Stale;
        if (OW::RayDecisionAllowsAutoFire(d, false, hybrid))
            return Fail("WaitBarrierDrop auto-fire must block barrier Stale vacuum");
        d.barrier = OW::RayDecisionStatus::Occluded;
        if (OW::RayDecisionAllowsAutoFire(d, false, hybrid) ||
            OW::RayDecisionAllowsSelection(d, false, hybrid))
            return Fail("WaitBarrierDrop must block selection and auto-fire on barrier Occluded");
        d.barrier = OW::RayDecisionStatus::Clear;
        d.barrierAgeMs = 150;
        if (OW::RayDecisionAllowsAutoFire(d, false, hybrid, 100))
            return Fail("WaitBarrierDrop auto-fire must reject expired barrier verdict");
        d.barrierAgeMs = 15;
        if (!OW::RayDecisionAllowsAutoFire(d, false, hybrid, 100))
            return Fail("WaitBarrierDrop auto-fire must allow fresh barrier Clear");

        // Multi-mask broker isolation + hitPosition/hitNormal propagation + EvaluateBarrierLayerDecision.
        constexpr uint64_t kTestBarrierMask = 0x0690000000000055ull;
        class MaskAwareBackend final : public OW::EngineTrace::IBackend {
        public:
            std::atomic<bool> barrierUp{true};
            const char* Name() const override { return "mask-aware-test"; }
            bool BindScene(uint64_t) override { return true; }
            OW::EngineTrace::Status Trace(const OW::EngineTrace::Request& req,
                                          OW::EngineTrace::Result& res) override {
                res.status = OW::EngineTrace::Status::Ok;
                if (req.mask == kTestBarrierMask && barrierUp.load(std::memory_order_acquire)) {
                    res.fraction = 0.40f;
                    res.hitPosition = {4.0f, 1.5f, 0.0f};
                    res.hasHitPosition = true;
                    res.hitNormal = {-3.0f, 0.0f, 4.0f};  // unnormalized; broker normalizes to (-0.6, 0, 0.8)
                    res.hasHitNormal = true;
                    res.hitFlags = 0x24;
                    res.entityId = 77;
                } else {
                    res.fraction = 1.0f;
                }
                return res.status;
            }
            void ReleaseScene() override {}
        };

        MaskAwareBackend backend;
        OW::EngineTrace::Service svc(&backend);
        OW::EngineTrace::RayQueryBroker broker;
        broker.Attach(&svc);
        broker.Start();
        broker.SetScene(11);

        const OW::Vector3 cam(0.0f, 1.5f, 0.0f);
        const OW::Vector3 target(10.0f, 1.5f, 0.0f);

        // 1) IgnoreBarrier -> Slot 3 skipped even if barrier is up.
        const auto ignoredEval = OW::TrajectoryRay::EvaluateBarrierLayerDecision(
            broker, OW::VisibilityPolicy::RayWorld, OW::BarrierPolicy::IgnoreBarrier,
            kTestBarrierMask, true, cam, target,
            100, 1, 11, 0x9001, 2, 17, 1, 100, true);
        if (ignoredEval.barrierActive || ignoredEval.barrierBlocked ||
            ignoredEval.hitKind != OW::TraceHitKind::None || ignoredEval.slot3Query.has_value()) {
            broker.Stop();
            return Fail("IgnoreBarrier must skip Slot 3 barrier query");
        }

        // 2) WaitBarrierDrop with barrierMask==0 -> unsupported, Slot 3 skipped honestly.
        const auto unsupportedEval = OW::TrajectoryRay::EvaluateBarrierLayerDecision(
            broker, OW::VisibilityPolicy::RayWorld, OW::BarrierPolicy::WaitBarrierDrop,
            0, true, cam, target,
            100, 1, 11, 0x9001, 2, 17, 1, 100, true);
        if (unsupportedEval.barrierActive || unsupportedEval.slot3Query.has_value()) {
            broker.Stop();
            return Fail("WaitBarrierDrop with barrierMask==0 must not emit dummy Slot 3 query");
        }

        // 3) WaitBarrierDrop with RayWorldAndBarrier + barrierMask!=0: cold vacuum -> submit -> Occluded with hitNormal!
        const auto coldEval = OW::TrajectoryRay::EvaluateBarrierLayerDecision(
            broker, OW::VisibilityPolicy::RayWorldAndBarrier, OW::BarrierPolicy::WaitBarrierDrop,
            kTestBarrierMask, true, cam, target,
            100, 1, 11, 0x9001, 2, 17, 1, 100, true);
        if (!coldEval.barrierActive || !coldEval.barrierVacuum || !coldEval.slot3Urgent ||
            !coldEval.slot3Query.has_value() ||
            coldEval.slot3Query->request.mask != kTestBarrierMask ||
            coldEval.slot3Query->segmentKind != OW::RaySegmentKind::Barrier) {
            broker.Stop();
            return Fail("WaitBarrierDrop cold lookup must report barrierVacuum and schedule urgent Slot 3");
        }

        // Also submit world query for the exact same entity/bone to verify cache isolation!
        OW::EngineTrace::RayQueryBroker::Query worldQ = *coldEval.slot3Query;
        worldQ.request.mask = OW::EngineTrace::kDefaultWorldMask;
        worldQ.segmentKind = OW::RaySegmentKind::Direct;
        broker.Submit(worldQ);

        OW::TrajectoryRay::BarrierEvaluationResult blockedEval{};
        for (int i = 0; i < 100; ++i) {
            blockedEval = OW::TrajectoryRay::EvaluateBarrierLayerDecision(
                broker, OW::VisibilityPolicy::RayWorldAndBarrier, OW::BarrierPolicy::WaitBarrierDrop,
                kTestBarrierMask, true, cam, target,
                100, 1, 11, 0x9001, 2, 17, 1, 100, false);
            if (blockedEval.barrierBlocked) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto worldCached = broker.Latest(
            worldQ.request, 100, 1, 11, 0x9001, 2, 17, 100,
            OW::EngineTrace::RayQueryBroker::kDefaultMaxEndpointDriftMeters,
            OW::RaySegmentKind::Direct, 0);
        if (!blockedEval.barrierBlocked ||
            blockedEval.hitKind != OW::TraceHitKind::Barrier ||
            !blockedEval.barrierDecision.hasHitPosition ||
            !Near(blockedEval.barrierDecision.hitPosX, 4.0f, 1e-3f) ||
            !blockedEval.barrierDecision.hasHitNormal ||
            !Near(blockedEval.barrierDecision.hitNormalX, -0.6f, 1e-3f) ||
            !Near(blockedEval.barrierDecision.hitNormalZ, 0.8f, 1e-3f)) {
            broker.Stop();
            return Fail("WaitBarrierDrop must classify TraceHitKind::Barrier and propagate hitPosition/hitNormal");
        }
        if (!worldCached.has_value() || !worldCached->WorldClear()) {
            broker.Stop();
            return Fail("Barrier cache entry polluted Direct world cache slot");
        }

        // 4) Barrier drops -> next Slot 3 refresh transitions to barrierClear / TraceHitKind::None!
        backend.barrierUp.store(false, std::memory_order_release);
        broker.Submit(*blockedEval.slot3Query);
        OW::TrajectoryRay::BarrierEvaluationResult droppedEval{};
        for (int i = 0; i < 100; ++i) {
            droppedEval = OW::TrajectoryRay::EvaluateBarrierLayerDecision(
                broker, OW::VisibilityPolicy::RayWorldAndBarrier, OW::BarrierPolicy::WaitBarrierDrop,
                kTestBarrierMask, true, cam, target,
                101, 1, 11, 0x9001, 2, 17, 1, 100, false);
            if (droppedEval.barrierClear) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!droppedEval.barrierClear || droppedEval.barrierBlocked ||
            droppedEval.hitKind != OW::TraceHitKind::None) {
            broker.Stop();
            return Fail("Barrier drop must transition WaitBarrierDrop back to Clear / TraceHitKind::None");
        }

        // 5) Mixed Head vs Chest barrier outcome on the same actor (head bone 17 behind partial
        //    barrier vs chest bone 4 clear): cache slots are keyed by boneId so fallback bone
        //    can resolve Clear independently while head stays TraceHitKind::Barrier!
        backend.barrierUp.store(true, std::memory_order_release);
        const OW::Vector3 headPt(10.0f, 1.8f, 0.0f);
        const OW::Vector3 chestPt(10.0f, 1.2f, 0.0f);
        // Submit barrier query for head (bone 17) while barrierUp==true -> Occluded.
        (void)OW::TrajectoryRay::EvaluateBarrierLayerDecision(
            broker, OW::VisibilityPolicy::RayWorldAndBarrier, OW::BarrierPolicy::WaitBarrierDrop,
            kTestBarrierMask, true, cam, headPt,
            102, 1, 11, 0x9002, 3, 17, 1, 100, true);
        OW::TrajectoryRay::BarrierEvaluationResult headBar{};
        for (int i = 0; i < 100; ++i) {
            headBar = OW::TrajectoryRay::EvaluateBarrierLayerDecision(
                broker, OW::VisibilityPolicy::RayWorldAndBarrier, OW::BarrierPolicy::WaitBarrierDrop,
                kTestBarrierMask, true, cam, headPt,
                102, 1, 11, 0x9002, 3, 17, 1, 100, false);
            if (headBar.barrierBlocked) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Now chest (bone 4) sees clear barrier (simulating head above/behind partial barrier).
        backend.barrierUp.store(false, std::memory_order_release);
        (void)OW::TrajectoryRay::EvaluateBarrierLayerDecision(
            broker, OW::VisibilityPolicy::RayWorldAndBarrier, OW::BarrierPolicy::WaitBarrierDrop,
            kTestBarrierMask, true, cam, chestPt,
            102, 1, 11, 0x9002, 3, 4, 1, 100, true);
        OW::TrajectoryRay::BarrierEvaluationResult chestBar{};
        for (int i = 0; i < 100; ++i) {
            chestBar = OW::TrajectoryRay::EvaluateBarrierLayerDecision(
                broker, OW::VisibilityPolicy::RayWorldAndBarrier, OW::BarrierPolicy::WaitBarrierDrop,
                kTestBarrierMask, true, cam, chestPt,
                102, 1, 11, 0x9002, 3, 4, 1, 100, false);
            if (chestBar.barrierClear) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Re-read head (bone 17) to confirm chest Clear did not clobber head Barrier!
        headBar = OW::TrajectoryRay::EvaluateBarrierLayerDecision(
            broker, OW::VisibilityPolicy::RayWorldAndBarrier, OW::BarrierPolicy::WaitBarrierDrop,
            kTestBarrierMask, true, cam, headPt,
            102, 1, 11, 0x9002, 3, 17, 1, 100, false);
        if (!headBar.barrierBlocked || headBar.hitKind != OW::TraceHitKind::Barrier ||
            !chestBar.barrierClear || chestBar.hitKind != OW::TraceHitKind::None) {
            broker.Stop();
            return Fail("Mixed head/chest barrier cache slots must resolve independently per boneId");
        }

        // 6) Off-guidance barrier warmup via PlanRayBatch Slot 1 alongside guidance Slot 3:
        //    proves an uncached off-guidance bone (head=17) gets warmed via Slot 1 Barrier query
        //    without colliding with guidance bone (chest=4) Slot 3 Barrier query, and transitions
        //    to fresh Clear once the shield drops!
        OW::TrajectoryRay::RayBatchPlanInput warmIn{};
        warmIn.barrierPolicy = OW::BarrierPolicy::WaitBarrierDrop;
        warmIn.top1WorldClear = true;
        OW::EngineTrace::RayQueryBroker::Query chestWorldQ{};
        chestWorldQ.request.start = {cam.X, cam.Y, cam.Z};
        chestWorldQ.request.end = {chestPt.X, chestPt.Y, chestPt.Z};
        chestWorldQ.request.mask = OW::EngineTrace::kDefaultWorldMask;
        chestWorldQ.request.sceneEpoch = 11;
        chestWorldQ.frame = 103;
        chestWorldQ.connectionEpoch = 1;
        chestWorldQ.sceneEpoch = 11;
        chestWorldQ.entityKey = 0x9003;
        chestWorldQ.actorGeneration = 4;
        chestWorldQ.boneId = 4;
        chestWorldQ.segmentKind = OW::RaySegmentKind::Direct;
        warmIn.top1Primary.PushRefresh(chestWorldQ);

        OW::EngineTrace::RayQueryBroker::Query chestBarQ = chestWorldQ;
        chestBarQ.request.mask = kTestBarrierMask;
        chestBarQ.segmentKind = OW::RaySegmentKind::Barrier;
        warmIn.top1BarrierQuery = chestBarQ;
        warmIn.top1BarrierUrgent = false;

        OW::EngineTrace::RayQueryBroker::Query headWarmBarQ = chestBarQ;
        headWarmBarQ.request.end = {headPt.X, headPt.Y, headPt.Z};
        headWarmBarQ.boneId = 17;
        warmIn.top1FallbackOrWarmup.PushUrgent(headWarmBarQ);

        const auto warmPlan = OW::TrajectoryRay::PlanRayBatch(warmIn);
        if (warmPlan.count != 3) {
            broker.Stop();
            return Fail("PlanRayBatch must preserve both guidance Slot 3 and off-guidance Slot 1 barrier warmup");
        }
        broker.SubmitBatch(warmPlan.Span());
        OW::TrajectoryRay::BarrierEvaluationResult headWarmed{};
        for (int i = 0; i < 100; ++i) {
            headWarmed = OW::TrajectoryRay::EvaluateBarrierLayerDecision(
                broker, OW::VisibilityPolicy::RayWorldAndBarrier, OW::BarrierPolicy::WaitBarrierDrop,
                kTestBarrierMask, true, cam, headPt,
                103, 1, 11, 0x9003, 4, 17, 1, 100, false);
            if (headWarmed.barrierClear) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!headWarmed.barrierClear || headWarmed.hitKind != OW::TraceHitKind::None) {
            broker.Stop();
            return Fail("Off-guidance Slot 1 barrier warmup failed to populate fresh Clear cache entry");
        }

        // 7) Fast-target 60-100 ms age-boundary regression: after 75 ms, a fast target (maxAgeMs=60)
        //    must classify the cached barrier entry as Stale (AgeExpired) / barrierVacuum=true and
        //    mark slot3Urgent=true for immediate resubmission, while a slow target (maxAgeMs=100)
        //    still treats the same 75 ms entry as fresh Clear.
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
        const auto fastAgeEval = OW::TrajectoryRay::EvaluateBarrierLayerDecision(
            broker, OW::VisibilityPolicy::RayWorldAndBarrier, OW::BarrierPolicy::WaitBarrierDrop,
            kTestBarrierMask, true, cam, headPt,
            104, 1, 11, 0x9003, 4, 17, 1, /*fastMaxAgeMs=*/60, false);
        const auto slowAgeEval = OW::TrajectoryRay::EvaluateBarrierLayerDecision(
            broker, OW::VisibilityPolicy::RayWorldAndBarrier, OW::BarrierPolicy::WaitBarrierDrop,
            kTestBarrierMask, true, cam, headPt,
            104, 1, 11, 0x9003, 4, 17, 1, /*slowMaxAgeMs=*/100, false);
        const auto fastWarmupLookup = broker.Latest(
            headWarmBarQ.request, 104, 1, 11, 0x9003, 4, 17, /*fastMaxAgeMs=*/60,
            OW::EngineTrace::RayQueryBroker::kDefaultMaxEndpointDriftMeters,
            OW::RaySegmentKind::Barrier, 0);
        broker.Stop();
        if (!fastAgeEval.barrierVacuum || !fastAgeEval.slot3Urgent ||
            fastAgeEval.barrierDecision.world != OW::RayDecisionStatus::Stale ||
            fastAgeEval.barrierDecision.staleReason != OW::RayStaleReason::AgeExpired ||
            !fastWarmupLookup.has_value() || !fastWarmupLookup->IsObservationVacuum() ||
            !slowAgeEval.barrierClear || slowAgeEval.slot3Urgent) {
            return Fail("Fast-target 60-100ms age boundary mismatch between acceptance and warmup");
        }
    }

    if (argc > 1) WriteSvg(argv[1], worldShapes);
    std::cout << "PASS shapeMapped=bound/unbound critical=unknown/critical/ordinary local+UniCore selector\n";
    return EXIT_SUCCESS;
}
