#pragma once

// Bounded, read-only live check of the UniCore ray backend. Used by
// `UNDMA_V3.exe --ray-probe ...` after the product config is loaded and before the
// overlay starts: it resolves the level context through the same
// OW::TryGetRayContext() the runtime uses, binds a VisionProbe with the active
// profile's entry RVA, and traces the caller's segments.
int RunRayProbeCheck(int argc, char** argv);
