#pragma once

#include "Game/EngineTrace.hpp"
#include "Game/EngineTrace/UnicornBackend.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace OW::EngineTrace {

// Product-facing owner of the visibility engines.
//
// One instance per level: it creates N Unicorn engines that share the host's page store
// (one DMA read serves every engine) and the host's reader callback. Requests are spread
// across the engines and run concurrently; results keep the caller's order.
//
// Lifecycle:
//   Start()        - builds the engines and binds the scene (validates the context)
//   Probe(...)     - one batch; safe to call from a single scheduler thread
//   CheckScene()   - resolves the product context again and compares it with the bound
//                    one; a changed or dead context means the level changed
//   Stop()         - releases the engines and their pages
class VisionProbe {
public:
    struct Config {
        std::string processName = "Overwatch.exe";
        // World 155284 wrapper; CN 155284N uses 0x10222E0; Nexon 155284KB uses 0x100C5E0.
        uint64_t entryRva = 0xD0E030;
        // 0x240-byte setup capture; empty means the backend builds the synthetic layout.
        std::string setupTemplatePath;
        // The game's player/world trace layer mask (see UnicornConfig::mask).
        uint64_t mask = 0x0690000000000017ull;
        uint64_t context = 0;
        uint64_t context2 = 0;
        // setup + 0x1E8 level/world object; see UnicornConfig::filterObject.
        uint64_t filterObject = 0;
        // setup + 0x1A0 world-trace callback (build constant).
        uint64_t auxCallback = 0;
        // setup + 0x1F8 second level object.
        uint64_t sceneObject2 = 0;
        // setup + 0x68 / + 0x70 game filter entries (self/team exclusions).
        uint32_t filterCount = 0;
        uint64_t filterItems[4] = {};
        std::function<uint64_t()> filterItemResolver;
        // setup + 0x40 per-call local ray object.
        uint64_t localObject = 0;
        uint64_t flags = 0;
        // Engines to create. One ray per engine in a batch.
        int engines = 1;
        uint64_t instructionLimit = 4'000'000;
        uint64_t timeoutUs = 0;
        bool refreshTouchedPages = false;
        // Product seam: resolves the level context that `setup + 0x1F0` must hold
        // (Game/Decrypt.hpp TryGetRayContext in the product). Start() uses it when
        // `context` is 0, and CheckScene() uses it to detect a map/match change.
        std::function<bool(uint64_t&)> resolveContext;
    };

    enum class SceneState : uint8_t {
        Healthy = 0,   // bound context is alive and unchanged
        Changed,       // context died, was replaced, or can no longer be resolved
        Unavailable,   // no engines are bound
    };

    VisionProbe(Config config, UnicornBackend::Reader reader, uint64_t base, uint32_t imageSize,
                uint64_t teb = 0);
    ~VisionProbe();

    VisionProbe(const VisionProbe&) = delete;
    VisionProbe& operator=(const VisionProbe&) = delete;

    // Creates the engines and binds the scene. Fails when the context is missing or dead.
    bool Start(uint64_t sceneEpoch);
    void Stop();
    bool Started() const { return started_.load(std::memory_order_acquire); }

    // One-qword liveness check for the bound context.
    bool Healthy() const;

    // Re-resolves the product context and compares it with the bound one. This is the
    // level-change signal: a changed address (or a dead vtable) means the cached engines
    // and their world pages belong to an old level and must be released.
    SceneState CheckScene() const;

    // Runs the batch concurrently, one request per engine. Returns one result per request.
    std::vector<Result> Probe(const std::vector<Request>& requests);
    // Single-request fast path; uses only the first engine.
    Result ProbeOne(const Request& request);

    const UnicornBackend::Stats& Stats() const { return stats_; }
    uint64_t Context() const { return context_.load(std::memory_order_acquire); }
    int EngineCount() const noexcept { return static_cast<int>(engines_.size()); }

private:
    void StartWorkers();
    void StopWorkers();
    void WorkerLoop(size_t engineIndex);
    void RefreshAggregatedStats();

    Config config_;
    UnicornBackend::Reader reader_;
    uint64_t base_ = 0;
    uint32_t imageSize_ = 0;
    uint64_t teb_ = 0;
    std::shared_ptr<PageStore> store_;
    std::vector<std::unique_ptr<UnicornBackend>> engines_;
    // Persistent pool for engines_[1..N-1]; engine 0 runs on the caller's thread.
    std::vector<std::thread> workers_;
    std::mutex waveMutex_;
    std::condition_variable waveStartCv_;
    std::condition_variable waveDoneCv_;
    bool workersStopping_ = false;
    uint64_t waveGeneration_ = 0;
    size_t activeWaveCount_ = 0;
    size_t pendingWorkers_ = 0;
    const Request* waveRequests_ = nullptr;
    Result* waveResults_ = nullptr;
    // Read from the runtime's scene watcher while the broker thread owns Start/Stop.
    std::atomic<uint64_t> context_{0};
    std::atomic<bool> started_{false};
    UnicornBackend::Stats stats_{};
};

}  // namespace OW::EngineTrace
