// TraceBench: drives the V3 UniCore backend directly, without the product UI or overlays.
//
// It answers two questions with numbers:
//   1. Once the engine and its pages are warm, how expensive is one more ray (p50/p95)?
//   2. If several rays are cast at the same target, does multi-engine fan-out beat 1 engine?
//
// Supports both explicit --setup <file> (e.g. range-chain-setup.bin) and --auto-chain
// (direct static ClientGame+0x30 + RayScene::Resolve without any template file).

#include "Game/EngineTrace/UnicornBackend.hpp"
#include "Game/EngineTrace/VisionProbe.hpp"
#include "Game/Offsets.hpp"
#include "Game/ProductImageIdentity.hpp"
#include "Game/RaySceneChain.hpp"
#include "Game/RegionalLayout.hpp"

#include <vmmdll.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <psapi.h>

int RunTraceBackendSelfTest();
int RunTraceTimerBench();

namespace {

uint64_t NowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool ParseU64(const std::string& text, uint64_t& value) {
    char* end = nullptr;
    value = std::strtoull(text.c_str(), &end, 0);
    return end && *end == '\0';
}

bool ParseVec3(const std::string& text, OW::EngineTrace::Vec3& out) {
    float x = 0, y = 0, z = 0;
    if (std::sscanf(text.c_str(), "%f,%f,%f", &x, &y, &z) != 3) return false;
    out.x = x;
    out.y = y;
    out.z = z;
    return true;
}

// "x,y,z;x,y,z;..." -> list of endpoints.
bool ParseEndList(const std::string& text, std::vector<OW::EngineTrace::Vec3>& out) {
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = text.find(';', begin);
        const std::string piece =
            text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!piece.empty()) {
            OW::EngineTrace::Vec3 value{};
            if (!ParseVec3(piece, value)) return false;
            out.push_back(value);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return !out.empty();
}

double WorkingSetMb() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return 0.0;
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
}

struct LatencySummary {
    size_t count = 0;
    uint64_t minUs = 0;
    uint64_t p50Us = 0;
    uint64_t p95Us = 0;
    uint64_t p99Us = 0;
    uint64_t maxUs = 0;
    double meanUs = 0.0;
};

LatencySummary SummarizeLatency(std::vector<uint64_t> samples) {
    LatencySummary s{};
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    s.count = samples.size();
    s.minUs = samples.front();
    s.maxUs = samples.back();
    auto pct = [&](double p) -> uint64_t {
        const size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(samples.size()))) - 1;
        return samples[(std::min)(idx, samples.size() - 1)];
    };
    s.p50Us = pct(0.50);
    s.p95Us = pct(0.95);
    s.p99Us = pct(0.99);
    const double total = std::accumulate(samples.begin(), samples.end(), 0.0);
    s.meanUs = total / static_cast<double>(samples.size());
    return s;
}

void PrintLatencySummary(const char* label, const LatencySummary& s) {
    const double p50Hz = s.p50Us > 0 ? 1'000'000.0 / static_cast<double>(s.p50Us) : 0.0;
    const double p95Hz = s.p95Us > 0 ? 1'000'000.0 / static_cast<double>(s.p95Us) : 0.0;
    std::printf(
        "BENCH %s n=%zu min_us=%llu p50_us=%llu (%.1fHz) p95_us=%llu (%.1fHz) "
        "p99_us=%llu max_us=%llu mean_us=%.1f\n",
        label, s.count,
        static_cast<unsigned long long>(s.minUs),
        static_cast<unsigned long long>(s.p50Us), p50Hz,
        static_cast<unsigned long long>(s.p95Us), p95Hz,
        static_cast<unsigned long long>(s.p99Us),
        static_cast<unsigned long long>(s.maxUs),
        s.meanUs);
}

struct LiveTarget {
    VMM_HANDLE vmm = nullptr;
    DWORD pid = 0;
    uint64_t base = 0;
    uint32_t imageSize = 0;
    uint32_t timestamp = 0;
    OW::EngineTrace::UnicornBackend::Reader reader;

    ~LiveTarget() {
        if (vmm) VMMDLL_Close(vmm);
    }

