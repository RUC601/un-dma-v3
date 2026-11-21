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
            return false;
        }
        engines_.push_back(std::move(backend));
    }
    // Every engine reads the same template, so the context is identical across them.
    context_ = engines_.empty() ? 0 : engines_.front()->Context();
    started_ = true;
    return true;
}

void VisionProbe::Stop() {
    engines_.clear();
    store_.reset();
    started_ = false;
    context_ = 0;
}

bool VisionProbe::Healthy() const {
    if (!started_ || !reader_ || !context_) return false;
    uint64_t head = 0;
    if (!reader_(context_, &head, sizeof(head))) return false;
    return head >= base_ && head < base_ + imageSize_;
}

std::vector<Result> VisionProbe::Probe(const std::vector<Request>& requests) {
    std::vector<Result> results(requests.size());
    if (!started_ || engines_.empty() || requests.empty()) return results;

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
    const auto& last = engines_.front()->GetStats();
    stats_ = last;
    return results;
}

}  // namespace OW::EngineTrace

