#include "Game/RayRuntime.hpp"

namespace OW::RayRuntime {
namespace {
EngineTrace::RayQueryBroker broker;
}

void Start() {
    broker.SetScene(0);
    broker.Start();
}

void Stop() { broker.Stop(); }

void Attach(EngineTrace::Service* service) { broker.Attach(service); }

void Detach() { broker.Detach(); }

void SetScene(uint64_t sceneEpoch) { broker.SetScene(sceneEpoch); }

EngineTrace::RayQueryBroker& Broker() { return broker; }

}  // namespace OW::RayRuntime
