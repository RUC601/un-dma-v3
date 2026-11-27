#include "config/UiPreferences.hpp"
#include "config/RayBackendMigration.hpp"
#include "ui/Theme.hpp"

#include <Windows.h>
#include <rapidjson/document.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
std::string g_testConfigDir;
} // namespace

namespace OW::Config {
std::string ConfigDirectoryPath() {
    return g_testConfigDir;
}
} // namespace OW::Config

namespace {

bool Near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

int Fail(const char* msg) {
    std::cerr << "FAIL " << msg << '\n';
    return EXIT_FAILURE;
}

std::string PrefsPath() {
    return g_testConfigDir + "/ui-preferences.json";
}

bool WriteTextFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

bool ReadTextFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), {});
    return true;
}

bool RemovePrefsFile() {
    std::error_code ec;
    std::filesystem::remove(PrefsPath(), ec);
    return !std::filesystem::exists(PrefsPath(), ec);
}

bool TestFirstLoadDefaults() {
    if (!RemovePrefsFile()) return false;
    UI::Preferences::ResetForTesting();
    if (!UI::Preferences::Load(1)) return false;
    if (!UI::Preferences::Loaded() || !UI::Preferences::Error().empty()) return false;
    if (UI::Preferences::values.language != 1) return false;
    if (UI::Preferences::ActiveVisibilityMode() != 0 || UI::Preferences::ActiveRayBackend() != 0) return false;
    if (UI::Preferences::values.activeThemeId != "crimson") return false;
    if (!UI::Preferences::values.customThemes.empty()) return false;
    return true;
}

bool TestV1AndV2Migration() {
    // v1: no visibilityMode, no rayBackend, customAccent=true with accent override.
    if (!WriteTextFile(PrefsPath(),
            R"({"version":1,"language":1,"skin":2,"customAccent":true,"accent":[0.1,0.8,0.4,1.0]})"))
        return false;
    UI::Preferences::ResetForTesting();
    if (!UI::Preferences::Load(0)) return false;
    if (UI::Preferences::ActiveVisibilityMode() != 0 || UI::Preferences::ActiveRayBackend() != 0) return false;
    if (UI::Preferences::values.language != 1) return false;
    if (!UI::Preferences::values.activeThemeId.empty() || !UI::Preferences::values.customAccent) return false;
    if (!Near(UI::Preferences::values.accent.x, 0.1f) || !Near(UI::Preferences::values.accent.y, 0.8f) ||
        !Near(UI::Preferences::values.accent.z, 0.4f))
        return false;

    // v2 with rayBackend=1 -> migrates to VisibilityMode::Ray (2) and upgrades to v3 on Save().
    if (!WriteTextFile(PrefsPath(),
            R"({"version":2,"language":0,"rayBackend":1,"skin":1,"customAccent":false})"))
        return false;
    UI::Preferences::ResetForTesting();
    if (!UI::Preferences::Load(1)) return false;
    if (UI::Preferences::ActiveVisibilityMode() != 2 || UI::Preferences::ActiveRayBackend() != 1) return false;
    if (UI::Preferences::values.activeThemeId != "graphite") return false;
    if (!UI::Preferences::Save()) return false;

    std::string savedRaw;
    if (!ReadTextFile(PrefsPath(), savedRaw)) return false;
    rapidjson::Document doc;
    doc.Parse(savedRaw.c_str(), savedRaw.size());
    if (doc.HasParseError() || !doc.IsObject()) return false;
    if (!doc.HasMember("version") || doc["version"].GetInt() != UI::Preferences::kCurrentVersion) return false;
    if (!doc.HasMember("visibilityMode") || doc["visibilityMode"].GetInt() != 2) return false;
    if (!doc.HasMember("rayBackend") || doc["rayBackend"].GetInt() != 1) return false;

    // v2 with rayBackend=0 -> migrates to VisibilityMode::Lite (0).
    if (!WriteTextFile(PrefsPath(), R"({"version":2,"rayBackend":0,"skin":0})")) return false;
    UI::Preferences::ResetForTesting();
    if (!UI::Preferences::Load(0)) return false;
    if (UI::Preferences::ActiveVisibilityMode() != 0 || UI::Preferences::ActiveRayBackend() != 0) return false;
    return true;
}

