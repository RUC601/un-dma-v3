#include "Game/EngineTrace/UnicornBackend.hpp"
#include "Game/EngineTrace/VisionProbe.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
// Echoes dword [rcx+0x00] (request.start.x) into [rdx+0x30] (result.fraction) to verify
// exact per-slot ordering across multi-engine waves of varying sizes.
constexpr unsigned char kEchoStartXToFraction[] = {
    0x8B, 0x01,             // mov eax, [rcx]
    0x89, 0x42, 0x30,       // mov [rdx+0x30], eax
    0xC3
};
// Phase 3: branches on low byte of [rcx+0x58] (per-request mask).
//   mask low byte != 0x55 -> writes fraction=1.0f (miss, no hitPos/hitNormal)
//   mask low byte == 0x55 -> writes hitPos=(10,2,5) at +0x00, hitNormal=(3,0,4) at +0x10,
//                            and fraction=0.25f at +0x30.
constexpr unsigned char kMaskBranchAndHitNormal[] = {
    0x8A, 0x41, 0x58,                               // mov al, [rcx+0x58]
    0x3C, 0x55,                                     // cmp al, 0x55
    0x74, 0x0B,                                     // je +11
    0xC7, 0x42, 0x30, 0x00, 0x00, 0x80, 0x3F,       // mov dword [rdx+0x30], 1.0f
    0xC3,                                           // ret
    0x90, 0x90, 0x90,                               // pad to +11
    0xC7, 0x02, 0x00, 0x00, 0x20, 0x41,             // mov dword [rdx+0x00], 10.0f
    0xC7, 0x42, 0x04, 0x00, 0x00, 0x00, 0x40,       // mov dword [rdx+0x04], 2.0f
    0xC7, 0x42, 0x08, 0x00, 0x00, 0xA0, 0x40,       // mov dword [rdx+0x08], 5.0f
    0xC7, 0x42, 0x10, 0x00, 0x00, 0x40, 0x40,       // mov dword [rdx+0x10], 3.0f
    0xC7, 0x42, 0x14, 0x00, 0x00, 0x00, 0x00,       // mov dword [rdx+0x14], 0.0f
    0xC7, 0x42, 0x18, 0x00, 0x00, 0x80, 0x40,       // mov dword [rdx+0x18], 4.0f
    0xC7, 0x42, 0x30, 0x00, 0x00, 0x80, 0x3E,       // mov dword [rdx+0x30], 0.25f
    0xC3                                            // ret
};
// Phase 3 edge case: blocking hit at fraction=0.0f, exact world origin (0,0,0), and NaN hitNormal.
constexpr unsigned char kOriginHitZeroFractionNanNormal[] = {
    0xC7, 0x02, 0x00, 0x00, 0x00, 0x00,             // mov dword [rdx+0x00], 0.0f
    0xC7, 0x42, 0x04, 0x00, 0x00, 0x00, 0x00,       // mov dword [rdx+0x04], 0.0f
    0xC7, 0x42, 0x08, 0x00, 0x00, 0x00, 0x00,       // mov dword [rdx+0x08], 0.0f
    0xC7, 0x42, 0x10, 0x00, 0x00, 0xC0, 0x7F,       // mov dword [rdx+0x10], qNaN
    0xC7, 0x42, 0x30, 0x00, 0x00, 0x00, 0x00,       // mov dword [rdx+0x30], 0.0f
    0xC3                                            // ret
};

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
        backend.GetStats().lastFailedPage == kData &&
        backend.GetStats().pageReadRetries == config.pageReadRetries &&
        backend.GetStats().pageUnavailableTraces == 1,
        "missing page fails after bounded retries");
    fixture.dataAvailable = true;
    check(backend.Trace(request, result) == Status::Ok && result.fraction == 0.25f &&
        fixture.dataReads == 1 + config.pageReadRetries + 1,
        "private cache retries the missing page on the next request");
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
        shared.dataReads == 2 * (1 + config.pageReadRetries) + 1,
        "shared cache retries then shares the successful read");

    // Phase 2 (§6.1): 6-engine concurrent VisionProbe batch sharing one PageStore.
    VisionProbe::Config sixConfig;
    sixConfig.context = kContext;
    sixConfig.entryRva = 0;
    sixConfig.engines = 6;
    sixConfig.timeoutUs = 0;
    Fixture sixFixture{kReadFraction};
    sixFixture.fractionBits = std::bit_cast<uint32_t>(0.375f);
    VisionProbe sixProbe(sixConfig, sixFixture.Reader(), kImage, kPageSize);
    if (!sixProbe.Start(kScene)) return 2;
    std::vector<Request> sixRequests(6, request);
    const auto sixResults = sixProbe.Probe(sixRequests);
    bool sixOk = sixProbe.EngineCount() == 6 && sixResults.size() == 6 && sixFixture.dataReads == 1;
    for (const auto& r : sixResults) {
        if (r.status != Status::Ok || r.fraction != 0.375f) sixOk = false;
    }
    check(sixOk, "6-engine VisionProbe batch shares page cache and returns all 6 fractions");
    sixProbe.Stop();

    // Finding #16 regression: distinct-result repeated waves of sizes 1, 2, 6, 7 across
    // persistent 6-engine worker pool + clean Stop()/Start() restart recovery.
    {
        VisionProbe::Config waveConfig;
        waveConfig.context = kContext;
        waveConfig.entryRva = 0;
        waveConfig.engines = 6;
        waveConfig.timeoutUs = 0;
        Fixture waveFixture{kEchoStartXToFraction};
        VisionProbe waveProbe(waveConfig, waveFixture.Reader(), kImage, kPageSize);
        if (!waveProbe.Start(kScene)) return 2;
        const std::array<float, 7> distinctFractions{
            0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f};
        bool wavesOrderedOk = true;
        for (size_t waveSize : {size_t{1}, size_t{2}, size_t{6}, size_t{7}, size_t{6}}) {
            std::vector<Request> reqs(waveSize, request);
            for (size_t i = 0; i < waveSize; ++i) {
                reqs[i].start.x = distinctFractions[i];
            }
            const auto res = waveProbe.Probe(reqs);
            if (res.size() != waveSize) {
                wavesOrderedOk = false;
                break;
            }
            for (size_t i = 0; i < waveSize; ++i) {
                if (res[i].status != Status::Ok || res[i].fraction != distinctFractions[i]) {
                    wavesOrderedOk = false;
                    break;
                }
            }
        }
        check(wavesOrderedOk,
              "6-engine VisionProbe preserves exact distinct result ordering across waves 1/2/6/7");

        // Serialized Stop() -> Start(newScene) restart recovery.
        waveProbe.Stop();
        bool restartOk = !waveProbe.Started() && waveProbe.Start(kScene + 1);
        if (restartOk) {
            std::vector<Request> nextReqs(6, request);
            for (size_t i = 0; i < 6; ++i) {
                nextReqs[i].sceneEpoch = kScene + 1;
                nextReqs[i].start.x = distinctFractions[5 - i];
            }
            const auto res = waveProbe.Probe(nextReqs);
            for (size_t i = 0; i < 6; ++i) {
                if (res[i].status != Status::Ok || res[i].fraction != distinctFractions[5 - i]) {
                    restartOk = false;
                    break;
                }
            }
        }
        waveProbe.Stop();
        check(restartOk, "6-engine VisionProbe restarts cleanly on new sceneEpoch");
    }

    // Phase 3: per-request mask propagation (kSetup+0x58) + hitPosition (+0x00) & normalized
    // hitNormal (+0x10) backfill across a concurrent multi-mask VisionProbe wave.
    {
        Fixture maskFixture{kMaskBranchAndHitNormal};
        VisionProbe::Config probeCfg;
        probeCfg.entryRva = 0;
        probeCfg.context = kContext;
        probeCfg.mask = kDefaultWorldMask;
        probeCfg.engines = 2;
        probeCfg.timeoutUs = 0;
        VisionProbe maskProbe(probeCfg, maskFixture.Reader(), kImage, kPageSize);
        const bool started = maskProbe.Start(kScene);
        Request worldReq = request;
        worldReq.mask = kDefaultWorldMask;
        Request barrierReq = request;
        barrierReq.mask = 0x0690000000000055ull;
        const auto res = started ? maskProbe.Probe({worldReq, barrierReq}) : std::vector<Result>{};
        maskProbe.Stop();
        const bool worldMissOk =
            started && res.size() == 2 &&
            res[0].status == Status::Ok &&
            res[0].fraction == 1.0f &&
            !res[0].hasHitPosition &&
            !res[0].hasHitNormal;
        const auto nearf = [](float a, float b) { return std::abs(a - b) <= 1e-4f; };
        const bool barrierHitOk =
            started && res.size() == 2 &&
            res[1].status == Status::Ok &&
            nearf(res[1].fraction, 0.25f) &&
            res[1].hasHitPosition &&
            nearf(res[1].hitPosition.x, 10.0f) &&
            nearf(res[1].hitPosition.y, 2.0f) &&
            nearf(res[1].hitPosition.z, 5.0f) &&
            res[1].hasHitNormal &&
            nearf(res[1].hitNormal.x, 0.6f) &&
            nearf(res[1].hitNormal.y, 0.0f) &&
            nearf(res[1].hitNormal.z, 0.8f);
        check(worldMissOk, "per-request world mask leaves miss hitPosition/hitNormal unset");
        check(barrierHitOk, "per-request mask reaches setup+0x58 and backfills hitPosition + unit hitNormal");

        // Edge case: fraction=0.0f blocking hit at exact origin (0,0,0) with NaN hitNormal
        // must keep hasHitPosition=true at (0,0,0) and reject NaN normal (hasHitNormal=false).
        Fixture originFixture{kOriginHitZeroFractionNanNormal};
        UnicornBackend originBackend(OfflineConfig());
        Result originRes{};
        const bool originBound = Bind(originBackend, originFixture);
        const auto originStatus = originBound ? originBackend.Trace(worldReq, originRes) : Status::Error;
        check(originBound && originStatus == Status::Ok &&
              originRes.fraction == 0.0f &&
              originRes.hasHitPosition &&
              originRes.hitPosition.x == 0.0f &&
              originRes.hitPosition.y == 0.0f &&
              originRes.hitPosition.z == 0.0f &&
              !originRes.hasHitNormal,
              "fraction=0 origin (0,0,0) preserves hasHitPosition and rejects NaN hitNormal");

        // Template-based setup: request.mask must override template bytes at setup+0x58.
        const auto tmpPath = std::filesystem::temp_directory_path() / "v3_phase3_mask_template.bin";
        bool templateMaskOk = false;
        {
            std::array<uint8_t, 0x240> tpl{};
            const uint64_t staleMask = 0x0690000000000011ull;
            std::memcpy(tpl.data() + 0x58, &staleMask, sizeof(staleMask));
            std::memcpy(tpl.data() + 0x1F0, &kContext, sizeof(kContext));
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(tpl.data()), static_cast<std::streamsize>(tpl.size()));
            out.close();

            UnicornConfig tplCfg = OfflineConfig();
            tplCfg.setupTemplatePath = tmpPath.string();
            UnicornBackend tplBackend(tplCfg);
            Result tplRes{};
            if (Bind(tplBackend, maskFixture) &&
                tplBackend.Trace(barrierReq, tplRes) == Status::Ok &&
                nearf(tplRes.fraction, 0.25f) &&
                tplRes.hasHitNormal) {
                templateMaskOk = true;
            }
            std::error_code ec;
            std::filesystem::remove(tmpPath, ec);
        }
        check(templateMaskOk, "template-based setup still honors per-request mask at setup+0x58");
    }

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
