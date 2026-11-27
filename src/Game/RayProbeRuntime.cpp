#include "Game/RayRuntime.hpp"

#include "Game/Decrypt.hpp"
#include "Game/EngineTrace/VisionProbe.hpp"
#include "Game/Offsets.hpp"
#include "Game/RaySceneChain.hpp"
#include "Game/snapshots/SnapshotAccess.hpp"
#include "Memory/Memory.h"
#include "Utils/Diagnostics.hpp"
#include "Utils/ProcessConnection.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace OW::RayRuntime {
namespace {

constexpr DWORD kWatchIntervalMs = 250;
constexpr DWORD kBindRetryMs = 1000;

// Phase 2 (§6.1, §6.3): default to 6 concurrent Unicorn engines sharing one PageStore so
// a 6-slot batch (Top-1/Top-2/parabolic chords/ground-clip/EdgeFan) finishes in one ~5-7 ms wave.
// Overridable via UN_DMA_RAY_ENGINES in [1, 8].
constexpr int kDefaultProbeEngines = 6;
constexpr int kMinProbeEngines = 1;
constexpr int kMaxProbeEngines = 8;

int ResolveConfiguredProbeEngines() noexcept {
    char buffer[32]{};
    const DWORD len = GetEnvironmentVariableA("UN_DMA_RAY_ENGINES", buffer,
        static_cast<DWORD>(sizeof(buffer)));
    if (len == 0 || len >= sizeof(buffer)) return kDefaultProbeEngines;
    char* end = nullptr;
    const long parsed = std::strtol(buffer, &end, 10);
    if (end == buffer || parsed <= 0) return kDefaultProbeEngines;
    return std::clamp(static_cast<int>(parsed), kMinProbeEngines, kMaxProbeEngines);
}

class VisionProbeBackend final : public EngineTrace::IBackend {
public:
    explicit VisionProbeBackend(EngineTrace::VisionProbe& probe) : probe_(probe) {}

    const char* Name() const override { return "unicorn-vision"; }

    bool BindScene(uint64_t sceneEpoch) override { return probe_.Start(sceneEpoch); }

    EngineTrace::Status Trace(const EngineTrace::Request& request,
                              EngineTrace::Result& result) override {
        result = probe_.ProbeOne(request);
        return result.status;
    }

    void TraceBatch(std::span<const EngineTrace::Request> requests,
                    std::span<EngineTrace::Result> results) override {
        const size_t count = (std::min)(requests.size(), results.size());
        if (count == 0) return;
        if (count == 1) {
            results[0] = probe_.ProbeOne(requests[0]);
            return;
        }
        std::vector<EngineTrace::Request> batch(requests.begin(), requests.begin() + count);
        const auto batchResults = probe_.Probe(batch);
        for (size_t i = 0; i < count && i < batchResults.size(); ++i) {
            results[i] = batchResults[i];
        }
    }

    void ReleaseScene() override { probe_.Stop(); }

private:
    EngineTrace::VisionProbe& probe_;
};

struct ProbeRuntime {
    ~ProbeRuntime() {
        stopping.store(true, std::memory_order_release);
        if (worker.joinable()) worker.join();
    }

    std::thread worker;
    std::mutex probeMutex;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};

    std::unique_ptr<EngineTrace::VisionProbe> probe;
    std::unique_ptr<VisionProbeBackend> backend;
    EngineTrace::Service service;
    bool attached = false;
    uint64_t attachedBase = 0;
    uint64_t attachedEntryRva = 0;
    offset::RuntimeProfile attachedProfile = offset::RuntimeProfile::Cn;
    uint64_t sceneEpoch = 0;
    uint64_t lastBindAttemptMs = 0;

    std::atomic<uint64_t> publishedEpoch{0};
    std::atomic<uint64_t> publishedContext{0};
    std::atomic<uint64_t> rebinds{0};
    std::atomic<int> configuredEngines{kDefaultProbeEngines};
};

