#include "Game/EngineTrace/VisionProbe.hpp"

#include <algorithm>
#include <cstdio>
#include <thread>

namespace OW::EngineTrace {

VisionProbe::VisionProbe(Config config, UnicornBackend::Reader reader, uint64_t base,
                         uint32_t imageSize, uint64_t teb)
    : config_(std::move(config)), reader_(std::move(reader)), base_(base), imageSize_(imageSize),
      teb_(teb) {}

VisionProbe::~VisionProbe() { Stop(); }

bool VisionProbe::Start(uint64_t sceneEpoch) {
    Stop();
    if (!reader_ || !base_ || !imageSize_) {
        std::fprintf(stderr, "VisionProbe: reader/base/imageSize are required\n");
        return false;
    }
    // A resolver, when present, is authoritative: a rebind after a level change must use
    // the context that is alive now, not the copy captured for the previous scene.
    if (config_.resolveContext) {
        uint64_t resolved = 0;
        if (!config_.resolveContext(resolved) || !resolved) {
            std::fprintf(stderr, "VisionProbe: context resolver failed; scene not bound\n");
            return false;
        }
        config_.context = resolved;
    }
    if (!config_.context) {
        std::fprintf(stderr, "VisionProbe: no context for the scene\n");
        return false;
    }

    const int count = (std::max)(1, config_.engines);
    store_ = std::make_shared<PageStore>();
    engines_.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        UnicornConfig config;
        config.processName = config_.processName;
        config.entryRva = config_.entryRva;
        config.setupTemplatePath = config_.setupTemplatePath;
        config.mask = config_.mask;
        config.context = config_.context;
        config.context2 = config_.context2;
        config.filterObject = config_.filterObject;
        config.auxCallback = config_.auxCallback;
        config.sceneObject2 = config_.sceneObject2;
        config.filterCount = config_.filterCount;
        for (uint32_t i = 0; i < 4; ++i) config.filterItems[i] = config_.filterItems[i];
        config.filterItemResolver = config_.filterItemResolver;
        config.localObject = config_.localObject;
        config.flags = config_.flags;
        config.instructionLimit = config_.instructionLimit;
        config.timeoutUs = config_.timeoutUs;
        config.refreshTouchedPages = config_.refreshTouchedPages;

        auto backend = std::make_unique<UnicornBackend>(config);
        backend->SetPageStore(store_);
        backend->SetReader(reader_);
        backend->SetTarget(base_, imageSize_, teb_);
        if (!backend->BindScene(sceneEpoch)) {
            std::fprintf(stderr, "VisionProbe: engine %d failed to bind the scene\n", i);
            engines_.clear();
            store_.reset();
            return false;
        }
        engines_.push_back(std::move(backend));
    }
    // Every engine reads the same synthetic setup, so the context is identical across them.
    context_.store(engines_.empty() ? 0 : engines_.front()->Context(), std::memory_order_release);
    started_.store(true, std::memory_order_release);
    return true;
}

void VisionProbe::Stop() {
    started_.store(false, std::memory_order_release);
    engines_.clear();
    store_.reset();
    context_.store(0, std::memory_order_release);
}

bool VisionProbe::Healthy() const {
    const uint64_t context = context_.load(std::memory_order_acquire);
    if (!started_.load(std::memory_order_acquire) || !reader_ || !context) return false;
    uint64_t head = 0;
    if (!reader_(context, &head, sizeof(head))) return false;
    return head >= base_ && head < base_ + imageSize_;
}

VisionProbe::SceneState VisionProbe::CheckScene() const {
    if (!started_.load(std::memory_order_acquire)) return SceneState::Unavailable;
    if (!Healthy()) return SceneState::Changed;
    if (!config_.resolveContext) return SceneState::Healthy;
    // The product can resolve the level context at any time. A different address means a
    // new level object was written to ClientGame + 0x30 (map/match change), so the old
    // engines and their world pages must not answer for the new scene.
    uint64_t resolved = 0;
    if (!config_.resolveContext(resolved) || !resolved) return SceneState::Changed;
    return resolved == context_.load(std::memory_order_acquire) ? SceneState::Healthy
                                                               : SceneState::Changed;
}

std::vector<Result> VisionProbe::Probe(const std::vector<Request>& requests) {
    std::vector<Result> results(requests.size());
    if (!started_.load(std::memory_order_acquire) || engines_.empty() || requests.empty()) {
        return results;
    }

    // One request per engine per wave; waves run until the batch is covered.
    size_t index = 0;
    while (index < requests.size()) {
        const size_t waveCount = (std::min)(engines_.size(), requests.size() - index);
        std::vector<std::thread> workers;
        workers.reserve(waveCount);
        for (size_t i = 0; i < waveCount; ++i) {
            UnicornBackend* engine = engines_[i].get();
            const Request* request = &requests[index + i];
            Result* result = &results[index + i];
            workers.emplace_back([engine, request, result]() { engine->Trace(*request, *result); });
        }
        for (auto& worker : workers) worker.join();
        index += waveCount;
    }
    stats_ = engines_.front()->GetStats();
    return results;
}

Result VisionProbe::ProbeOne(const Request& request) {
    if (!started_.load(std::memory_order_acquire) || engines_.empty()) return Result{};
    Result result;
    engines_.front()->Trace(request, result);
    stats_ = engines_.front()->GetStats();
    return result;
}

}  // namespace OW::EngineTrace
