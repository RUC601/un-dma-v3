#include "Game/RayQueryBroker.hpp"

#include <algorithm>
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
           lhs.segmentKind == rhs.segmentKind &&
           lhs.segmentIndex == rhs.segmentIndex &&
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
        hasService_.store(service != nullptr, std::memory_order_release);
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
    hasService_.store(false, std::memory_order_release);
    boundSceneEpoch_ = 0;
    sceneDirty_ = false;
    pendingQueue_.clear();
    cache_.clear();
    idleCv_.wait(lock, [&] { return !inFlight_; });
}

bool RayQueryBroker::HasService() const noexcept {
    return hasService_.load(std::memory_order_acquire);
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

void RayQueryBroker::EnqueueLocked(Query query) {
    for (auto& existing : pendingQueue_) {
        if (SameQuerySlot(existing, query)) {
            existing = std::move(query);
            return;
        }
    }
    if (pendingQueue_.size() >= kMaxPendingEntries) {
        pendingQueue_.erase(pendingQueue_.begin());
    }
    pendingQueue_.push_back(std::move(query));
}

void RayQueryBroker::Submit(Query query) {
    bool enqueued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || query.sceneEpoch == 0 || query.sceneEpoch != desiredSceneEpoch_) return;
        EnqueueLocked(std::move(query));
        enqueued = true;
    }
    if (enqueued) wake_.notify_one();
}

void RayQueryBroker::SubmitBatch(std::span<const Query> queries) {
    if (queries.empty()) return;
    bool enqueued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || desiredSceneEpoch_ == 0) return;
        for (const auto& query : queries) {
            if (query.sceneEpoch == 0 || query.sceneEpoch != desiredSceneEpoch_) continue;
            EnqueueLocked(query);
            enqueued = true;
        }
    }
    if (enqueued) wake_.notify_one();
}

