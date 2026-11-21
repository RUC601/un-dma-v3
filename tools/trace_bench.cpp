// TraceBench: drives the V3 UniCore backend directly, without the product UI or overlays.
//
// It answers two questions with numbers:
//   1. Once the engine and its pages are warm, how expensive is one more ray?
//   2. If several rays are cast at the same target, does page sharing keep the batch cheap?
//
// The product itself only ever sees the EngineTrace contract.

#include "Game/EngineTrace/UnicornBackend.hpp"
#include "Game/EngineTrace/VisionProbe.hpp"

#include <vmmdll.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <psapi.h>

int RunTraceBackendSelfTest();
int RunTraceTimerBench();

namespace {

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

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0) return RunTraceBackendSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "--timer-bench") == 0) return RunTraceTimerBench();

    OW::EngineTrace::UnicornConfig config;
    OW::EngineTrace::Vec3 start{};
    std::vector<OW::EngineTrace::Vec3> ends;
    bool haveStart = false;
    int repeat = 1;
    int engines = 0;
    bool concurrent = false;
    bool vision = false;
    uint64_t sceneEpoch = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--process" && i + 1 < argc) config.processName = argv[++i];
        else if (arg == "--entry" && i + 1 < argc) ParseU64(argv[++i], config.entryRva);
        else if (arg == "--setup" && i + 1 < argc) config.setupTemplatePath = argv[++i];
        else if (arg == "--context" && i + 1 < argc) ParseU64(argv[++i], config.context);
        else if (arg == "--mask" && i + 1 < argc) ParseU64(argv[++i], config.mask);
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

    if (vision) {
        // Same composition the product uses: its own reader + VisionProbe + shared store.
        char device[] = "fpga";
        char norefresh[] = "-norefresh";
        char* args[] = {const_cast<char*>(""), const_cast<char*>("-device"), device, norefresh};
        VMM_HANDLE vmm = VMMDLL_Initialize(4, args);
        if (!vmm) {
            std::fprintf(stderr, "BENCH vision: VMMDLL_Initialize failed\n");
            return 2;
        }
        char name[128] = {};
        std::snprintf(name, sizeof(name), "%s", config.processName.c_str());
        DWORD pid = 0;
        if (!VMMDLL_PidGetFromName(vmm, name, &pid) || !pid) {
            std::fprintf(stderr, "BENCH vision: process not found\n");
            return 2;
        }
        const uint64_t base = VMMDLL_ProcessGetModuleBaseU(vmm, pid, name);
        DWORD imageSize = 0;
        PVMMDLL_MAP_MODULEENTRY entry = nullptr;
        if (VMMDLL_Map_GetModuleFromNameU(vmm, pid, name, &entry, 0) && entry) {
            imageSize = entry->cbImageSize;
            VMMDLL_MemFree(entry);
        }
        auto reader = [vmm, pid](uint64_t address, void* buffer, size_t size) {
            DWORD actual = 0;
            return VMMDLL_MemReadEx(vmm, pid, address, static_cast<PBYTE>(buffer),
                       static_cast<DWORD>(size), &actual, VMMDLL_FLAG_NOCACHE) != FALSE &&
                actual == size;
        };
        OW::EngineTrace::VisionProbe::Config visionConfig;
        visionConfig.processName = config.processName;
        visionConfig.entryRva = config.entryRva;
        visionConfig.setupTemplatePath = config.setupTemplatePath;
        visionConfig.mask = config.mask;
        visionConfig.context = config.context;
        visionConfig.engines = engines > 0 ? engines : 6;
        OW::EngineTrace::VisionProbe probe(visionConfig, reader, base, imageSize);
        if (!probe.Start(sceneEpoch)) {
            std::fprintf(stderr, "BENCH vision: Start failed\n");
            return 2;
        }
        std::printf("BENCH vision started engines=%d context=0x%llX base=0x%llX\n",
            visionConfig.engines, static_cast<unsigned long long>(probe.Context()),
            static_cast<unsigned long long>(base));
        std::vector<OW::EngineTrace::Request> requests;
        for (const auto& end : ends) {
            OW::EngineTrace::Request request;
            request.start = start;
            request.end = end;
            request.mask = config.mask;
            request.sceneEpoch = sceneEpoch;
            requests.push_back(request);
        }
        for (int round = 0; round < (repeat > 0 ? repeat : 1); ++round) {
            const uint64_t wall0 = GetTickCount64();
            const auto results = probe.Probe(requests);
            const uint64_t wallUs = (GetTickCount64() - wall0) * 1000;
            int index = 0;
            for (const auto& result : results) {
                std::printf("BENCH vision ray=%d status=%d fraction=%.6f instr=%llu us=%u\n",
                    index++, static_cast<int>(result.status),
                    static_cast<double>(result.fraction),
                    static_cast<unsigned long long>(result.instructions), result.latencyUs);
            }
            std::printf("BENCH vision round=%d rays=%zu wall_us=%llu\n", round, results.size(),
                static_cast<unsigned long long>(wallUs));
        }
        std::printf("BENCH vision healthy=%d\n", probe.Healthy() ? 1 : 0);
        probe.Stop();
        VMMDLL_Close(vmm);
        return 0;
    }

    if (engines > 0) {
        // Multi-engine fan-out: one Unicorn engine per ray, one shared target.
        const double baseMb = WorkingSetMb();
        auto store = std::make_shared<OW::EngineTrace::PageStore>();
        std::vector<std::unique_ptr<OW::EngineTrace::UnicornBackend>> pool;
        for (int e = 0; e < engines; ++e) {
            auto backend = std::make_unique<OW::EngineTrace::UnicornBackend>(config);
            backend->SetPageStore(store);
            if (!backend->BindScene(sceneEpoch)) {
                std::fprintf(stderr, "BENCH engine %d bind failed\n", e);
                return 2;
            }
            pool.push_back(std::move(backend));
        }
        const double boundMb = WorkingSetMb();
        std::printf("BENCH engines=%d bound ws_before=%.1fMB ws_after=%.1fMB delta=%.1fMB\n",
            engines, baseMb, boundMb, boundMb - baseMb);

        auto runOne = [&](int index) {
            OW::EngineTrace::Request req;
            req.start = start;
            req.end = ends[static_cast<size_t>(index) % ends.size()];
            req.mask = config.mask;
            req.sceneEpoch = sceneEpoch;
            OW::EngineTrace::Result result;
            const auto status = pool[static_cast<size_t>(index)]->Trace(req, result);
            std::printf("BENCH engine=%d status=%d fraction=%.6f instr=%llu us=%u\n", index,
                static_cast<int>(status), static_cast<double>(result.fraction),
                static_cast<unsigned long long>(result.instructions), result.latencyUs);
        };
        std::printf("BENCH warmup: one cold ray per engine\n");
        for (int e = 0; e < engines; ++e) runOne(e);
        std::printf("BENCH warmup done ws=%.1fMB store_fetches=%llu store_hits=%llu\n",
            WorkingSetMb(),
            static_cast<unsigned long long>(store->stats.fetches),
            static_cast<unsigned long long>(store->stats.hits));

        const int rounds = repeat > 1 ? repeat : 1;
        const uint64_t wall0 = GetTickCount64();
        for (int round = 0; round < rounds; ++round) {
            if (concurrent) {
                std::vector<std::thread> workers;
                for (int e = 0; e < engines; ++e) workers.emplace_back(runOne, e);
                for (auto& worker : workers) worker.join();
            } else {
                for (int e = 0; e < engines; ++e) runOne(e);
            }
        }
        const uint64_t wallUs = (GetTickCount64() - wall0) * 1000;
        std::printf("BENCH engines=%d rounds=%d wall_us=%llu per_round_us=%llu concurrent=%d\n",
            engines, rounds, static_cast<unsigned long long>(wallUs),
            static_cast<unsigned long long>(wallUs / static_cast<uint64_t>(rounds)),
            concurrent ? 1 : 0);
        return 0;
    }

    OW::EngineTrace::UnicornBackend backend(config);
    if (!backend.BindScene(sceneEpoch)) {
        std::fprintf(stderr, "BENCH bind failed (process, base, or context vtable)\n");
        return 2;
    }
    std::printf("BENCH bind ok base=0x%llX rays=%zu repeat=%d\n",
        static_cast<unsigned long long>(backend.Base()), ends.size(), repeat);

    OW::EngineTrace::Request request;
    request.start = start;
    request.mask = config.mask;
    request.sceneEpoch = sceneEpoch;

    uint64_t batchUs = 0;
    int rayIndex = 0;
    for (int round = 0; round < repeat; ++round) {
        const uint64_t before = backend.GetStats().pageFetches;
        const uint64_t mappedBefore = backend.GetStats().mappedPages;
        uint64_t roundUs = 0;
        for (size_t k = 0; k < ends.size(); ++k) {
            request.end = ends[k];
            OW::EngineTrace::Result result;
            const OW::EngineTrace::Status status = backend.Trace(request, result);
            roundUs += result.latencyUs;
            ++rayIndex;
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
        batchUs += roundUs;
        std::printf("BENCH round=%d rays=%zu total_us=%llu\n", round, ends.size(),
            static_cast<unsigned long long>(roundUs));
    }

    const auto& stats = backend.GetStats();
    std::printf("BENCH split last_setup_us=%llu last_prep_us=%llu last_emu_us=%llu\n",
        static_cast<unsigned long long>(stats.setupUs),
        static_cast<unsigned long long>(stats.prepUs),
        static_cast<unsigned long long>(stats.emuUs));
    std::printf("BENCH summary traces=%llu total_us=%llu avg_us_per_ray=%llu page_fetches=%llu "
                "mapped_pages=%llu faults=%llu max_us=%llu\n",
        static_cast<unsigned long long>(stats.traces),
        static_cast<unsigned long long>(batchUs),
        static_cast<unsigned long long>(rayIndex ? batchUs / static_cast<uint64_t>(rayIndex) : 0),
        static_cast<unsigned long long>(stats.pageFetches),
        static_cast<unsigned long long>(stats.mappedPages),
        static_cast<unsigned long long>(stats.faults),
        static_cast<unsigned long long>(stats.maxUs));
    backend.ReleaseScene();
    return 0;
}






