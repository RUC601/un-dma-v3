#pragma once

#include "Game/EngineTrace.hpp"
#include "Game/EngineTrace/UnicornBackend.hpp"

#include <cstdint>
#include <memory>
#include <string>
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
//   Healthy()      - one-qword context check, ~0.3 ms
//   Stop()         - releases the engines and their pages
class VisionProbe {
public:
    struct Config {
        std::string processName = "Overwatch.exe";
        // World 155284 wrapper; CN 155284N uses 0x10222E0.
        uint64_t entryRva = 0xD0E030;
        // 0x240-byte setup capture; the context is taken from it unless context is set.
        std::string setupTemplatePath;
        uint64_t mask = 0x0690000000000017ull;
        uint64_t context = 0;
        uint64_t context2 = 0;
        uint64_t flags = 0;
        // Engines to create. One ray per engine in a batch; 6 covers a full body sweep.
        int engines = 6;
        uint64_t instructionLimit = 4'000'000;
        uint64_t timeoutUs = 6'000'000;
        bool refreshTouchedPages = false;
    };

    VisionProbe(Config config, UnicornBackend::Reader reader, uint64_t base, uint32_t imageSize,
                uint64_t teb = 0);
    ~VisionProbe();

    VisionProbe(const VisionProbe&) = delete;
    VisionProbe& operator=(const VisionProbe&) = delete;

    // Creates the engines and binds the scene. Fails when the context is missing or dead.
    bool Start(uint64_t sceneEpoch);
    void Stop();
    bool Started() const { return started_; }

    // One-qword liveness check for the bound context.
    bool Healthy() const;

    // Runs the batch concurrently, one request per engine. Returns one result per request.
    std::vector<Result> Probe(const std::vector<Request>& requests);

    const UnicornBackend::Stats& Stats() const { return stats_; }
    uint64_t Context() const { return context_; }

private:
    Config config_;
    UnicornBackend::Reader reader_;
    uint64_t base_ = 0;
    uint32_t imageSize_ = 0;
    uint64_t teb_ = 0;
    std::shared_ptr<PageStore> store_;
    std::vector<std::unique_ptr<UnicornBackend>> engines_;
    uint64_t context_ = 0;
    bool started_ = false;
    UnicornBackend::Stats stats_{};
};

}  // namespace OW::EngineTrace