std::optional<RayDecision> RayQueryBroker::Latest(
    const Request& request, uint64_t frame, uint64_t connectionEpoch,
    uint64_t sceneEpoch, uint64_t entityKey, uint64_t actorGeneration,
    int boneId, uint32_t maxAgeMs, float maxEndpointDriftMeters,
    RaySegmentKind segmentKind, uint8_t segmentIndex) const {
    if (sceneEpoch == 0) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : cache_) {
        if (entry.decision.frame > frame ||
            entry.decision.connectionEpoch != connectionEpoch ||
            entry.decision.sceneEpoch != sceneEpoch ||
            entry.decision.entityKey != entityKey ||
            entry.decision.actorGeneration != actorGeneration ||
            entry.boneId != boneId ||
            entry.segmentKind != segmentKind ||
            entry.segmentIndex != segmentIndex ||
            entry.request.mask != request.mask) {
            continue;
        }
        RayDecision current = entry.decision;
        if (current.completedAtUs != 0) {
            const uint64_t nowUs = RayDecisionNowUs();
            current.ageMs = nowUs >= current.completedAtUs
                ? static_cast<uint32_t>((nowUs - current.completedAtUs) / 1000u)
                : 0u;
        }
        // Phase 2 (§6.2): distinguish spatial drift from cold-cache miss by returning
        // an explicit Stale verdict with SpatialDrift reason rather than std::nullopt.
        if (!VecWithinDrift(entry.request.start, request.start, maxEndpointDriftMeters) ||
            !VecWithinDrift(entry.request.end, request.end, maxEndpointDriftMeters)) {
            current.world = RayDecisionStatus::Stale;
            current.staleReason = RayStaleReason::SpatialDrift;
            return current;
        }
        if (current.ageMs > maxAgeMs) {
            current.world = RayDecisionStatus::Stale;
            current.staleReason = RayStaleReason::AgeExpired;
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
            existing.segmentKind == entry.segmentKind &&
            existing.segmentIndex == entry.segmentIndex &&
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
    std::vector<Query> batch;
    std::vector<Request> requests;
    batch.reserve(kMaxBatchSize);
    requests.reserve(kMaxBatchSize);

    for (;;) {
        Service* service = nullptr;
        uint64_t serviceGen = 0;
        uint64_t sceneToBind = 0;
        bool needBind = false;
        batch.clear();
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
                const size_t take = (std::min)(kMaxBatchSize, pendingQueue_.size());
                for (size_t i = 0; i < take; ++i) {
                    batch.push_back(std::move(pendingQueue_[i]));
                }
                pendingQueue_.erase(pendingQueue_.begin(),
                                    pendingQueue_.begin() + static_cast<std::ptrdiff_t>(take));
                if (service != nullptr) {
                    inFlight_ = true;
                    for (const auto& q : batch) {
                        if (q.sceneEpoch != 0 &&
                            q.sceneEpoch == desiredSceneEpoch_ &&
                            boundSceneEpoch_ != desiredSceneEpoch_) {
                            needBind = true;
                            sceneToBind = desiredSceneEpoch_;
                            break;
                        }
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
            if (batch.empty()) {
                inFlight_ = false;
                idleCv_.notify_all();
                continue;
            }
            if (stopping_ || serviceGen != serviceGeneration_) {
                inFlight_ = false;
                idleCv_.notify_all();
                continue;
            }
        }

        if (batch.empty()) continue;

        requests.clear();
        requests.reserve(batch.size());
        for (const auto& q : batch) {
            requests.push_back(q.request);
        }

        const uint64_t startedUs = RayDecisionNowUs();
        std::vector<Result> traceResults;
        if (service != nullptr) {
            traceResults = service->TraceBatch(requests);
        }
        const uint64_t completedUs = RayDecisionNowUs();
        const uint32_t wallLatencyUs = completedUs >= startedUs
            ? static_cast<uint32_t>(completedUs - startedUs)
            : 0u;

        std::vector<CacheEntry> completedEntries;
        completedEntries.reserve(batch.size());
        for (size_t i = 0; i < batch.size(); ++i) {
            const Query& q = batch[i];
            RayDecision decision{};
            decision.frame = q.frame;
            decision.connectionEpoch = q.connectionEpoch;
            decision.sceneEpoch = q.sceneEpoch;
            decision.entityKey = q.entityKey;
            decision.actorGeneration = q.actorGeneration;
            decision.sampleTick = q.sampleTick;
            decision.segmentKind = q.segmentKind;
            decision.segmentIndex = q.segmentIndex;
            decision.world = RayDecisionStatus::Unavailable;
            decision.fraction = 1.0f;
            decision.staleReason = RayStaleReason::None;

            if (service != nullptr && i < traceResults.size()) {
                const Result& result = traceResults[i];
                decision.hitFlags = result.hitFlags;
                decision.hitbox = result.hitbox;
                decision.hitEntityId = result.entityId;
                switch (result.status) {
                    case Status::Ok:
                        if (std::isfinite(result.fraction) &&
                            result.fraction >= 0.0f && result.fraction <= 1.0f) {
                            decision.fraction = result.fraction;
                            decision.world = result.fraction < 1.0f
                                ? RayDecisionStatus::Occluded
                                : RayDecisionStatus::Clear;
                        } else {
                            decision.world = RayDecisionStatus::Invalid;
                        }
                        break;
                    case Status::Timeout:
                        decision.world = RayDecisionStatus::Timeout;
                        break;
                    case Status::InvalidScene:
                        decision.world = RayDecisionStatus::Stale;
                        decision.staleReason = RayStaleReason::SceneMismatch;
                        break;
                    case Status::Unavailable:
                        decision.world = RayDecisionStatus::Unavailable;
                        break;
                    case Status::Error:
                        decision.world = RayDecisionStatus::Invalid;
                        break;
                }
                decision.latencyUs = result.latencyUs != 0 ? result.latencyUs : wallLatencyUs;
            } else {
                decision.latencyUs = wallLatencyUs;
            }
            decision.ageMs = 0;
            decision.completedAtUs = completedUs;
            completedEntries.push_back(CacheEntry{
                decision, q.request, q.boneId, q.segmentKind, q.segmentIndex
            });
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (serviceGen == serviceGeneration_) {
                for (const auto& entry : completedEntries) {
                    if (entry.decision.sceneEpoch != 0 &&
                        entry.decision.sceneEpoch == desiredSceneEpoch_) {
                        StoreResultLocked(entry);
                    }
                }
            }
            if (inFlight_) {
                inFlight_ = false;
                idleCv_.notify_all();
            }
        }
    }
}

}  // namespace OW::EngineTrace