bool TestMalformedPreservation() {
    // 1. Corrupt JSON must fail Load(), refuse Save(), and leave original bytes intact on disk.
    const std::string brokenJson = "{ \"version\": 3, \"broken\": [1, 2, ";
    if (!WriteTextFile(PrefsPath(), brokenJson)) return false;
    UI::Preferences::ResetForTesting();
    if (UI::Preferences::Load(0)) return false;
    if (UI::Preferences::Error().rfind("Invalid UI preferences", 0) != 0) return false;
    if (UI::Preferences::Save()) return false;
    std::string afterSave;
    if (!ReadTextFile(PrefsPath(), afterSave) || afterSave != brokenJson) return false;

    // 2. Unsupported future version must also preserve original bytes.
    const std::string futureJson = R"({"version":99,"visibilityMode":2})";
    if (!WriteTextFile(PrefsPath(), futureJson)) return false;
    UI::Preferences::ResetForTesting();
    if (UI::Preferences::Load(0)) return false;
    if (UI::Preferences::Error().rfind("Invalid UI preferences", 0) != 0) return false;
    if (UI::Preferences::Save()) return false;
    if (!ReadTextFile(PrefsPath(), afterSave) || afterSave != futureJson) return false;
    return true;
}

bool TestCustomThemeRoundTripAndSanitization() {
    // 1. Save custom theme + Vision mode -> reload -> verify exact round-trip.
    if (!RemovePrefsFile()) return false;
    UI::Preferences::ResetForTesting();
    if (!UI::Preferences::Load(0)) return false;
    UI::Preferences::SetActiveVisibilityMode(static_cast<int>(UI::Preferences::VisibilityMode::Vision));
    const ImVec4 dark(0.05f, 0.08f, 0.12f, 1.0f);
    const ImVec4 accent(0.18f, 0.88f, 0.66f, 1.0f);
    const std::string createdId = UI::Preferences::AddCustomThemePreset("  Neon Mint  ", dark, accent);
    if (createdId.empty()) return false;
    if (UI::Preferences::values.activeThemeId != createdId) return false;
    if (!UI::Preferences::Save()) return false;

    UI::Preferences::ResetForTesting();
    if (!UI::Preferences::Load(0)) return false;
    if (UI::Preferences::ActiveVisibilityMode() != 1 || UI::Preferences::ActiveRayBackend() != 0) return false;
    if (UI::Preferences::values.activeThemeId != createdId) return false;
    if (UI::Preferences::values.customThemes.size() != 1) return false;
    if (UI::Preferences::values.customThemes[0].name != "Neon Mint") return false;
    if (!Near(UI::Preferences::values.baseDark.x, 0.05f) || !Near(UI::Preferences::values.baseDark.z, 0.12f)) return false;
    if (!Near(UI::Preferences::values.accent.y, 0.88f) || !Near(UI::Preferences::values.accent.z, 0.66f)) return false;

    // Deleting built-in must fail; deleting active custom theme clears activeThemeId.
    if (UI::Preferences::DeleteCustomThemePreset("crimson")) return false;
    if (!UI::Preferences::DeleteCustomThemePreset(createdId)) return false;
    if (!UI::Preferences::values.activeThemeId.empty() || !UI::Preferences::values.customThemes.empty()) return false;

    // 2. Duplicate IDs, built-in ID collision, invalid color arrays, and out-of-range color clamping.
    const std::string dirtyV3 = R"({
        "version": 3,
        "visibilityMode": 1,
        "activeThemeId": "dup-1",
        "baseDark": [0.06, 0.07, 0.09, 1.0],
        "accent": [1.5, -0.25, 0.5, 0.2],
        "customThemes": [
            {"id": "dup-1", "name": "First Valid", "baseDark": [0.04, 0.05, 0.06], "accent": [0.2, 0.6, 0.9, 1.0]},
            {"id": "dup-1", "name": "Duplicate Ignored", "baseDark": [0.1, 0.1, 0.1], "accent": [0.9, 0.1, 0.1]},
            {"id": "crimson", "name": "Builtin Collision", "baseDark": [0.1, 0.1, 0.1], "accent": [0.1, 0.9, 0.1]},
            {"id": "bad-color-len", "name": "Short Color", "baseDark": [0.1, 0.1], "accent": [0.2, 0.3, 0.4]},
            {"id": "bad-color-type", "name": "String Color", "baseDark": ["x", 0.1, 0.1], "accent": [0.2, 0.3, 0.4]},
            {"id": "empty-name", "name": "   ", "baseDark": [0.1, 0.1, 0.1], "accent": [0.2, 0.3, 0.4]},
            {"id": "clamped-2", "name": "Clamped", "baseDark": [2.0, -1.0, 0.1, 0.1], "accent": [-0.5, 1.8, 0.3, 0.4]}
        ]
    })";
    if (!WriteTextFile(PrefsPath(), dirtyV3)) return false;
    UI::Preferences::ResetForTesting();
    if (!UI::Preferences::Load(0)) return false;
    if (UI::Preferences::values.customThemes.size() != 2) return false;
    if (UI::Preferences::values.customThemes[0].id != "dup-1" ||
        UI::Preferences::values.customThemes[0].name != "First Valid")
        return false;
    if (UI::Preferences::values.customThemes[1].id != "clamped-2")
        return false;
    const ImVec4 clampedBase = UI::Preferences::values.customThemes[1].baseDark;
    const ImVec4 clampedAcc = UI::Preferences::values.customThemes[1].accent;
    if (!Near(clampedBase.x, 1.0f) || !Near(clampedBase.y, 0.0f) || !Near(clampedBase.w, 1.0f)) return false;
    if (!Near(clampedAcc.x, 0.0f) || !Near(clampedAcc.y, 1.0f) || !Near(clampedAcc.w, 1.0f)) return false;
    if (!Near(UI::Preferences::values.accent.x, 1.0f) || !Near(UI::Preferences::values.accent.y, 0.0f) ||
        !Near(UI::Preferences::values.accent.w, 1.0f))
        return false;

    return true;
}

