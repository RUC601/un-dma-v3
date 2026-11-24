#pragma once

namespace UI::Preferences {

// ui-preferences.json schema versions.
//   1: rayBackend 0=Native, 1=Embree, 2=UniCore (Embree has since been removed).
//   2: rayBackend 0=Native-only, 1=Hybrid.
// A missing version field is treated as the legacy schema.
constexpr int kLegacyVersion = 1;
constexpr int kCurrentVersion = 2;
constexpr int kRayBackendCount = 2;

// Maps a stored rayBackend value to the current policy. Legacy Embree falls back to Native-only.
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

}  // namespace UI::Preferences
