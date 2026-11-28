#pragma once

#include "Game/EngineTrace.hpp"
#include "Game/RayDecision.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace OW::EngineTrace {

// Owns the asynchronous world-query handoff. All Service::OnSceneChanged and
// Service::TraceBatch calls execute strictly on the broker's worker thread so the
// aim loop never blocks on Unicorn bind/trace and Detach() can synchronously
// drain any in-flight call before the caller destroys the Service.
class RayQueryBroker final {
public:
    static constexpr float kDefaultMaxEndpointDriftMeters = 0.35f;
    static constexpr size_t kMaxBatchSize = 6;
    static constexpr size_t kMaxPendingEntries = 16;
    static constexpr size_t kMaxCacheEntries = 64;

    struct Query {
        Request request{};
        uint64_t frame = 0;
        uint64_t connectionEpoch = 0;
        uint64_t sceneEpoch = 0;
        uint64_t entityKey = 0;
        uint64_t actorGeneration = 0;
        uint64_t bodyAuthorityRevision = 0;
        int boneId = -1;
        uint32_t sampleTick = 0;
        RaySegmentKind segmentKind = RaySegmentKind::Direct;
        uint8_t segmentIndex = 0;
    };

    RayQueryBroker() = default;
    ~RayQueryBroker();
    RayQueryBroker(const RayQueryBroker&) = delete;
    RayQueryBroker& operator=(const RayQueryBroker&) = delete;

    void Start();
    void Stop();
    void Attach(Service* service);
    void Detach();
    bool HasService() const noexcept;
    uint32_t ConsecutiveErrorWaves() const noexcept {
        return consecutiveErrorWaves_.load(std::memory_order_acquire);
    }
    uint32_t LastWaveLatencyUs() const noexcept {
        return lastWaveLatencyUs_.load(std::memory_order_acquire);
    }
    void SetScene(uint64_t sceneEpoch);
    void Submit(Query query);
    void SubmitBatch(std::span<const Query> queries);
    std::optional<RayDecision> Latest(
        const Request& request, uint64_t frame, uint64_t connectionEpoch,
        uint64_t sceneEpoch, uint64_t entityKey, uint64_t actorGeneration,
        int boneId = -1, uint32_t maxAgeMs = 100,
        float maxEndpointDriftMeters = kDefaultMaxEndpointDriftMeters,
        RaySegmentKind segmentKind = RaySegmentKind::Direct,
        uint8_t segmentIndex = 0,
        uint64_t bodyAuthorityRevision = 0) const;

private:
    struct CacheEntry {
        RayDecision decision{};
        Request request{};
        int boneId = -1;
        RaySegmentKind segmentKind = RaySegmentKind::Direct;
        uint8_t segmentIndex = 0;
    };

    void Run();
    void EnqueueLocked(Query query);
    void StoreResultLocked(const CacheEntry& entry);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idleCv_;
    std::thread worker_;
    bool running_ = false;
    bool stopping_ = false;
    bool inFlight_ = false;
    std::atomic<bool> hasService_{false};
    std::atomic<uint32_t> consecutiveErrorWaves_{0};
    std::atomic<uint32_t> lastWaveLatencyUs_{0};
    Service* service_ = nullptr;
    uint64_t serviceGeneration_ = 0;
    uint64_t desiredSceneEpoch_ = 0;
    uint64_t boundSceneEpoch_ = 0;
    bool sceneDirty_ = false;
    std::vector<Query> pendingQueue_;
    std::vector<CacheEntry> cache_;
};

}  // namespace OW::EngineTrace