    bool Open(const std::string& processName) {
        char device[] = "fpga";
        char norefresh[] = "-norefresh";
        char* args[] = {const_cast<char*>(""), const_cast<char*>("-device"), device, norefresh};
        vmm = VMMDLL_Initialize(4, args);
        if (!vmm) {
            std::fprintf(stderr, "BENCH: VMMDLL_Initialize failed\n");
            return false;
        }
        char name[128] = {};
        std::snprintf(name, sizeof(name), "%s", processName.c_str());
        if (!VMMDLL_PidGetFromName(vmm, name, &pid) || !pid) {
            std::fprintf(stderr, "BENCH: process %s not found\n", name);
            return false;
        }
        base = VMMDLL_ProcessGetModuleBaseU(vmm, pid, name);
        if (!base) {
            std::fprintf(stderr, "BENCH: module base not found for %s\n", name);
            return false;
        }
        PVMMDLL_MAP_MODULEENTRY entry = nullptr;
        if (VMMDLL_Map_GetModuleFromNameU(vmm, pid, name, &entry, 0) && entry) {
            imageSize = entry->cbImageSize;
            VMMDLL_MemFree(entry);
        }
        VMM_HANDLE handle = vmm;
        DWORD targetPid = pid;
        reader = [handle, targetPid](uint64_t address, void* buffer, size_t size) {
            DWORD actual = 0;
            return VMMDLL_MemReadEx(handle, targetPid, address, static_cast<PBYTE>(buffer),
                       static_cast<DWORD>(size), &actual, VMMDLL_FLAG_NOCACHE) != FALSE &&
                actual == size;
        };
        uint32_t hdrImageSize = 0;
        if (OW::ProductImageIdentity::ReadHeaders(base, reader, timestamp, hdrImageSize) ==
            OW::ProductImageIdentity::Result::Accepted) {
            if (!imageSize) imageSize = hdrImageSize;
        }
        return imageSize != 0;
    }
};

const OW::offset::RuntimeOffsetProfile* MatchProfile(uint32_t timestamp, uint32_t imageSize) {
    using OW::offset::RuntimeProfile;
    for (RuntimeProfile p : {RuntimeProfile::Cn, RuntimeProfile::World155284, RuntimeProfile::Nexon155284}) {
        const auto& prof = OW::offset::ForProfile(p);
        if (prof.imageTimestamp == timestamp && prof.imageSize == imageSize) return &prof;
    }
    return nullptr;
}

