#pragma once

#include "Game/EngineTrace.hpp"
#include "Game/RayDecision.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace OW::EngineTrace {

// Owns the asynchronous world-query handoff. All Service::OnSceneChanged and
// Service::Trace calls execute strictly on the broker's worker thread so the
// aim loop never blocks on Unicorn bind/trace and Detach() can synchronously
// drain any in-flight call before the caller destroys the Service.
class RayQueryBroker final {
public:
    static constexpr float kDefaultMaxEndpointDriftMeters = 0.35f;
    static constexpr size_t kMaxPendingEntries = 4;
    static constexpr size_t kMaxCacheEntries = 32;

    struct Query {
        Request request{};
        uint64_t frame = 0;
        uint64_t connectionEpoch = 0;
        uint64_t sceneEpoch = 0;
        uint64_t entityKey = 0;
        uint64_t actorGeneration = 0;
        int boneId = -1;
        uint32_t sampleTick = 0;
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
    void SetScene(uint64_t sceneEpoch);
    void Submit(Query query);
    std::optional<RayDecision> Latest(
        const Request& request, uint64_t frame, uint64_t connectionEpoch,
        uint64_t sceneEpoch, uint64_t entityKey, uint64_t actorGeneration,
        int boneId = -1, uint32_t maxAgeMs = 100,
        float maxEndpointDriftMeters = kDefaultMaxEndpointDriftMeters) const;

private:
    struct CacheEntry {
        RayDecision decision{};
        Request request{};
        int boneId = -1;
    };

    void Run();
    void StoreResultLocked(const CacheEntry& entry);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idleCv_;
    std::thread worker_;
    bool running_ = false;
    bool stopping_ = false;
    bool inFlight_ = false;
    std::atomic<bool> hasService_{false};
    Service* service_ = nullptr;
    uint64_t serviceGeneration_ = 0;
    uint64_t desiredSceneEpoch_ = 0;
    uint64_t boundSceneEpoch_ = 0;
    bool sceneDirty_ = false;
    std::vector<Query> pendingQueue_;
    std::vector<CacheEntry> cache_;
};

}  // namespace OW::EngineTrace
