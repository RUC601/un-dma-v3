// Live, read-only validation of the product ray path:
//
//   profile (image timestamp/size) -> TryGetRayContext() -> VisionProbe -> fraction
//
// No setup template file is used unless --ray-setup is given, so the default run
// exercises the template-free 0x240 layout in UnicornBackend. The mode never opens the
// overlay, never starts output and never writes target memory.

#include "runtime/RayProbeCheck.hpp"

#include "Game/Decrypt.hpp"
#include "Game/EngineTrace/VisionProbe.hpp"
#include "Game/Offsets.hpp"
#include "Game/ProductImageIdentity.hpp"
#include "Game/RayRuntime.hpp"
#include "Game/RaySceneChain.hpp"
#include "Game/SDK.hpp"
#include "Game/profiles/LayoutProbe.hpp"
#include "Game/profiles/ProfileState.hpp"
#include "Memory/Memory.h"
#include "Utils/Diagnostics.hpp"
#include "Utils/ProcessConnection.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

using OW::EngineTrace::Request;
using OW::EngineTrace::Result;
using OW::EngineTrace::Status;
using OW::EngineTrace::Vec3;

struct Endpoint {
    Vec3 start{};
    Vec3 end{};
    const char* label = "";
};

bool ParseVec3(const std::string& text, Vec3& out) {
    float x = 0, y = 0, z = 0;
    if (std::sscanf(text.c_str(), "%f,%f,%f", &x, &y, &z) != 3) return false;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    out = {x, y, z};
    return true;
}

bool ParseEndpoints(const std::string& text, Vec3 start, std::vector<Endpoint>& out) {
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = text.find(';', begin);
        const std::string piece =
            text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!piece.empty()) {
            Vec3 value{};
            if (!ParseVec3(piece, value)) return false;
            out.push_back({start, value, "requested"});
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return !out.empty();
}

bool ParseU64(const std::string& text, uint64_t& value) {
    char* end = nullptr;
    value = std::strtoull(text.c_str(), &end, 0);
    return end && *end == '\0';
}

const char* SceneStateName(OW::EngineTrace::VisionProbe::SceneState state) {
    using State = OW::EngineTrace::VisionProbe::SceneState;
    switch (state) {
        case State::Healthy: return "healthy";
        case State::Changed: return "changed";
        case State::Unavailable: return "unavailable";
    }
    return "unknown";
}

const char* StatusName(Status status) {
    switch (status) {
        case Status::Ok: return "ok";
        case Status::Unavailable: return "unavailable";
        case Status::Timeout: return "timeout";
        case Status::InvalidScene: return "invalid-scene";
        case Status::Error: return "error";
    }
    return "error";
}

bool ReadHeaderBytes(uint64_t address, void* buffer, size_t size) {
    return mem.Read(static_cast<uintptr_t>(address), buffer, size);
}

void PrintResult(size_t index, const Result& result) {
    std::printf(
        "RAY_PROBE ray=%zu status=%s fraction=%.6f flags=0x%X hitbox=0x%X entity=0x%X "
        "instructions=%llu latency_us=%u\n",
        index, StatusName(result.status), static_cast<double>(result.fraction),
        result.hitFlags, result.hitbox, result.entityId,
        static_cast<unsigned long long>(result.instructions), result.latencyUs);
}

}  // namespace

