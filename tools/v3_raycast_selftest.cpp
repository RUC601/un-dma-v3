#include "Game/V3Raycast.hpp"
#include "config/RayBackendMigration.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
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
    // ui-preferences.json rayBackend migration: legacy v1 was 0=Native/1=Embree/2=UniCore,
    // current v2 is 0=Native/1=UniCore. Legacy Embree must land on Native, not UniCore.
    {
        using OW_Backend = OW::V3Raycast::Backend;
        struct Case { int version; unsigned saved; bool accepted; int expected; };
        const Case cases[] = {
            {1, 0, true, static_cast<int>(OW_Backend::NativeHitbox)},  // legacy Native
            {1, 1, true, static_cast<int>(OW_Backend::NativeHitbox)},  // legacy Embree -> Native
            {1, 2, true, static_cast<int>(OW_Backend::UniCore)},       // legacy UniCore
            {1, 3, false, -1},                                         // legacy out of range
            {2, 0, true, static_cast<int>(OW_Backend::NativeHitbox)},  // current Native
            {2, 1, true, static_cast<int>(OW_Backend::UniCore)},       // current UniCore
            {2, 2, false, -1},                                         // current out of range
        };
        for (const auto& c : cases) {
            int out = -1;
            const bool ok = UI::Preferences::MigrateRayBackend(c.version, c.saved, out);
            if (ok != c.accepted || (ok && out != c.expected)) return Fail("rayBackend preference migration");
        }
        if (UI::Preferences::kCurrentVersion <= UI::Preferences::kLegacyVersion) return Fail("preference version not bumped");
    }

    if (argc > 1) WriteSvg(argv[1], worldShapes);
    std::cout << "PASS shapeMapped=bound/unbound critical=unknown/critical/ordinary local+UniCore selector\n";
    return EXIT_SUCCESS;
}