ProbeRuntime& Runtime() {
    static ProbeRuntime runtime;
    return runtime;
}

uint64_t NowMs() { return GetTickCount64(); }

void AdvanceScene(const char* reason) {
    ProbeRuntime& runtime = Runtime();
    const uint64_t epoch = ++runtime.sceneEpoch;
    runtime.lastBindAttemptMs = NowMs();
    const uint64_t context = runtime.probe ? runtime.probe->Context() : 0;
    runtime.publishedEpoch.store(epoch, std::memory_order_release);
    runtime.publishedContext.store(context, std::memory_order_release);
    ++runtime.rebinds;
    Diagnostics::Info(
        "[RAY-PROBE] scene_epoch=%llu reason=%s previous_context=0x%llX entry_rva=0x%llX engines=%d",
        static_cast<unsigned long long>(epoch), reason,
        static_cast<unsigned long long>(context),
        static_cast<unsigned long long>(offset::Active().rayEntryRva),
        runtime.configuredEngines.load(std::memory_order_acquire));
    Broker().SetScene(epoch);
}

void DetachProbe() {
    ProbeRuntime& runtime = Runtime();
    if (!runtime.attached) {
        runtime.publishedEpoch.store(0, std::memory_order_release);
        runtime.publishedContext.store(0, std::memory_order_release);
        return;
    }
    runtime.publishedEpoch.store(0, std::memory_order_release);
    runtime.publishedContext.store(0, std::memory_order_release);
    // Broker first: it stops accepting requests and drains the one in flight, so the
    // probe cannot be destroyed under a running Trace.
    RayRuntime::Detach();
    runtime.service.SetBackend(nullptr);  // ReleaseScene -> probe.Stop()
    runtime.attached = false;
    runtime.backend.reset();
    runtime.probe.reset();
    runtime.attachedBase = 0;
    runtime.attachedEntryRva = 0;
    Broker().SetScene(0);
    Diagnostics::Warn("[RAY-PROBE] detached; live world occlusion is unavailable");
}