int RunRayProbeCheck(int argc, char** argv) {
    std::string setupTemplate;
    std::string seedFilePath;
    std::string endList;
    Vec3 start{};
    bool haveStart = false;
    uint64_t mask = 0;
    uint64_t filterObject = 0;
    uint64_t sceneObject2 = 0;
    uint64_t auxCallback = 0;
    bool auxExplicit = false;
    uint32_t filterCount = 0;
    uint64_t filterItems[4] = {};
    uint64_t localObject = 0;
    bool captureSeed = false;
    bool captureHint = false;
    bool seedEndpoints = false;
    bool rayChain = false;
    uint64_t filterItem = 0;
    bool haveFilterItem = false;
    std::string seedSavePath;
    int captureSeconds = 10;
    int engines = 1;
    int repeat = 1;
    int watchSeconds = 0;
    uint64_t instructionLimit = 4'000'000;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument == "--ray-setup" && index + 1 < argc) setupTemplate = argv[++index];
        else if (argument == "--ray-seed-file" && index + 1 < argc) seedFilePath = argv[++index];
        else if (argument == "--ray-capture-seed") captureSeed = true;
        else if (argument == "--ray-capture-hint") captureHint = true;
        else if (argument == "--ray-chain") rayChain = true;
        else if (argument == "--ray-filter-item" && index + 1 < argc) {
            if (!ParseU64(argv[++index], filterItem)) {
                std::fprintf(stderr, "[RAY-PROBE] bad --ray-filter-item\n");
                return 2;
            }
            haveFilterItem = true;
        }
        else if (argument == "--ray-seed-endpoints") seedEndpoints = true;
        else if (argument == "--ray-seed-save" && index + 1 < argc) seedSavePath = argv[++index];
        else if (argument == "--ray-capture-seconds" && index + 1 < argc) {
            captureSeconds = std::atoi(argv[++index]);
        }
        else if (argument == "--ray-filter-object" && index + 1 < argc) {
            if (!ParseU64(argv[++index], filterObject)) {
                std::fprintf(stderr, "[RAY-PROBE] bad --ray-filter-object\n");
                return 2;
            }
        }
        else if (argument == "--ray-scene-object" && index + 1 < argc) {
            if (!ParseU64(argv[++index], sceneObject2)) {
                std::fprintf(stderr, "[RAY-PROBE] bad --ray-scene-object\n");
                return 2;
            }
        }
        else if (argument == "--ray-aux-callback" && index + 1 < argc) {
            if (!ParseU64(argv[++index], auxCallback)) {
                std::fprintf(stderr, "[RAY-PROBE] bad --ray-aux-callback\n");
                return 2;
            }
            auxExplicit = true;
        }
        else if (argument == "--ray-start" && index + 1 < argc) {
            if (!ParseVec3(argv[++index], start)) {
                std::fprintf(stderr, "[RAY-PROBE] bad --ray-start\n");
                return 2;
            }
            haveStart = true;
        } else if (argument == "--ray-end" && index + 1 < argc) endList = argv[++index];
        else if (argument == "--ray-mask" && index + 1 < argc) {
            if (!ParseU64(argv[++index], mask)) {
                std::fprintf(stderr, "[RAY-PROBE] bad --ray-mask\n");
                return 2;
            }
        } else if (argument == "--ray-engines" && index + 1 < argc) {
            engines = std::atoi(argv[++index]);
        } else if (argument == "--ray-repeat" && index + 1 < argc) {
            repeat = std::atoi(argv[++index]);
        } else if (argument == "--ray-watch" && index + 1 < argc) {
            watchSeconds = std::atoi(argv[++index]);
        } else if (argument == "--ray-limit" && index + 1 < argc) {
            if (!ParseU64(argv[++index], instructionLimit)) {
                std::fprintf(stderr, "[RAY-PROBE] bad --ray-limit\n");
                return 2;
            }
        }
    }

    const int requested = V2::Profiles::requestedProfile.load();
    if (requested >= 0) {
        OW::offset::SetActiveProfile(V2::Profiles::profiles[static_cast<size_t>(requested)].runtime);
    } else {
        OW::offset::SetActiveProfile(OW::offset::kProductRuntimeProfile);
    }

    std::printf("[RAY-PROBE] read-only live check (no setup template by default)\n");
    if (!mem.LoadDmaDeviceConfig()) {
        std::fprintf(stderr, "[RAY-PROBE] DMA config load failed\n");
        return 3;
    }
    if (!mem.InitDma(false)) {
        std::fprintf(stderr, "[RAY-PROBE] DMA init failed\n");
        return 3;
    }
    if (!mem.RefreshProcessMetadata("ray_probe")) {
        std::fprintf(stderr, "[RAY-PROBE] process metadata unavailable\n");
        mem.CloseDma();
        return 3;
    }
    if (!OW::SDK->Initialize()) {
        std::fprintf(stderr, "[RAY-PROBE] Overwatch.exe not attached\n");
        mem.CloseDma();
        return 3;
    }

    const uint64_t base = OW::SDK->dwGameBase.load();
    uint32_t timestamp = 0, imageSize = 0;
    const auto identity =
        OW::ProductImageIdentity::ReadHeaders(base, ReadHeaderBytes, timestamp, imageSize);
    const int imageProfile = V2::Profiles::FindImageProfile(timestamp, imageSize);
    if (identity != OW::ProductImageIdentity::Result::Accepted || imageProfile < 0 ||
        (requested >= 0 && requested != imageProfile)) {
        std::fprintf(stderr,
            "[RAY-PROBE] unsupported image timestamp=0x%X size=0x%X profile=%d requested=%d\n",
            timestamp, imageSize, imageProfile, requested);
        OW::SDK->Reset();
        mem.CloseDma();
        return 3;
    }
    const auto& selected = V2::Profiles::profiles[static_cast<size_t>(imageProfile)];
    OW::offset::SetActiveProfile(selected.runtime);
    const auto& offsets = OW::offset::Active();
    std::printf(
        "[RAY-PROBE] pid=%d base=0x%llX image=0x%X timestamp=0x%X profile=%s %s\n",
        static_cast<int>(mem.GetCurrentProcessId()),
        static_cast<unsigned long long>(base), imageSize, timestamp, selected.region,
        selected.build);

    if (!offsets.rayEntryRva) {
        std::fprintf(stderr, "[RAY-PROBE] profile has no verified ray entry RVA\n");
        OW::SDK->Reset();
        mem.CloseDma();
        return 4;
    }
    if (!mask) mask = offsets.rayMask;

    uint64_t context = 0;
    if (!OW::TryGetRayContext(context)) {
        std::fprintf(stderr,
            "[RAY-PROBE] TryGetRayContext failed (clientGame=0x%llX gameAdminMode=%d)\n",
            static_cast<unsigned long long>(offsets.clientGameRva),
            static_cast<int>(offsets.gameAdminMode));
        OW::SDK->Reset();
        mem.CloseDma();
        return 4;
    }
    std::printf(
        "[RAY-PROBE] context=0x%llX vtable_ok=%d client_game_rva=0x%llX entry_rva=0x%llX "
        "mask=0x%llX\n",
        static_cast<unsigned long long>(context), 1,
        static_cast<unsigned long long>(offsets.clientGameRva),
        static_cast<unsigned long long>(offsets.rayEntryRva),
        static_cast<unsigned long long>(mask));

    // Static scene chain: context + 0x220 -> sub60 -> setup + 0x1E8. This is the product
    // path; no stack capture is involved for the scene object.
    OW::RayScene::Chain chain;
    bool haveChain = false;
    if (rayChain) {
        const auto decode = offsets.raySceneDecode == OW::offset::RaySceneDecode::World155284
            ? OW::RayScene::Decode::World155284
            : OW::RayScene::Decode::CnNexon;
        auto read = [](uint64_t address, void* buffer, size_t size) {
            return mem.Read(static_cast<uintptr_t>(address), buffer, size);
        };
        haveChain = OW::RayScene::Resolve(context, base, decode,
            offsets.raySceneSub60VtableRva, read, chain);
        std::printf(
            "[RAY-PROBE] chain ok=%d raw220=0x%llX sub60=0x%llX vtable_ok=%d flag=%u "
            "holder=0x%llX setup_1e8=0x%llX\n",
            haveChain ? 1 : 0, static_cast<unsigned long long>(chain.raw220),
            static_cast<unsigned long long>(chain.sub60), chain.vtableOk ? 1 : 0,
            static_cast<unsigned>(chain.flag),
            static_cast<unsigned long long>(chain.holder),
            static_cast<unsigned long long>(chain.setup1e8));
        if (haveChain) filterObject = chain.setup1e8;
    }
    if (haveFilterItem) {
        filterCount = 1;
        std::memset(filterItems, 0, sizeof(filterItems));
        filterItems[0] = filterItem;
    }

    bool useSeedEndpoints = false;
    Vec3 seedStart{}, seedEnd{};
    if (!seedFilePath.empty()) {
        // Offline/reproducible seed: load the objects from a previously captured 0x240
        // setup instead of scanning the live stacks again.
        std::ifstream file(seedFilePath, std::ios::binary);
        std::array<uint8_t, 0x240> bytes{};
        if (!file || !file.read(reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()))) {
            std::fprintf(stderr, "[RAY-PROBE] cannot read --ray-seed-file %s\n",
                seedFilePath.c_str());
            OW::SDK->Reset();
            mem.CloseDma();
            return 2;
        }
        uint64_t fileAux = 0;
        std::memcpy(&fileAux, bytes.data() + 0x1A0, 8);
        // An explicit --ray-aux-callback wins; otherwise the captured setup's callback is
        // authoritative. The profile RVA is only a last resort (and must be a valid one).
        if (!auxExplicit && fileAux) auxCallback = fileAux;
        uint64_t file1e8 = 0;
        std::memcpy(&file1e8, bytes.data() + 0x1E8, 8);
        if (!(rayChain && haveChain)) filterObject = file1e8;
        else std::printf(
            "[RAY-PROBE] chain_vs_file chain=0x%llX file=0x%llX equal=%d\n",
            static_cast<unsigned long long>(chain.setup1e8),
            static_cast<unsigned long long>(file1e8),
            chain.setup1e8 == file1e8 ? 1 : 0);
        if (!(rayChain && haveChain)) {
            std::memcpy(&sceneObject2, bytes.data() + 0x1F8, 8);
            std::memcpy(&localObject, bytes.data() + 0x40, 8);
        }
        uint32_t count = 0;
        std::memcpy(&count, bytes.data() + 0x68, 4);
        filterCount = (std::min)(count, 4u);
        for (uint32_t i = 0; i < filterCount; ++i)
            std::memcpy(&filterItems[i], bytes.data() + 0x70 + i * 8, 8);
        float s[3]{}, e[3]{};
        std::memcpy(s, bytes.data(), sizeof(s));
        std::memcpy(e, bytes.data() + 0x10, sizeof(e));
        seedStart = {s[0], s[1], s[2]};
        seedEnd = {e[0], e[1], e[2]};
        if (seedEndpoints) useSeedEndpoints = true;
        std::printf(
            "[RAY-PROBE] seed-file=%s local=0x%llX aux=0x%llX scene=0x%llX scene2=0x%llX "
            "filters=%u start=%.3f,%.3f,%.3f end=%.3f,%.3f,%.3f\n",
            seedFilePath.c_str(), static_cast<unsigned long long>(localObject),
            static_cast<unsigned long long>(auxCallback),
            static_cast<unsigned long long>(filterObject),
            static_cast<unsigned long long>(sceneObject2), filterCount,
            seedStart.x, seedStart.y, seedStart.z, seedEnd.x, seedEnd.y, seedEnd.z);
    } else if (captureSeed) {
        Memory::RaySetupSeed seed;
        const float hint[3] = {start.x, start.y, start.z};
        const bool haveHint = captureHint && haveStart &&
            std::isfinite(start.x) && std::isfinite(start.y) && std::isfinite(start.z);
        if (mem.CaptureRaySetupSeed(context, base, imageSize, captureSeconds * 1000, seed,
                haveHint ? hint : nullptr, 3.0f)) {
            if (!(rayChain && haveChain)) filterObject = seed.filterObject;
            else std::printf(
                "[RAY-PROBE] chain_vs_stack chain=0x%llX stack=0x%llX equal=%d\n",
                static_cast<unsigned long long>(chain.setup1e8),
                static_cast<unsigned long long>(seed.filterObject),
                chain.setup1e8 == seed.filterObject ? 1 : 0);
            sceneObject2 = (rayChain && haveChain) ? 0 : seed.sceneObject2;
            localObject = (rayChain && haveChain) ? 0 : seed.localObject;
            if (!haveFilterItem) {
                filterCount = seed.filterCount;
                for (uint32_t i = 0; i < 4; ++i) filterItems[i] = seed.filterItems[i];
            }
            if (!auxExplicit && seed.auxCallback) auxCallback = seed.auxCallback;
            if (!seedSavePath.empty()) {
                std::ofstream file(seedSavePath, std::ios::binary);
                if (file)
                    file.write(reinterpret_cast<const char*>(seed.bytes),
                        static_cast<std::streamsize>(sizeof(seed.bytes)));
            }
            if (seedEndpoints) {
                useSeedEndpoints = true;
                seedStart = {seed.start[0], seed.start[1], seed.start[2]};
                seedEnd = {seed.end[0], seed.end[1], seed.end[2]};
            }
            std::printf(
                "[RAY-PROBE] seed setup=0x%llX local=0x%llX aux=0x%llX scene=0x%llX "
                "scene2=0x%llX filters=%u start=%.3f,%.3f,%.3f end=%.3f,%.3f,%.3f\n",
                static_cast<unsigned long long>(seed.setup),
                static_cast<unsigned long long>(seed.localObject),
                static_cast<unsigned long long>(seed.auxCallback),
                static_cast<unsigned long long>(seed.filterObject),
                static_cast<unsigned long long>(seed.sceneObject2),
                seed.filterCount,
                seed.start[0], seed.start[1], seed.start[2],
                seed.end[0], seed.end[1], seed.end[2]);
        } else {
            std::fprintf(stderr, "[RAY-PROBE] seed capture found no live setup in %ds\n",
                captureSeconds);
        }
    }

    if (!auxExplicit && !auxCallback && offsets.rayAuxCallbackRva)
        auxCallback = base + offsets.rayAuxCallbackRva;

    std::vector<Endpoint> endpoints;
    if (useSeedEndpoints) {
        endpoints.push_back({seedStart, seedEnd, "seed"});
    } else if (!endList.empty()) {
        if (!haveStart || !ParseEndpoints(endList, start, endpoints)) {
            std::fprintf(stderr, "[RAY-PROBE] --ray-end needs --ray-start x,y,z and x,y,z;...\n");
            OW::SDK->Reset();
            mem.CloseDma();
            return 2;
        }
    } else {
        if (!haveStart) start = {0.0f, 200.0f, 0.0f};
        // Default fan: straight-down terrain probes around the map origin. Any hit proves
        // the synthetic setup reached the real world query; the exact hit is reported.
        const float offsets2[] = {0.0f, 50.0f, -50.0f};
        for (float dx : offsets2) {
            for (float dz : offsets2) {
                endpoints.push_back(
                    {{start.x + dx, start.y, start.z + dz},
                     {start.x + dx, start.y - 400.0f, start.z + dz}, "down"});
            }
        }
        endpoints.push_back({{start.x, start.y - 198.0f, start.z},
                             {start.x, start.y + 400.0f, start.z}, "up"});
    }

    OW::EngineTrace::VisionProbe::Config config;
    config.processName = "Overwatch.exe";
    config.entryRva = offsets.rayEntryRva;
    config.mask = mask;
    config.context = context;
    config.setupTemplatePath = setupTemplate;
    config.filterObject = filterObject;
    config.sceneObject2 = sceneObject2;
    config.auxCallback = auxCallback;
    config.filterCount = filterCount;
    for (uint32_t i = 0; i < 4; ++i) config.filterItems[i] = filterItems[i];
    config.localObject = localObject;
    config.engines = (std::max)(1, engines);
    config.instructionLimit = instructionLimit;
    config.timeoutUs = 0;
    config.refreshTouchedPages = false;
    config.resolveContext = [](uint64_t& resolved) { return OW::TryGetRayContext(resolved); };

    OW::EngineTrace::VisionProbe probe(
        config,
        [](uint64_t address, void* buffer, size_t size) { return mem.Read(address, buffer, size); },
        base, imageSize, 0);
    const uint64_t sceneEpoch = 1;
    if (!probe.Start(sceneEpoch)) {
        std::fprintf(stderr, "[RAY-PROBE] VisionProbe::Start failed (template=%s)\n",
            setupTemplate.empty() ? "synthetic" : setupTemplate.c_str());
        OW::SDK->Reset();
        mem.CloseDma();
        return 4;
    }
    char scene2Text[32] = {};
    if (sceneObject2)
        std::snprintf(scene2Text, sizeof(scene2Text), "0x%llX",
            static_cast<unsigned long long>(sceneObject2));
    else
        std::snprintf(scene2Text, sizeof(scene2Text), "scratch");
    std::printf(
        "[RAY-PROBE] probe started engines=%d template=%s context=0x%llX setup_1e8=0x%llX "
        "setup_1f8=%s setup_1a0=0x%llX healthy=%d scene_state=%s\n",
        config.engines, setupTemplate.empty() ? "synthetic" : setupTemplate.c_str(),
        static_cast<unsigned long long>(probe.Context()),
        static_cast<unsigned long long>(filterObject),
        scene2Text,
        static_cast<unsigned long long>(auxCallback), probe.Healthy() ? 1 : 0,
        SceneStateName(probe.CheckScene()));

    std::vector<Request> requests;
    requests.reserve(endpoints.size());
    for (const auto& endpoint : endpoints) {
        Request request;
        request.start = endpoint.start;
        request.end = endpoint.end;
        request.mask = mask;
        request.sceneEpoch = sceneEpoch;
        requests.push_back(request);
    }

    unsigned blocked = 0, clear = 0, failed = 0;
    const int rounds = (std::max)(1, repeat);
    for (int round = 0; round < rounds; ++round) {
        const uint64_t startedMs = GetTickCount64();
        const auto results = probe.Probe(requests);
        for (size_t index = 0; index < results.size(); ++index) {
            PrintResult(index, results[index]);
            if (results[index].status != Status::Ok) ++failed;
            else if (results[index].fraction < 1.0f) ++blocked;
            else ++clear;
        }
        std::printf("[RAY-PROBE] round=%d rays=%zu wall_ms=%llu healthy=%d scene=%s\n", round,
            results.size(), static_cast<unsigned long long>(GetTickCount64() - startedMs),
            probe.Healthy() ? 1 : 0, SceneStateName(probe.CheckScene()));
    }

    const auto& stats = probe.Stats();
    std::printf(
        "[RAY-PROBE] summary blocked=%u clear=%u failed=%u traces=%llu pages=%llu hits=%llu "
        "page_read_failures=%llu page_unavailable_traces=%llu retries=%llu last_page=0x%llX "
        "max_us=%llu\n",
        blocked, clear, failed, static_cast<unsigned long long>(stats.traces),
        static_cast<unsigned long long>(stats.pageFetches),
        static_cast<unsigned long long>(stats.cacheHits),
        static_cast<unsigned long long>(stats.pageReadFailures),
        static_cast<unsigned long long>(stats.pageUnavailableTraces),
        static_cast<unsigned long long>(stats.pageReadRetries),
        static_cast<unsigned long long>(stats.lastFailedPage),
        static_cast<unsigned long long>(stats.maxUs));

    for (int second = 0; second < watchSeconds; ++second) {
        Sleep(1000);
        const auto state = probe.CheckScene();
        std::printf("[RAY-PROBE] watch t=%ds scene=%s alive=%d context=0x%llX\n", second + 1,
            SceneStateName(state), probe.Healthy() ? 1 : 0,
            static_cast<unsigned long long>(probe.Context()));
        if (state != OW::EngineTrace::VisionProbe::SceneState::Healthy) break;
    }

    probe.Stop();
    OW::SDK->Reset();
    mem.CloseDma();
    return failed == 0 ? 0 : 5;
}

