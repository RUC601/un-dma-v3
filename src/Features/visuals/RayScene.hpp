#pragma once
#include <filesystem>

namespace V3::RayScene {
bool Open(const std::filesystem::path& path);
void Close();
bool IsOpen();
// Returns true when this frame belongs to the loaded scene.
bool Draw();
}
