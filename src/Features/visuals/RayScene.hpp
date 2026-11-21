#pragma once
#include <filesystem>

namespace V3::RayScene {
bool Open(const std::filesystem::path& path);
// Returns true when this frame belongs to the loaded scene.
bool Draw();
}
