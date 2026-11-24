#include "Game/RayQueryBroker.hpp"

#include <chrono>
#include <cmath>
#include <utility>

namespace OW::EngineTrace {
namespace {

bool VecWithinDrift(const Vec3& lhs, const Vec3& rhs, float maxDriftMeters) {
    if (!std::isfinite(lhs.x) || !std::isfinite(lhs.y) || !std::isfinite(lhs.z) ||
        !std::isfinite(rhs.x) || !std::isfinite(rhs.y) || !std::isfinite(rhs.z) ||
        !std::isfinite(maxDriftMeters) || maxDriftMeters < 0.0f) {
        return false;
    }
    const float dx = lhs.x - rhs.x;
    const float dy = lhs.y - rhs.y;
    const float dz = lhs.z - rhs.z;
    return (dx * dx + dy * dy + dz * dz) <= (maxDriftMeters * maxDriftMeters);
}

bool SameQuerySlot(const RayQueryBroker::Query& lhs, const RayQueryBroker::Query& rhs) {
    return lhs.entityKey == rhs.entityKey &&
           lhs.actorGeneration == rhs.actorGeneration &&
           lhs.boneId == rhs.boneId &&
           lhs.request.mask == rhs.request.mask;
}

}  // namespace

RayQueryBroker::~RayQueryBroker() { Stop(); }

void RayQueryBroker::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    stopping_ = false;
    running_ = true;
    if (service_ != nullptr && desiredSceneEpoch_ != 0 && boundSceneEpoch_ != desiredSceneEpoch_) {
        sceneDirty_ = true;
    }
    worker_ = std::thread([this] { Run(); });
}

void RayQueryBroker::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        stopping_ = true;
    }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    stopping_ = false;
    inFlight_ = false;
    boundSceneEpoch_ = 0;
    sceneDirty_ = false;
    pendingQueue_.clear();
    cache_.clear();
    idleCv_.notify_all();
}

void RayQueryBroker::Attach(Service* service) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        idleCv_.wait(lock, [&] { return !inFlight_; });
        ++serviceGeneration_;
        service_ = service;
        boundSceneEpoch_ = 0;
        sceneDirty_ = (service_ != nullptr && desiredSceneEpoch_ != 0);
        pendingQueue_.clear();
        cache_.clear();
    }
    wake_.notify_one();
}

void RayQueryBroker::Detach() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++serviceGeneration_;
    service_ = nullptr;
    boundSceneEpoch_ = 0;
    sceneDirty_ = false;
    pendingQueue_.clear();
    cache_.clear();
    idleCv_.wait(lock, [&] { return !inFlight_; });
}

void RayQueryBroker::SetScene(uint64_t sceneEpoch) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (desiredSceneEpoch_ == sceneEpoch) return;
        desiredSceneEpoch_ = sceneEpoch;
        boundSceneEpoch_ = 0;
        pendingQueue_.clear();
        cache_.clear();
        sceneDirty_ = (service_ != nullptr && sceneEpoch != 0);
        notify = running_ && sceneDirty_;
    }
    if (notify) wake_.notify_one();
}

void RayQueryBroker::Submit(Query query) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || query.sceneEpoch == 0 || query.sceneEpoch != desiredSceneEpoch_) return;
        for (auto& existing : pendingQueue_) {
            if (SameQuerySlot(existing, query)) {
                existing = std::move(query);
                wake_.notify_one();
                return;
            }
        }
        if (pendingQueue_.size() >= kMaxPendingEntries) {
            pendingQueue_.erase(pendingQueue_.begin());
        }
        pendingQueue_.push_back(std::move(query));
    }
    wake_.notify_one();
}

std::optional<RayDecision> RayQueryBroker::Latest(
    const Request& request, uint64_t frame, uint64_t connectionEpoch,
    uint64_t sceneEpoch, uint64_t entityKey, uint64_t actorGeneration,
    int boneId, uint32_t maxAgeMs, float maxEndpointDriftMeters) const {
    if (sceneEpoch == 0) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : cache_) {
        if (entry.decision.frame > frame ||
            entry.decision.connectionEpoch != connectionEpoch ||
            entry.decision.sceneEpoch != sceneEpoch ||
            entry.decision.entityKey != entityKey ||
            entry.decision.actorGeneration != actorGeneration ||
            entry.boneId != boneId ||
            entry.request.mask != request.mask) {
            continue;
        }
        if (!VecWithinDrift(entry.request.start, request.start, maxEndpointDriftMeters) ||
            !VecWithinDrift(entry.request.end, request.end, maxEndpointDriftMeters)) {
            return std::nullopt;
        }
        RayDecision current = entry.decision;
        if (current.completedAtUs != 0) {
            const uint64_t nowUs = RayDecisionNowUs();
            current.ageMs = nowUs >= current.completedAtUs
                ? static_cast<uint32_t>((nowUs - current.completedAtUs) / 1000u)
                : 0u;
        }
        if (current.ageMs > maxAgeMs) {
            current.world = RayDecisionStatus::Stale;
        }
        return current;
    }
    return std::nullopt;
}

