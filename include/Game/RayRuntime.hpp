#pragma once

#include "Game/RayQueryBroker.hpp"

#include <cstdint>

namespace OW::RayRuntime {

void Start();
void Stop();
void Attach(EngineTrace::Service* service);
void Detach();
bool HasAttachedService() noexcept;
void SetScene(uint64_t sceneEpoch);
EngineTrace::RayQueryBroker& Broker();

// ---------------------------------------------------------------------------
// Live UniCore probe (V3 product integration)
//
// The probe owner resolves the level context through the product's own
// TryGetRayContext() (Game/Decrypt.hpp), builds the VisionProbe engines from the active
// profile (entry RVA, mask, image size), and watches the scene:
//
//   * the context vtable must stay inside the module image (one DMA qword), and
//   * the freshly resolved context must equal the one the engines were bound to.
//
// Either check failing means a new level instance (map/match change) or a dead one. The
// owner then advances the ray scene epoch and calls RayQueryBroker::SetScene(); the
// broker releases the old engines and their page cache on its own thread and binds the
// new context. No stack capture and no setup template file are involved.
// ---------------------------------------------------------------------------

void StartProbe();
void StopProbe();
// Synchronously detaches the live probe and drains any in-flight Unicorn Trace
// before the process session detaches target memory or closes the DMA handle.
void DetachProbeForDisconnect();
bool ProbeRunning() noexcept;

// Scene epoch owned by the probe owner. 0 means no live ray scene: consumers must treat
// world occlusion as unavailable instead of reusing an older scene's answers.
uint64_t SceneEpoch() noexcept;

// Configured concurrent Unicorn engine count for live VisionProbe batches (default 6).
int ProbeEngineCount() noexcept;

}  // namespace OW::RayRuntime
