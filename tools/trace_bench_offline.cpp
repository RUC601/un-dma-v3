#include "Game/EngineTrace/UnicornBackend.hpp"
#include "Game/EngineTrace/VisionProbe.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <span>

namespace {

using namespace OW::EngineTrace;

constexpr uint64_t kImage = 0x100000;
constexpr uint64_t kContext = 0x180000;
constexpr uint64_t kData = 0x200000;
constexpr uint64_t kScene = 7;
constexpr uint64_t kDefaultTimeoutUs = 6'000'000;
constexpr size_t kPageSize = 4096;

// The guest entry receives the result pointer in RDX.
constexpr unsigned char kReadFraction[] = {
    0xA1, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, // mov eax, [kData]
    0x89, 0x42, 0x30,                                     // mov [rdx+0x30], eax
    0xC3
};
constexpr unsigned char kWriteInitialFraction[] = {
    0xC7, 0x42, 0x30, 0x00, 0x00, 0x00, 0x3F, // mov dword [rdx+0x30], 0.5f bits
    0xC3
};
constexpr unsigned char kConditionallyWriteFraction[] = {
    0x83, 0x39, 0x00, 0x75, 0x07,             // Skip the write when start.x is nonzero.
    0xC7, 0x42, 0x30, 0x00, 0x00, 0x00, 0x3F,
    0xC3
};
constexpr unsigned char kWriteOnlyFlags[] = {0xC7, 0x42, 0x28, 0, 0, 0, 0, 0xC3};
constexpr unsigned char kPartialFraction[] = {0xC6, 0x42, 0x30, 0, 0xC3};

struct Fixture {
    std::span<const unsigned char> code;
    bool dataAvailable = true;
    uint32_t fractionBits = std::bit_cast<uint32_t>(0.25f);
    int dataReads = 0;

    UnicornBackend::Reader Reader() {
        return [this](uint64_t address, void* out, size_t size) {
            if (address == kContext && size == sizeof(kImage)) {
                std::memcpy(out, &kImage, size);
                return true;
            }
            if (size != kPageSize) return false;
            std::memset(out, 0, size);
            if (address == kImage) {
                std::memcpy(out, code.data(), code.size());
                return true;
            }
            if (address != kData) return false;
            ++dataReads;
            if (!dataAvailable) return false;
            std::memcpy(out, &fractionBits, sizeof(fractionBits));
            return true;
        };
    }
};

UnicornConfig OfflineConfig(uint64_t timeoutUs = 0) {
    UnicornConfig config;
    config.entryRva = 0;
    config.context = kContext;
    config.timeoutUs = timeoutUs;
    return config;
}

bool Bind(UnicornBackend& backend, Fixture& fixture) {
    backend.SetReader(fixture.Reader());
    backend.SetTarget(kImage, kPageSize);
    return backend.BindScene(kScene);
}

Request TestRequest() {
    Request request;
    request.sceneEpoch = kScene;
    return request;
}

}  // namespace

int RunTraceBackendSelfTest() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", label);
        if (!passed) ++failures;
    };
    const Request request = TestRequest();

    Fixture fixture{kReadFraction};
    fixture.dataAvailable = false;
    UnicornConfig config = OfflineConfig();
    config.refreshTouchedPages = true;
    UnicornBackend backend(config);
    if (!Bind(backend, fixture)) return 2;
    Result result;
    Request wrongScene = request;
    ++wrongScene.sceneEpoch;
    const bool rebound = backend.BindScene(wrongScene.sceneEpoch);
    const auto sceneStatus = backend.Trace(wrongScene, result);
    check(!rebound && sceneStatus == Status::InvalidScene && result.status == sceneStatus &&
        backend.GetStats().traces == 0, "wrong scene is rejected before execution");

    const auto missingStatus = backend.Trace(request, result);
    check(missingStatus == Status::Error && result.status == missingStatus &&
        backend.GetStats().lastFailedPage == kData, "missing page fails the trace");
    fixture.dataAvailable = true;
    check(backend.Trace(request, result) == Status::Ok && result.fraction == 0.25f &&
        fixture.dataReads == 2, "private cache retries the missing page");
    fixture.dataAvailable = false;
    check(backend.Trace(request, result) == Status::Error && result.status == Status::Error,
        "refresh failure cannot return the old result");

    VisionProbe::Config visionConfig;
    visionConfig.context = kContext;
    visionConfig.entryRva = 0;
    visionConfig.engines = 2;
    visionConfig.timeoutUs = 0;
    Fixture shared{kReadFraction};
    shared.dataAvailable = false;
    VisionProbe probe(visionConfig, shared.Reader(), kImage, kPageSize);
    if (!probe.Start(kScene)) return 2;
    const auto missing = probe.Probe({request, request});
    check(missing[0].status == Status::Error && missing[1].status == Status::Error,
        "both workers reject a missing shared page");
    shared.dataAvailable = true;
    shared.fractionBits = std::bit_cast<uint32_t>(1.0f);
    const auto available = probe.Probe({request, request});
    check(available[0].status == Status::Ok && available[1].status == Status::Ok &&
        available[0].fraction == 1.0f && available[1].fraction == 1.0f &&
        shared.dataReads == 3, "shared cache retries then shares the successful read");

    for (const auto code : {std::span<const unsigned char>(kWriteOnlyFlags),
            std::span<const unsigned char>(kPartialFraction)}) {
        Fixture incomplete{code};
        UnicornBackend incompleteBackend(OfflineConfig());
        if (!Bind(incompleteBackend, incomplete)) return 2;
        const auto status = incompleteBackend.Trace(request, result);
        check(status == Status::Error && result.status == status,
            code.data() == kWriteOnlyFlags ? "flags alone are not a fraction" :
            "partial fraction write is rejected");
    }
    Fixture sameValue{kConditionallyWriteFraction};
    UnicornBackend sameValueBackend(OfflineConfig());
    if (!Bind(sameValueBackend, sameValue)) return 2;
    check(sameValueBackend.Trace(request, result) == Status::Ok && result.fraction == 0.5f,
        "writing the initial fraction value is a valid result");
    Request skipWrite = request;
    skipWrite.start.x = 1.0f;
    check(sameValueBackend.Trace(skipWrite, result) == Status::Error &&
        result.status == Status::Error, "previous trace cannot satisfy the write check");
    return failures == 0 ? 0 : 1;
}

int RunTraceTimerBench() {
    const Request request = TestRequest();
    for (uint64_t timeout : {uint64_t{0}, kDefaultTimeoutUs, kDefaultTimeoutUs, uint64_t{0}}) {
        Fixture fixture{kWriteInitialFraction};
        UnicornBackend backend(OfflineConfig(timeout));
        if (!Bind(backend, fixture)) return 2;
        Result result;
        if (backend.Trace(request, result) != Status::Ok) return 1;
        const uint64_t warmFetches = backend.GetStats().pageFetches;
        std::array<uint64_t, 40> times{};
        for (auto& time : times) {
            if (backend.Trace(request, result) != Status::Ok) return 1;
            time = backend.GetStats().emuUs;
        }
        if (backend.GetStats().pageFetches != warmFetches) return 1;
        std::sort(times.begin(), times.end());
        std::printf("timeout_us=%llu n=%zu instr=%llu min_us=%llu p50_us=%llu p95_us=%llu\n",
            static_cast<unsigned long long>(timeout), times.size(),
            static_cast<unsigned long long>(result.instructions),
            static_cast<unsigned long long>(times.front()),
            static_cast<unsigned long long>((times[19] + times[20]) / 2),
            static_cast<unsigned long long>(times[37]));
    }
    return 0;
}