bool ResolveAutoChain(const LiveTarget& target, OW::EngineTrace::UnicornConfig& config,
                      bool explicitEntry, bool explicitMask) {
    const auto* prof = MatchProfile(target.timestamp, target.imageSize);
    if (!prof || !prof->clientGameRva || !prof->rayEntryRva) {
        std::fprintf(stderr, "BENCH auto-chain: unsupported image timestamp=0x%X size=0x%X\n",
            target.timestamp, target.imageSize);
        return false;
    }
    uint64_t clientGame = 0;
    if (!target.reader(target.base + prof->clientGameRva, &clientGame, sizeof(clientGame)) ||
        clientGame < 0x10000 || clientGame >= 0x800000000000ull) {
        std::fprintf(stderr, "BENCH auto-chain: unreadable ClientGame at base+0x%llX\n",
            static_cast<unsigned long long>(prof->clientGameRva));
        return false;
    }
    uint64_t raw = 0;
    if (!target.reader(clientGame + OW::offset::LiveGameAdmin_InputOffset, &raw, sizeof(raw)) || !raw) {
        std::fprintf(stderr, "BENCH auto-chain: unreadable ClientGame+0x30\n");
        return false;
    }
    uint64_t context = 0;
    if (prof->gameAdminMode == OW::offset::GameAdminMode::World155284) {
        context = OW::RegionalLayout::World155284::DecodeGameAdmin(raw);
    } else if (prof->gameAdminMode == OW::offset::GameAdminMode::Cn155284) {
        context = OW::RegionalLayout::Cn155284::DecodeGameAdmin(raw);
    } else {
        return false;
    }
    if (prof->rayContextVtableRva) {
        uint64_t vtable = 0;
        if (!target.reader(context, &vtable, sizeof(vtable)) ||
            vtable != target.base + prof->rayContextVtableRva) {
            std::fprintf(stderr, "BENCH auto-chain: context 0x%llX vtable mismatch (got 0x%llX)\n",
                static_cast<unsigned long long>(context), static_cast<unsigned long long>(vtable));
            return false;
        }
    }
    const auto decode = prof->raySceneDecode == OW::offset::RaySceneDecode::World155284
        ? OW::RayScene::Decode::World155284
        : OW::RayScene::Decode::CnNexon;
    OW::RayScene::Chain chain{};
    if (!OW::RayScene::Resolve(context, target.base, decode, prof->raySceneSub60VtableRva,
            target.reader, chain)) {
        std::fprintf(stderr, "BENCH auto-chain: RayScene::Resolve failed (raw220=0x%llX sub60=0x%llX)\n",
            static_cast<unsigned long long>(chain.raw220),
            static_cast<unsigned long long>(chain.sub60));
        return false;
    }
    if (!explicitEntry) config.entryRva = prof->rayEntryRva;
    if (!explicitMask) config.mask = prof->rayMask;
    config.context = context;
    config.filterObject = chain.setup1e8;
    if (prof->rayAuxCallbackRva && !config.auxCallback) {
        config.auxCallback = target.base + prof->rayAuxCallbackRva;
    }
    std::printf(
        "BENCH auto-chain profile=%s pid=%lu base=0x%llX entry=0x%llX mask=0x%llX "
        "context=0x%llX setup_1e8=0x%llX\n",
        prof->name, static_cast<unsigned long>(target.pid),
        static_cast<unsigned long long>(target.base),
        static_cast<unsigned long long>(config.entryRva),
        static_cast<unsigned long long>(config.mask),
        static_cast<unsigned long long>(config.context),
        static_cast<unsigned long long>(config.filterObject));
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0) return RunTraceBackendSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "--timer-bench") == 0) return RunTraceTimerBench();

    OW::EngineTrace::UnicornConfig config;
    OW::EngineTrace::Vec3 start{};
    std::vector<OW::EngineTrace::Vec3> ends;
    bool haveStart = false;
    bool autoChain = false;
    bool explicitEntry = false;
    bool explicitMask = false;
    bool quiet = false;
    int warmup = 1;
    int repeat = 1;
    int engines = 0;
    bool concurrent = false;
    bool vision = false;
    uint64_t sceneEpoch = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--process" && i + 1 < argc) config.processName = argv[++i];
        else if (arg == "--entry" && i + 1 < argc) { ParseU64(argv[++i], config.entryRva); explicitEntry = true; }
        else if (arg == "--setup" && i + 1 < argc) config.setupTemplatePath = argv[++i];
        else if (arg == "--context" && i + 1 < argc) ParseU64(argv[++i], config.context);
        else if (arg == "--filter-object" && i + 1 < argc) ParseU64(argv[++i], config.filterObject);
        else if (arg == "--aux-callback" && i + 1 < argc) ParseU64(argv[++i], config.auxCallback);
        else if (arg == "--mask" && i + 1 < argc) { ParseU64(argv[++i], config.mask); explicitMask = true; }
        else if (arg == "--timeout-us" && i + 1 < argc) ParseU64(argv[++i], config.timeoutUs);
        else if (arg == "--auto-chain") autoChain = true;
        else if (arg == "--quiet") quiet = true;
        else if (arg == "--warmup" && i + 1 < argc) warmup = std::atoi(argv[++i]);
        else if (arg == "--repeat" && i + 1 < argc) repeat = std::atoi(argv[++i]);
        else if (arg == "--refresh") config.refreshTouchedPages = true;
        else if (arg == "--no-code-hook") config.skipCodeHook = true;
        else if (arg == "--engines" && i + 1 < argc) engines = std::atoi(argv[++i]);
        else if (arg == "--vision") vision = true;
        else if (arg == "--concurrent") concurrent = true;
        else if (arg == "--scene-epoch" && i + 1 < argc) ParseU64(argv[++i], sceneEpoch);
        else if (arg == "--start" && i + 1 < argc) haveStart = ParseVec3(argv[++i], start);
        else if (arg == "--end" && i + 1 < argc) {
            if (!ParseEndList(argv[++i], ends)) {
                std::fprintf(stderr, "bad --end list\n");
                return 1;
            }
        } else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", arg.c_str());
            return 1;
        }
    }
    if (!haveStart || ends.empty()) {
        std::fprintf(stderr, "--start x,y,z and --end x,y,z[;x,y,z...] are required\n");
        return 1;
    }

    // Open target once via VMMDLL when --vision or --auto-chain is active so all engines
    // share the single FPGA handle without re-initializing VMMDLL.
    std::unique_ptr<LiveTarget> liveTarget;
    if (vision || autoChain) {
        liveTarget = std::make_unique<LiveTarget>();
        if (!liveTarget->Open(config.processName)) return 2;
        if (autoChain && !ResolveAutoChain(*liveTarget, config, explicitEntry, explicitMask)) return 2;
    }

    if (vision) {
        const double baseMb = WorkingSetMb();
        OW::EngineTrace::VisionProbe::Config visionConfig;
        visionConfig.processName = config.processName;
        visionConfig.entryRva = config.entryRva;
        visionConfig.setupTemplatePath = config.setupTemplatePath;
        visionConfig.mask = config.mask;
        visionConfig.context = config.context;
        visionConfig.filterObject = config.filterObject;
        visionConfig.auxCallback = config.auxCallback;
        visionConfig.timeoutUs = config.timeoutUs;
        visionConfig.instructionLimit = config.instructionLimit;
        visionConfig.refreshTouchedPages = config.refreshTouchedPages;
        visionConfig.engines = engines > 0 ? engines : 6;
        OW::EngineTrace::VisionProbe probe(
            visionConfig, liveTarget->reader, liveTarget->base, liveTarget->imageSize);
        if (!probe.Start(sceneEpoch)) {
            std::fprintf(stderr, "BENCH vision: Start failed\n");
            return 2;
        }
        const double boundMb = WorkingSetMb();
        std::printf(
            "BENCH vision started engines=%d timeout_us=%llu context=0x%llX base=0x%llX "
            "ws_before=%.1fMB ws_bound=%.1fMB\n",
            visionConfig.engines, static_cast<unsigned long long>(visionConfig.timeoutUs),
            static_cast<unsigned long long>(probe.Context()),
            static_cast<unsigned long long>(liveTarget->base), baseMb, boundMb);
        std::vector<OW::EngineTrace::Request> requests;
        for (const auto& end : ends) {
            OW::EngineTrace::Request request;
            request.start = start;
            request.end = end;
            request.mask = config.mask;
            request.sceneEpoch = sceneEpoch;
            requests.push_back(request);
        }
        for (int w = 0; w < warmup; ++w) {
            const uint64_t t0 = NowUs();
            const auto results = probe.Probe(requests);
            const uint64_t dt = NowUs() - t0;
            unsigned okCount = 0, blocked = 0;
            for (const auto& r : results) {
                if (r.status == OW::EngineTrace::Status::Ok) {
                    ++okCount;
                    if (r.fraction < 1.0f) ++blocked;
                }
            }
            std::printf("BENCH vision warmup=%d rays=%zu ok=%u blocked=%u wall_us=%llu ws=%.1fMB\n",
                w, results.size(), okCount, blocked, static_cast<unsigned long long>(dt), WorkingSetMb());
        }
        std::vector<uint64_t> roundWallUs;
        std::vector<uint64_t> rayLatencyUs;
        unsigned totalOk = 0, totalFailed = 0, totalBlocked = 0;
        const int rounds = repeat > 0 ? repeat : 1;
        roundWallUs.reserve(static_cast<size_t>(rounds));
        rayLatencyUs.reserve(static_cast<size_t>(rounds) * requests.size());
        for (int round = 0; round < rounds; ++round) {
            const uint64_t wall0 = NowUs();
            const auto results = probe.Probe(requests);
            const uint64_t wallUs = NowUs() - wall0;
            roundWallUs.push_back(wallUs);
            int index = 0;
            for (const auto& result : results) {
                rayLatencyUs.push_back(result.latencyUs);
                if (result.status == OW::EngineTrace::Status::Ok) {
                    ++totalOk;
                    if (result.fraction < 1.0f) ++totalBlocked;
                } else {
                    ++totalFailed;
                }
                if (!quiet) {
                    std::printf("BENCH vision ray=%d status=%d fraction=%.6f instr=%llu us=%u\n",
                        index++, static_cast<int>(result.status),
                        static_cast<double>(result.fraction),
                        static_cast<unsigned long long>(result.instructions), result.latencyUs);
                }
            }
            if (!quiet) {
                std::printf("BENCH vision round=%d rays=%zu wall_us=%llu\n", round, results.size(),
                    static_cast<unsigned long long>(wallUs));
            }
        }
        PrintLatencySummary("vision_warm_round", SummarizeLatency(roundWallUs));
        PrintLatencySummary("vision_warm_ray", SummarizeLatency(rayLatencyUs));
        std::printf(
            "BENCH vision totals ok=%u blocked=%u failed=%u healthy=%d ws_warm=%.1fMB\n",
            totalOk, totalBlocked, totalFailed, probe.Healthy() ? 1 : 0, WorkingSetMb());
        probe.Stop();
        return totalFailed == 0 ? 0 : 3;
    }

    if (engines > 0) {
        // Multi-engine fan-out: one Unicorn engine per ray, one shared target + PageStore.
        const double baseMb = WorkingSetMb();
        auto store = std::make_shared<OW::EngineTrace::PageStore>();
        std::vector<std::unique_ptr<OW::EngineTrace::UnicornBackend>> pool;
        for (int e = 0; e < engines; ++e) {
            auto backend = std::make_unique<OW::EngineTrace::UnicornBackend>(config);
            backend->SetPageStore(store);
            if (liveTarget) {
                backend->SetReader(liveTarget->reader);
                backend->SetTarget(liveTarget->base, liveTarget->imageSize);
            }
            if (!backend->BindScene(sceneEpoch)) {
                std::fprintf(stderr, "BENCH engine %d bind failed\n", e);
                return 2;
            }
            pool.push_back(std::move(backend));
        }
        const double boundMb = WorkingSetMb();
        std::printf(
            "BENCH engines=%d timeout_us=%llu no_code_hook=%d bound ws_before=%.1fMB ws_after=%.1fMB delta=%.1fMB\n",
            engines, static_cast<unsigned long long>(config.timeoutUs),
            config.skipCodeHook ? 1 : 0, baseMb, boundMb, boundMb - baseMb);

        auto runOne = [&](int index, OW::EngineTrace::Result& result, bool log) {
            OW::EngineTrace::Request req;
            req.start = start;
            req.end = ends[static_cast<size_t>(index) % ends.size()];
            req.mask = config.mask;
            req.sceneEpoch = sceneEpoch;
            const auto status = pool[static_cast<size_t>(index)]->Trace(req, result);
            if (log) {
                std::printf("BENCH engine=%d status=%d fraction=%.6f instr=%llu us=%u\n", index,
                    static_cast<int>(status), static_cast<double>(result.fraction),
                    static_cast<unsigned long long>(result.instructions), result.latencyUs);
            }
        };
        for (int w = 0; w < (std::max)(1, warmup); ++w) {
            const uint64_t t0 = NowUs();
            for (int e = 0; e < engines; ++e) {
                OW::EngineTrace::Result r{};
                runOne(e, r, !quiet);
            }
            std::printf(
                "BENCH warmup=%d wall_us=%llu ws=%.1fMB store_fetches=%llu store_hits=%llu\n",
                w, static_cast<unsigned long long>(NowUs() - t0), WorkingSetMb(),
                static_cast<unsigned long long>(store->stats.fetches),
                static_cast<unsigned long long>(store->stats.hits));
        }

        const int rounds = repeat > 1 ? repeat : 1;
        std::vector<uint64_t> roundWallUs;
        std::vector<uint64_t> rayLatencyUs;
        roundWallUs.reserve(static_cast<size_t>(rounds));
        rayLatencyUs.reserve(static_cast<size_t>(rounds) * static_cast<size_t>(engines));
        unsigned totalOk = 0, totalFailed = 0, totalBlocked = 0;
        const uint64_t totalWall0 = NowUs();
        for (int round = 0; round < rounds; ++round) {
            std::vector<OW::EngineTrace::Result> results(static_cast<size_t>(engines));
            const uint64_t r0 = NowUs();
            if (concurrent) {
                std::vector<std::thread> workers;
                workers.reserve(static_cast<size_t>(engines));
                for (int e = 0; e < engines; ++e) {
                    workers.emplace_back([&, e]() { runOne(e, results[static_cast<size_t>(e)], !quiet); });
                }
                for (auto& worker : workers) worker.join();
            } else {
                for (int e = 0; e < engines; ++e) runOne(e, results[static_cast<size_t>(e)], !quiet);
            }
            roundWallUs.push_back(NowUs() - r0);
            for (const auto& r : results) {
                rayLatencyUs.push_back(r.latencyUs);
                if (r.status == OW::EngineTrace::Status::Ok) {
                    ++totalOk;
                    if (r.fraction < 1.0f) ++totalBlocked;
                } else {
                    ++totalFailed;
                }
            }
        }
        const uint64_t totalWallUs = NowUs() - totalWall0;
        PrintLatencySummary("pool_warm_round", SummarizeLatency(roundWallUs));
        PrintLatencySummary("pool_warm_ray", SummarizeLatency(rayLatencyUs));
        std::printf(
            "BENCH engines=%d rounds=%d total_wall_us=%llu per_round_us=%llu concurrent=%d "
            "ok=%u blocked=%u failed=%u ws_warm=%.1fMB store_fetches=%llu store_hits=%llu\n",
            engines, rounds, static_cast<unsigned long long>(totalWallUs),
            static_cast<unsigned long long>(totalWallUs / static_cast<uint64_t>(rounds)),
            concurrent ? 1 : 0, totalOk, totalBlocked, totalFailed, WorkingSetMb(),
            static_cast<unsigned long long>(store->stats.fetches),
            static_cast<unsigned long long>(store->stats.hits));
        return totalFailed == 0 ? 0 : 3;
    }

    const double baseMb = WorkingSetMb();
    OW::EngineTrace::UnicornBackend backend(config);
    if (liveTarget) {
        backend.SetReader(liveTarget->reader);
        backend.SetTarget(liveTarget->base, liveTarget->imageSize);
    }
    if (!backend.BindScene(sceneEpoch)) {
        std::fprintf(stderr, "BENCH bind failed (process, base, or context vtable)\n");
        return 2;
    }
    std::printf("BENCH bind ok base=0x%llX timeout_us=%llu no_code_hook=%d rays=%zu warmup=%d repeat=%d ws=%.1fMB\n",
        static_cast<unsigned long long>(backend.Base()),
        static_cast<unsigned long long>(config.timeoutUs),
        config.skipCodeHook ? 1 : 0, ends.size(), warmup, repeat, WorkingSetMb());

    OW::EngineTrace::Request request;
    request.start = start;
    request.mask = config.mask;
    request.sceneEpoch = sceneEpoch;

    for (int w = 0; w < warmup; ++w) {
        const uint64_t before = backend.GetStats().pageFetches;
        const uint64_t t0 = NowUs();
        unsigned okCount = 0, blocked = 0;
        for (size_t k = 0; k < ends.size(); ++k) {
            request.end = ends[k];
            OW::EngineTrace::Result result;
            const auto status = backend.Trace(request, result);
            if (status == OW::EngineTrace::Status::Ok) {
                ++okCount;
                if (result.fraction < 1.0f) ++blocked;
            }
            if (!quiet) {
                std::printf("BENCH warmup=%d idx=%zu status=%d fraction=%.6f instr=%llu us=%u\n",
                    w, k, static_cast<int>(status), static_cast<double>(result.fraction),
                    static_cast<unsigned long long>(result.instructions), result.latencyUs);
            }
        }
        std::printf("BENCH warmup=%d rays=%zu ok=%u blocked=%u wall_us=%llu new_pages=%llu ws=%.1fMB\n",
            w, ends.size(), okCount, blocked, static_cast<unsigned long long>(NowUs() - t0),
            static_cast<unsigned long long>(backend.GetStats().pageFetches - before), WorkingSetMb());
    }

    std::vector<uint64_t> roundWallUs;
    std::vector<uint64_t> rayLatencyUs;
    roundWallUs.reserve(static_cast<size_t>(repeat));
    rayLatencyUs.reserve(static_cast<size_t>(repeat) * ends.size());
    uint64_t batchUs = 0;
    unsigned totalOk = 0, totalFailed = 0, totalBlocked = 0;
    int rayIndex = 0;
    for (int round = 0; round < repeat; ++round) {
        const uint64_t before = backend.GetStats().pageFetches;
        const uint64_t mappedBefore = backend.GetStats().mappedPages;
        const uint64_t round0 = NowUs();
        uint64_t roundUs = 0;
        for (size_t k = 0; k < ends.size(); ++k) {
            request.end = ends[k];
            OW::EngineTrace::Result result;
            const OW::EngineTrace::Status status = backend.Trace(request, result);
            roundUs += result.latencyUs;
            rayLatencyUs.push_back(result.latencyUs);
            if (status == OW::EngineTrace::Status::Ok) {
                ++totalOk;
                if (result.fraction < 1.0f) ++totalBlocked;
            } else {
                ++totalFailed;
            }
            ++rayIndex;
            if (!quiet) {
                std::printf("BENCH ray=%d round=%d idx=%zu status=%d fraction=%.6f flags=0x%X "
                            "instr=%llu us=%u new_pages=%llu new_mapped=%llu "
                            "page_read_failures=%llu last_failed_page=0x%llX\n",
                    rayIndex, round, k, static_cast<int>(status),
                    static_cast<double>(result.fraction), result.hitFlags,
                    static_cast<unsigned long long>(result.instructions), result.latencyUs,
                    static_cast<unsigned long long>(backend.GetStats().pageFetches - before),
                    static_cast<unsigned long long>(backend.GetStats().mappedPages - mappedBefore),
                    static_cast<unsigned long long>(backend.GetStats().pageReadFailures),
                    static_cast<unsigned long long>(backend.GetStats().lastFailedPage));
            }
        }
        roundWallUs.push_back(NowUs() - round0);
        batchUs += roundUs;
        if (!quiet) {
            std::printf("BENCH round=%d rays=%zu total_us=%llu\n", round, ends.size(),
                static_cast<unsigned long long>(roundUs));
        }
    }

    const auto& stats = backend.GetStats();
    PrintLatencySummary("single_warm_round", SummarizeLatency(roundWallUs));
    PrintLatencySummary("single_warm_ray", SummarizeLatency(rayLatencyUs));
    std::printf("BENCH split last_setup_us=%llu last_prep_us=%llu last_emu_us=%llu\n",
        static_cast<unsigned long long>(stats.setupUs),
        static_cast<unsigned long long>(stats.prepUs),
        static_cast<unsigned long long>(stats.emuUs));
    std::printf("BENCH summary traces=%llu ok=%u blocked=%u failed=%u total_us=%llu avg_us_per_ray=%llu "
                "page_fetches=%llu mapped_pages=%llu faults=%llu max_us=%llu ws_before=%.1fMB ws_warm=%.1fMB\n",
        static_cast<unsigned long long>(stats.traces),
        totalOk, totalBlocked, totalFailed,
        static_cast<unsigned long long>(batchUs),
        static_cast<unsigned long long>(rayIndex ? batchUs / static_cast<uint64_t>(rayIndex) : 0),
        static_cast<unsigned long long>(stats.pageFetches),
        static_cast<unsigned long long>(stats.mappedPages),
        static_cast<unsigned long long>(stats.faults),
        static_cast<unsigned long long>(stats.maxUs),
        baseMb, WorkingSetMb());
    backend.ReleaseScene();
    return totalFailed == 0 ? 0 : 3;
}
