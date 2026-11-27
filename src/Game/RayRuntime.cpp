#include "Game/RayRuntime.hpp"

#include "Game/Offsets.hpp"

#include <atomic>

namespace OW::RayRuntime {
namespace {
EngineTrace::RayQueryBroker broker;
std::atomic<uint64_t> barrierMaskOverrideForTest{0};
}

void Start() {
    broker.SetScene(0);
    broker.Start();
}

void Stop() { broker.Stop(); }

void Attach(EngineTrace::Service* service) { broker.Attach(service); }

void Detach() { broker.Detach(); }

bool HasAttachedService() noexcept { return broker.HasService(); }

void SetScene(uint64_t sceneEpoch) { broker.SetScene(sceneEpoch); }

EngineTrace::RayQueryBroker& Broker() { return broker; }

uint64_t WorldRayMask() noexcept {
    const uint64_t mask = offset::Active().rayWorldMask;
    return mask != 0 ? mask : EngineTrace::kDefaultWorldMask;
}

uint64_t BarrierRayMask() noexcept {
    const uint64_t overrideMask = barrierMaskOverrideForTest.load(std::memory_order_acquire);
    if (overrideMask != 0)
        return overrideMask;
    return offset::Active().rayBarrierMask;
}

void SetBarrierRayMaskOverrideForTest(uint64_t mask) noexcept {
    barrierMaskOverrideForTest.store(mask, std::memory_order_release);
}

}  // namespace OW::RayRuntime
