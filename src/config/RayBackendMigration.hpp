#pragma once

namespace UI::Preferences {

// ui-preferences.json schema versions.
//   1: rayBackend 0=Native, 1=Embree, 2=UniCore (Embree has since been removed).
//   2: rayBackend 0=Native-only, 1=Hybrid.
//   3: visibilityMode 0=Lite (entity Vis only), 1=Vision (global contour), 2=Ray (UniCore),
//      plus dual-color theme presets (baseDark + accent + customThemes).
// A missing version field is treated as the legacy schema.
constexpr int kLegacyVersion = 1;
constexpr int kRayBackendV2Version = 2;
constexpr int kCurrentVersion = 3;
constexpr int kRayBackendCount = 2;
constexpr int kVisibilityModeCount = 3;

// Maps a stored rayBackend value to the v2 2-state policy. Legacy Embree falls back to Native-only.
// Returns false (leaving `out` untouched) for values outside the schema's range.
inline bool MigrateRayBackend(int version, unsigned saved, int& out) {
    if (version <= kLegacyVersion) {
        if (saved <= 1) { out = 0; return true; }   // Native, or removed Embree -> Native-only
        if (saved == 2) { out = 1; return true; }   // UniCore -> Hybrid
        return false;
    }
    if (saved < static_cast<unsigned>(kRayBackendCount)) { out = static_cast<int>(saved); return true; }
    return false;
}

// Maps stored preferences (v1/v2 rayBackend or v3 visibilityMode) to the unified 3-state
// visibility mode: 0 = Lite, 1 = Vision, 2 = Ray.
inline bool MigrateVisibilityMode(int version,
                                  bool hasVisibilityMode,
                                  unsigned savedVisibilityMode,
                                  bool hasRayBackend,
                                  unsigned savedRayBackend,
                                  int& outMode) {
    if (version >= kCurrentVersion && hasVisibilityMode) {
        if (savedVisibilityMode < static_cast<unsigned>(kVisibilityModeCount)) {
            outMode = static_cast<int>(savedVisibilityMode);
            return true;
        }
        return false;
    }
    if (hasRayBackend) {
        int legacyBackend = 0;
        if (!MigrateRayBackend(version, savedRayBackend, legacyBackend))
            return false;
        outMode = (legacyBackend == 1) ? 2 : 0;
        return true;
    }
    outMode = 0;
    return true;
}

}  // namespace UI::Preferences