bool AttachProbe() {
    ProbeRuntime& runtime = Runtime();
    const auto& offsets = offset::Active();
    if (!offsets.rayEntryRva) return false;

    uint64_t context = 0;
    if (!TryGetRayContext(context)) return false;

    // Static scene chain: context + 0x220 -> sub60 -> setup + 0x1E8. This replaces the
    // one-time stack seed; +0x1E8 is the only game object the synthetic setup needs.
    RayScene::Chain chain;
    const auto decode = offsets.raySceneDecode == offset::RaySceneDecode::World155284
        ? RayScene::Decode::World155284
        : RayScene::Decode::CnNexon;
    const auto read = [](uint64_t address, void* buffer, size_t size) {
        return mem.Read(address, buffer, size);
    };
    if (!RayScene::Resolve(context, SDK->dwGameBase, decode,
            offsets.raySceneSub60VtableRva, read, chain))
        return false;

    EngineTrace::VisionProbe::Config config;
    config.processName = "Overwatch.exe";
    config.entryRva = offsets.rayEntryRva;
    config.mask = offsets.rayMask;
    config.context = context;  // re-resolved by Start() if this copy goes stale
    config.filterObject = chain.setup1e8;
    // One filter entry excludes the local player's own body; without it a camera-origin
    // ray can hit self at fraction 0. Refresh the parent per trace so a hero swap does not
    // leave a stale entity pointer behind.
    const auto local = OW::SnapshotLocalEntity();
    config.filterCount = local.address ? 1 : 0;
    config.filterItems[0] = local.address;
    config.filterItemResolver = [] {
        const auto current = OW::SnapshotLocalEntity();
        return current.address;
    };
    // +0x1F8 is a writable scratch pointer, not a game object; the backend substitutes a
    // zeroed workspace page. +0x40 stays zero.
    config.sceneObject2 = 0;
    config.localObject = 0;
    config.auxCallback = 0;
    const int engines = ResolveConfiguredProbeEngines();
    runtime.configuredEngines.store(engines, std::memory_order_release);
    config.engines = engines;
    // Normal Overwatch world raycasts finish in ~2k-20k instructions (~1-3 ms). Cap at 250k
    // so a degenerate ray or stale cached heap page aborts in ~15-25 ms instead of stalling
    // all 6 engines for ~5 seconds and turning every Aim/Trigger ray Stale.
    config.instructionLimit = 250'000;
    config.timeoutUs = 0;  // instructionLimit is the only bound; no 15 ms timer floor
    config.refreshTouchedPages = false;  // static world pages stay cached for the scene
    config.skipCodeHook = true;  // native instructionLimit + unmapped-fetch hook remain active
    config.resolveContext = [](uint64_t& resolved) { return TryGetRayContext(resolved); };

    auto probe = std::make_unique<EngineTrace::VisionProbe>(
        config,
        [](uint64_t address, void* buffer, size_t size) { return mem.Read(address, buffer, size); },
        SDK->dwGameBase, offsets.imageSize, 0);
    auto backend = std::make_unique<VisionProbeBackend>(*probe);

    runtime.probe = std::move(probe);
    runtime.backend = std::move(backend);
    runtime.service.SetBackend(runtime.backend.get());
    RayRuntime::Attach(&runtime.service);
    runtime.attached = true;
    runtime.attachedBase = SDK->dwGameBase;
    runtime.attachedEntryRva = offsets.rayEntryRva;
    runtime.attachedProfile = offset::ActiveProfile();
    runtime.publishedContext.store(context, std::memory_order_release);
    Diagnostics::Info(
        "[RAY-PROBE] attached pid=%d base=0x%llX image=0x%X profile=%s entry_rva=0x%llX mask=0x%llX world_mask=0x%llX skip_code_hook=%d context=0x%llX chain_raw220=0x%llX chain_sub60=0x%llX chain_1e8=0x%llX setup_1f8=scratch setup_1a0=0",
        static_cast<int>(ProcessConnection::ConnectedPid()),
        static_cast<unsigned long long>(SDK->dwGameBase),
        static_cast<unsigned int>(offsets.imageSize), offsets.name,
        static_cast<unsigned long long>(offsets.rayEntryRva),
        static_cast<unsigned long long>(offsets.rayMask),
        static_cast<unsigned long long>(RayRuntime::WorldRayMask()),
        config.skipCodeHook ? 1 : 0,
        static_cast<unsigned long long>(context),
        static_cast<unsigned long long>(chain.raw220),
        static_cast<unsigned long long>(chain.sub60),
        static_cast<unsigned long long>(chain.setup1e8));
    AdvanceScene("attach");
    return true;
}