void RayQueryBroker::StoreResultLocked(const CacheEntry& entry) {
    for (auto& existing : cache_) {
        if (existing.decision.entityKey == entry.decision.entityKey &&
            existing.decision.actorGeneration == entry.decision.actorGeneration &&
            existing.boneId == entry.boneId &&
            existing.request.mask == entry.request.mask) {
            existing = entry;
            return;
        }
    }
    if (cache_.size() >= kMaxCacheEntries) {
        size_t oldestIndex = 0;
        for (size_t i = 1; i < cache_.size(); ++i) {
            if (cache_[i].decision.completedAtUs < cache_[oldestIndex].decision.completedAtUs) {
                oldestIndex = i;
            }
        }
        cache_[oldestIndex] = entry;
        return;
    }
    cache_.push_back(entry);
}

void RayQueryBroker::Run() {
    for (;;) {
        Service* service = nullptr;
        uint64_t serviceGen = 0;
        uint64_t sceneToBind = 0;
        bool needBind = false;
        std::optional<Query> query;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] {
                return stopping_ || (sceneDirty_ && service_ != nullptr) || !pendingQueue_.empty();
            });
            if (stopping_) return;

            service = service_;
            serviceGen = serviceGeneration_;
            if (sceneDirty_ && service != nullptr) {
                needBind = true;
                sceneToBind = desiredSceneEpoch_;
                sceneDirty_ = false;
                inFlight_ = true;
            } else if (!pendingQueue_.empty()) {
                query = std::move(pendingQueue_.front());
                pendingQueue_.erase(pendingQueue_.begin());
                if (service != nullptr) {
                    inFlight_ = true;
                    if (query->sceneEpoch != 0 &&
                        query->sceneEpoch == desiredSceneEpoch_ &&
                        boundSceneEpoch_ != desiredSceneEpoch_) {
                        needBind = true;
                        sceneToBind = desiredSceneEpoch_;
                    }
                }
            }
        }

        if (needBind && service != nullptr) {
            const bool boundOk = service->OnSceneChanged(sceneToBind);
            std::lock_guard<std::mutex> lock(mutex_);
            if (serviceGen == serviceGeneration_ && sceneToBind == desiredSceneEpoch_) {
                boundSceneEpoch_ = boundOk ? sceneToBind : 0;
            }
            if (!query.has_value()) {
                inFlight_ = false;
                idleCv_.notify_all();
                continue;
            }
            if (stopping_ || serviceGen != serviceGeneration_ ||
                query->sceneEpoch != desiredSceneEpoch_) {
                inFlight_ = false;
                idleCv_.notify_all();
                continue;
            }
        }

        if (!query.has_value()) continue;

        RayDecision decision{};
        decision.frame = query->frame;
        decision.connectionEpoch = query->connectionEpoch;
        decision.sceneEpoch = query->sceneEpoch;
        decision.entityKey = query->entityKey;
        decision.actorGeneration = query->actorGeneration;
        decision.sampleTick = query->sampleTick;
        decision.world = RayDecisionStatus::Unavailable;

        const uint64_t startedUs = RayDecisionNowUs();
        if (service != nullptr) {
            const Result result = service->Trace(query->request);
            switch (result.status) {
                case Status::Ok:
                    decision.world = (std::isfinite(result.fraction) &&
                                      result.fraction >= 0.0f && result.fraction <= 1.0f)
                        ? (result.fraction < 1.0f ? RayDecisionStatus::Occluded
                                                  : RayDecisionStatus::Clear)
                        : RayDecisionStatus::Invalid;
                    break;
                case Status::Timeout: decision.world = RayDecisionStatus::Timeout; break;
                case Status::InvalidScene: decision.world = RayDecisionStatus::Stale; break;
                case Status::Unavailable: decision.world = RayDecisionStatus::Unavailable; break;
                case Status::Error: decision.world = RayDecisionStatus::Invalid; break;
            }
        }
        const uint64_t completedUs = RayDecisionNowUs();
        decision.latencyUs = completedUs >= startedUs
            ? static_cast<uint32_t>(completedUs - startedUs)
            : 0u;
        decision.ageMs = 0;
        decision.completedAtUs = completedUs;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (serviceGen == serviceGeneration_ &&
                query->sceneEpoch != 0 &&
                query->sceneEpoch == desiredSceneEpoch_) {
                StoreResultLocked(CacheEntry{decision, query->request, query->boneId});
            }
            if (inFlight_) {
                inFlight_ = false;
                idleCv_.notify_all();
            }
        }
    }
}

}  // namespace OW::EngineTrace
