#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Engine ray service boundary for V3 (UniCore experiment).
//
// This header deliberately contains no emulator, no DMA and no Unicorn dependency. It is the
// contract between the product loop and whatever eventually answers "is this segment blocked
// by world geometry". The first backend is expected to be the external UniCore ray probe;
// a null backend ships now so the boundary can be compiled and reviewed before any runtime
// behaviour depends on it.
//
// Design notes that come from the 2026-11-21 field work:
//   - A ray is expensive (55-100 ms, 83-137 demand pages measured cold on CN 155284N), so the
//     contract is built for one request at a time, not for a per-frame sweep.
//   - The engine trace context is scene-bound. The backend must BindScene() once per scene and
//     report Unavailable/InvalidScene once the scene is gone, instead of returning stale data.
//   - A missing answer is never silently turned into "visible". Callers must see the status.

namespace OW::EngineTrace {

enum class Status : uint8_t {
    Ok = 0,
    Unavailable,   // backend has no bound scene or is not running
    Timeout,       // backend exceeded the caller budget
    InvalidScene,  // the request names a scene the backend is not bound to
    Error,
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline constexpr uint64_t kDefaultWorldMask = 0x0690000000000017ull;

struct Request {
    Vec3 start{};
    Vec3 end{};
    // Engine layer mask. Live CN 155284N verification (2026-11-27):
    // 0x0690000000000017 traces static world/terrain occlusion only (visible enemies return
    // fraction=1.0, walled enemies return wall hit with flags=0x24). 0x0690000000000002 hits
    // actor hitboxes (hitbox=0xFFFF0004/0xFFFF0028, fraction<1.0 on visible targets) and must
    // NOT be used for static world occlusion.
    uint64_t mask = kDefaultWorldMask;
    // Caller's scene generation. A mismatch means the answer would describe another scene.
    uint64_t sceneEpoch = 0;
    // 0 means "backend default". The backend owns enforcement; the service records overruns.
    uint32_t budgetMs = 0;
};

struct Result {
    Status status = Status::Unavailable;
    float fraction = 1.0f;  // 1.0 = segment clear, < 1.0 = blocked at that fraction
    Vec3 hitPosition{};
    Vec3 hitNormal{};
    bool hasHitPosition = false;
    bool hasHitNormal = false;
    uint32_t hitbox = 0xFFFFFFFFu;
    uint32_t entityId = 0xFFFFFFFFu;
    uint32_t hitFlags = 0;
    uint64_t instructions = 0;
    uint32_t latencyUs = 0;
    bool overBudget = false;
    bool fromCache = false;
};

class IBackend {
public:
    virtual ~IBackend() = default;
    virtual const char* Name() const = 0;
    // Binds the backend to one scene. Called once per scene change; may be expensive.
    virtual bool BindScene(uint64_t sceneEpoch) = 0;
    virtual Status Trace(const Request& request, Result& result) = 0;
    // Batch dispatch hook. Multi-engine backends (e.g. 6-engine VisionProbe) override this
    // to execute requests concurrently in one wave; single-engine backends fall back to Trace().
    virtual void TraceBatch(std::span<const Request> requests, std::span<Result> results) {
        const size_t count = (std::min)(requests.size(), results.size());
        for (size_t i = 0; i < count; ++i) {
            results[i] = Result{};
            results[i].status = Trace(requests[i], results[i]);
        }
    }
    virtual void ReleaseScene() = 0;
};

struct Stats {
    uint64_t requests = 0;
    uint64_t ok = 0;
    uint64_t unavailable = 0;
    uint64_t timeouts = 0;
    uint64_t invalidScene = 0;
    uint64_t errors = 0;
    uint64_t overBudget = 0;
    uint32_t lastLatencyUs = 0;
    uint32_t maxLatencyUs = 0;
};

class Service {
public:
    explicit Service(IBackend* backend = nullptr);
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    // Installs a backend. Releases the previous backend's scene first.
    void SetBackend(IBackend* backend);

    // Called by the runtime when the scene generation changes. Drops the old binding and asks
    // the backend to bind the new scene. Returns false when no backend could bind it.
    bool OnSceneChanged(uint64_t sceneEpoch);

    // Single-request synchronous trace.
    Result Trace(const Request& request);

    // Concurrent multi-ray batch trace. Preserves caller order and per-request scene validation.
    std::vector<Result> TraceBatch(std::span<const Request> requests);

    const Stats& GetStats() const { return stats_; }
    void ResetStats() { stats_ = Stats{}; }
    uint64_t BoundScene() const { return boundEpoch_; }
    bool IsBound() const { return bound_; }

private:
    IBackend* backend_ = nullptr;
    uint64_t boundEpoch_ = 0;
    bool bound_ = false;
    Stats stats_{};
};

}  // namespace OW::EngineTrace