bool TestConcurrentVisibilityModeReads() {
    UI::Preferences::ResetForTesting();
    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};
    std::thread writer([&]() {
        for (int i = 0; i < 4000; ++i) {
            UI::Preferences::SetActiveVisibilityMode(i % 3);
        }
        stop.store(true, std::memory_order_release);
    });
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            const int m = UI::Preferences::ActiveVisibilityMode();
            const int r = UI::Preferences::ActiveRayBackend();
            if (m < 0 || m > 2 || (r != 0 && r != 1)) {
                bad.store(true, std::memory_order_release);
                break;
            }
        }
    });
    writer.join();
    reader.join();
    if (bad.load(std::memory_order_acquire)) return false;

    // Verify exact deterministic projection after writer finishes.
    for (int m = 0; m < 3; ++m) {
        UI::Preferences::SetActiveVisibilityMode(m);
        if (UI::Preferences::ActiveVisibilityMode() != m) return false;
        if (UI::Preferences::ActiveRayBackend() != (m == 2 ? 1 : 0)) return false;
    }
    return true;
}

bool TestSanitizeBaseDarkColor() {
    const ImVec4 bright = UI::Theme::SanitizeBaseDarkColor(ImVec4(1.0f, 1.0f, 1.0f, 0.3f));
    const float brightLum = 0.2126f * bright.x + 0.7152f * bright.y + 0.0722f * bright.z;
    if (brightLum > 0.1755f || !Near(bright.w, 1.0f)) return false;

    const ImVec4 darkIn(0.06f, 0.07f, 0.09f, 0.8f);
    const ImVec4 darkOut = UI::Theme::SanitizeBaseDarkColor(darkIn);
    if (!Near(darkOut.x, darkIn.x) || !Near(darkOut.y, darkIn.y) || !Near(darkOut.z, darkIn.z) || !Near(darkOut.w, 1.0f))
        return false;

    const float nanVal = std::numeric_limits<float>::quiet_NaN();
    const ImVec4 weird = UI::Theme::SanitizeBaseDarkColor(ImVec4(nanVal, -2.0f, 5.0f, nanVal));
    const float weirdLum = 0.2126f * weird.x + 0.7152f * weird.y + 0.0722f * weird.z;
    if (!std::isfinite(weird.x) || !std::isfinite(weird.y) || !std::isfinite(weird.z) ||
        weird.x < 0.0f || weird.y < 0.0f || weird.z < 0.0f || weirdLum > 0.1755f || !Near(weird.w, 1.0f))
        return false;
    return true;
}

} // namespace

int main() {
    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path(ec) /
        ("un_dma_v3_uiprefs_selftest_" + std::to_string(GetCurrentProcessId()));
    if (ec) return Fail("resolve temp directory");
    std::filesystem::create_directories(tempDir, ec);
    if (ec) return Fail("create temp directory");
    g_testConfigDir = tempDir.string();

    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() {
            std::error_code cleanupEc;
            std::filesystem::remove_all(dir, cleanupEc);
        }
    } cleanup{tempDir};

    if (!TestFirstLoadDefaults()) return Fail("first-load defaults");
    if (!TestV1AndV2Migration()) return Fail("v1/v2 migration and v3 save upgrade");
    if (!TestMalformedPreservation()) return Fail("malformed JSON / unsupported version preservation");
    if (!TestCustomThemeRoundTripAndSanitization()) return Fail("v3 customThemes round-trip, duplicates, and color sanitization");
    if (!TestConcurrentVisibilityModeReads()) return Fail("concurrent visibility mode reads/writes");
    if (!TestSanitizeBaseDarkColor()) return Fail("UI::Theme::SanitizeBaseDarkColor luminance cap");

    std::cout << "PASS V3UiPreferencesSelfTest: defaults + v1/v2/v3 migration + malformed preservation + customThemes + concurrent mode + SanitizeBaseDarkColor\n";
    return EXIT_SUCCESS;
}
