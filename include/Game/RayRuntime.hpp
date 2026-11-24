#pragma once

#include "Game/RayQueryBroker.hpp"

namespace OW::RayRuntime {

void Start();
void Stop();
void Attach(EngineTrace::Service* service);
void Detach();
void SetScene(uint64_t sceneEpoch);
EngineTrace::RayQueryBroker& Broker();

}  // namespace OW::RayRuntime
