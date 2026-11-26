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
    StartWorkers();
    started_.store(true, std::memory_order_release);
    return true;
}

void VisionProbe::Stop() {
    started_.store(false, std::memory_order_release);
    StopWorkers();
    engines_.clear();
    store_.reset();
    context_.store(0, std::memory_order_release);
}

void VisionProbe::StartWorkers() {
    StopWorkers();
    if (engines_.size() <= 1) return;
    {
        std::lock_guard<std::mutex> lock(waveMutex_);
        workersStopping_ = false;
        waveGeneration_ = 0;
        activeWaveCount_ = 0;
        pendingWorkers_ = 0;
        waveRequests_ = nullptr;
        waveResults_ = nullptr;
    }
    workers_.reserve(engines_.size() - 1);
    for (size_t i = 1; i < engines_.size(); ++i) {
        workers_.emplace_back([this, i]() { WorkerLoop(i); });
    }
}

void VisionProbe::StopWorkers() {
    {
        std::lock_guard<std::mutex> lock(waveMutex_);
        workersStopping_ = true;
    }
    waveStartCv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
    {
        std::lock_guard<std::mutex> lock(waveMutex_);
        workersStopping_ = false;
        activeWaveCount_ = 0;
        pendingWorkers_ = 0;
        waveRequests_ = nullptr;
        waveResults_ = nullptr;
    }
}

void VisionProbe::WorkerLoop(size_t engineIndex) {
    uint64_t seenGeneration = 0;
    for (;;) {
        const Request* req = nullptr;
        Result* res = nullptr;
        {
            std::unique_lock<std::mutex> lock(waveMutex_);
            waveStartCv_.wait(lock, [&]() {
                return workersStopping_ || waveGeneration_ != seenGeneration;
            });
            if (workersStopping_) return;
            seenGeneration = waveGeneration_;
            if (engineIndex < activeWaveCount_) {
                req = waveRequests_ + engineIndex;
                res = waveResults_ + engineIndex;
            }
        }
        if (req && res) {
            engines_[engineIndex]->Trace(*req, *res);
            std::lock_guard<std::mutex> lock(waveMutex_);
            if (pendingWorkers_ > 0 && --pendingWorkers_ == 0) {
                waveDoneCv_.notify_one();
            }
        }
    }
}

void VisionProbe::RefreshAggregatedStats() {
    UnicornBackend::Stats total{};
    for (const auto& engine : engines_) {
        const auto& s = engine->GetStats();
        total.traces += s.traces;
        total.faults += s.faults;
        total.mappedPages += s.mappedPages;
        total.pageFetches += s.pageFetches;
        total.cacheHits += s.cacheHits;
        total.pageReadFailures += s.pageReadFailures;
        total.pageUnavailableTraces += s.pageUnavailableTraces;
        total.pageReadRetries += s.pageReadRetries;
        total.instructions += s.instructions;
        if (s.lastFailedPage != 0) total.lastFailedPage = s.lastFailedPage;
        if (s.maxUs > total.maxUs) total.maxUs = s.maxUs;
        if (s.lastUs > total.lastUs) total.lastUs = s.lastUs;
    }
    stats_ = total;
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

    // One request per engine per wave; engine 0 runs on the caller thread while persistent
    // worker threads execute engines_[1..waveCount-1] concurrently.
    size_t index = 0;
    while (index < requests.size()) {
        const size_t waveCount = (std::min)(engines_.size(), requests.size() - index);
        if (waveCount == 1) {
            engines_[0]->Trace(requests[index], results[index]);
        } else {
            {
                std::lock_guard<std::mutex> lock(waveMutex_);
                waveRequests_ = &requests[index];
                waveResults_ = &results[index];
                activeWaveCount_ = waveCount;
                pendingWorkers_ = waveCount - 1;
                ++waveGeneration_;
            }
            waveStartCv_.notify_all();
            engines_[0]->Trace(requests[index], results[index]);
            {
                std::unique_lock<std::mutex> lock(waveMutex_);
                waveDoneCv_.wait(lock, [&]() { return pendingWorkers_ == 0; });
                activeWaveCount_ = 0;
                waveRequests_ = nullptr;
                waveResults_ = nullptr;
            }
        }
        index += waveCount;
    }
    RefreshAggregatedStats();
    return results;
}

Result VisionProbe::ProbeOne(const Request& request) {
    if (!started_.load(std::memory_order_acquire) || engines_.empty()) return Result{};
    Result result;
    engines_.front()->Trace(request, result);
    RefreshAggregatedStats();
    return result;
}

}  // namespace OW::EngineTrace
