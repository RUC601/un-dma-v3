#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include <imgui.h>

namespace UI::Preferences {

enum class VisibilityMode : int {
    Lite = 0,   // Entity Vis + local native Hitbox only (zero capture, zero ray)
    Vision = 1, // Entity Vis + global Capture/Contour exposure fallback (Aim + Trigger, zero ray)
    Ray = 2,    // UniCore 6-engine async ray pipeline (+ optional per-slot BarrierPolicy)
};

struct ThemePreset {
    std::string id;
    std::string name;
    ImVec4 baseDark{17.f / 255.f, 18.f / 255.f, 23.f / 255.f, 1.f};
    ImVec4 accent{228.f / 255.f, 17.f / 255.f, 67.f / 255.f, 1.f};
    bool builtin = false;
};

inline constexpr std::size_t kMaxCustomThemePresets = 24;

struct Values {
    int language = 0;
    int skin = 0;
    int rayBackend = 0;      // Derived mirror: visibilityMode == Ray ? 1 : 0
    int visibilityMode = 0;  // 0=Lite, 1=Vision, 2=Ray
    bool reducedMotion = false;
    bool loginGlass = true;
    bool consoleLayers = true;
    bool sidebarCollapsed = false;
    int windowWidth = 1160, windowHeight = 800;
    ImVec4 baseDark{17.f / 255.f, 18.f / 255.f, 23.f / 255.f, 1.f};
    ImVec4 accent{228.f / 255.f, 17.f / 255.f, 67.f / 255.f, 1.f};
    bool customAccent = false;
    std::string activeThemeId = "crimson";
    std::vector<ThemePreset> customThemes;
};

extern Values values;

bool Load(int legacyLanguage);
bool Save();

int ActiveVisibilityMode() noexcept;
void SetActiveVisibilityMode(int value) noexcept;

// Derived 2-state ray backend projection (0 = NativeOnly, 1 = Hybrid) for existing callers.
int ActiveRayBackend() noexcept;
void SetActiveRayBackend(int value) noexcept;

const std::vector<ThemePreset>& BuiltinThemePresets();
const ThemePreset* FindThemePreset(const std::string& id);
bool ApplyThemePreset(const std::string& id);
std::string AddCustomThemePreset(const std::string& rawName, ImVec4 baseDark, ImVec4 accent);
bool DeleteCustomThemePreset(const std::string& id);

const std::string& Error();
bool Loaded();
void ResetForTesting();
}