void WatchThread() {
    ProbeRuntime& runtime = Runtime();
    bool attachFailureLogged = false;
    while (!runtime.stopping.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(runtime.probeMutex);
            const bool connected =
                ProcessConnection::IsConnected() && SDK->dwGameBase != 0 && SDK->IsInitialized();
            const auto& offsets = offset::Active();
            if (!connected || !offsets.rayEntryRva) {
                if (runtime.attached) DetachProbe();
                attachFailureLogged = false;
            } else if (runtime.attached &&
                       (runtime.attachedBase != SDK->dwGameBase ||
                        runtime.attachedEntryRva != offsets.rayEntryRva ||
                        runtime.attachedProfile != offset::ActiveProfile())) {
                Diagnostics::Info("[RAY-PROBE] process image/profile changed; rebuilding probe");
                DetachProbe();
            } else if (!runtime.attached) {
                if (!AttachProbe() && !attachFailureLogged) {
                    Diagnostics::Warn(
                        "[RAY-PROBE] context or live setup seed not available yet (profile=%s); retrying",
                        offsets.name);
                    attachFailureLogged = true;
                }
            } else {
                attachFailureLogged = false;
                const bool started = runtime.probe && runtime.probe->Started();
                const uint64_t boundContext = runtime.probe ? runtime.probe->Context() : 0;
                const bool healthy = started && runtime.probe->Healthy();
                if (!started) {
                    if (NowMs() - runtime.lastBindAttemptMs >= kBindRetryMs)
                        AdvanceScene("bind_retry");
                } else {
                    uint64_t resolved = 0;
                    const bool resolvedOk = TryGetRayContext(resolved);
                    const uint32_t errWaves = Broker().ConsecutiveErrorWaves();
                    const uint32_t lastWaveUs = Broker().LastWaveLatencyUs();
                    const bool probeDegraded = errWaves >= 8;
                    if (lastWaveUs >= 150'000) {
                        static uint64_t lastSlowLogMs = 0;
                        const uint64_t nowMs = NowMs();
                        if (lastSlowLogMs == 0 || nowMs - lastSlowLogMs >= 2000) {
                            lastSlowLogMs = nowMs;
                            Diagnostics::Warn(
                                "[RAY-PROBE] slow wave detected lastWaveMs=%.1f errWaves=%u",
                                static_cast<double>(lastWaveUs) / 1000.0, errWaves);
                        }
                    }
                    if (!resolvedOk || !healthy || resolved != boundContext || probeDegraded) {
                        // The level instance changed, died, or cached dynamic heap pages went
                        // stale causing repeated trace errors. Drop the engines and page cache
                        // so the next tick re-resolves fresh memory cleanly.
                        Diagnostics::Info(
                            "[RAY-PROBE] scene change detected (%s, errWaves=%u, lastWaveMs=%.1f); releasing engines and page cache",
                            !resolvedOk ? "context_unresolved"
                                        : (!healthy ? "context_dead"
                                                    : (probeDegraded ? "stale_page_errors" : "context_changed")),
                            errWaves, static_cast<double>(lastWaveUs) / 1000.0);
                        DetachProbe();
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kWatchIntervalMs));
    }
    std::lock_guard<std::mutex> lock(runtime.probeMutex);
    DetachProbe();
}

}  // namespace

void StartProbe() {
    ProbeRuntime& runtime = Runtime();
    if (runtime.running.exchange(true, std::memory_order_acq_rel)) return;
    runtime.stopping.store(false, std::memory_order_release);
    runtime.sceneEpoch = 0;
    runtime.lastBindAttemptMs = 0;
    runtime.publishedEpoch.store(0, std::memory_order_release);
    runtime.publishedContext.store(0, std::memory_order_release);
    const int engines = ResolveConfiguredProbeEngines();
    runtime.configuredEngines.store(engines, std::memory_order_release);
    runtime.worker = std::thread(WatchThread);
    Diagnostics::Info("[RAY-PROBE] owner started (interval_ms=%lu engines=%d)",
        static_cast<unsigned long>(kWatchIntervalMs), engines);
}

void StopProbe() {
    ProbeRuntime& runtime = Runtime();
    if (!runtime.running.exchange(false, std::memory_order_acq_rel)) return;
    runtime.stopping.store(true, std::memory_order_release);
    if (runtime.worker.joinable()) runtime.worker.join();
    {
        std::lock_guard<std::mutex> lock(runtime.probeMutex);
        DetachProbe();
        runtime.sceneEpoch = 0;
    }
    Diagnostics::Info("[RAY-PROBE] owner stopped");
}

void DetachProbeForDisconnect() {
    ProbeRuntime& runtime = Runtime();
    std::lock_guard<std::mutex> lock(runtime.probeMutex);
    if (runtime.attached) {
        Diagnostics::Info("[RAY-PROBE] synchronous detach requested by process disconnect/drain");
        DetachProbe();
    }
}

bool ProbeRunning() noexcept { return Runtime().running.load(std::memory_order_acquire); }

uint64_t SceneEpoch() noexcept {
    return Runtime().publishedEpoch.load(std::memory_order_acquire);
}

int ProbeEngineCount() noexcept {
    return Runtime().configuredEngines.load(std::memory_order_acquire);
}

}  // namespace OW::RayRuntime

