#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING
#define _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING
#endif
#include "UiPreferences.hpp"
#include "RayBackendMigration.hpp"
#include "FileTransaction.hpp"
#include "Utils/Config.hpp"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace UI::Preferences {
Values values;
namespace {
bool loaded = false;
std::string error;
std::atomic<int> activeVisibilityMode{0};

std::string Path() { return OW::Config::ConfigDirectoryPath() + "/ui-preferences.json"; }

ImVec4 ClampOpaqueColor(ImVec4 c) noexcept {
    const auto safe = [](float v) noexcept {
        return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    };
    return ImVec4(safe(c.x), safe(c.y), safe(c.z), 1.0f);
}

bool ReadColorArray(const rapidjson::Value& obj, const char* key, ImVec4& out, bool preserveAlpha) {
    if (!obj.HasMember(key) || !obj[key].IsArray())
        return false;
    const auto& arr = obj[key];
    if (arr.Size() != 3 && arr.Size() != 4)
        return false;
    ImVec4 next = out;
    for (unsigned i = 0; i < arr.Size(); ++i) {
        if (!arr[i].IsNumber() || !std::isfinite(arr[i].GetFloat()))
            return false;
        (&next.x)[i] = std::clamp(arr[i].GetFloat(), 0.0f, 1.0f);
    }
    if (!preserveAlpha || arr.Size() == 3)
        next.w = 1.0f;
    out = next;
    return true;
}

void WriteColorArray(rapidjson::Writer<rapidjson::StringBuffer>& w, const char* key, const ImVec4& c) {
    w.Key(key);
    w.StartArray();
    w.Double(std::clamp(std::isfinite(c.x) ? c.x : 0.0f, 0.0f, 1.0f));
    w.Double(std::clamp(std::isfinite(c.y) ? c.y : 0.0f, 0.0f, 1.0f));
    w.Double(std::clamp(std::isfinite(c.z) ? c.z : 0.0f, 0.0f, 1.0f));
    w.Double(std::clamp(std::isfinite(c.w) ? c.w : 1.0f, 0.0f, 1.0f));
    w.EndArray();
}

std::string TrimName(const std::string& raw) {
    size_t begin = 0;
    while (begin < raw.size() && std::isspace(static_cast<unsigned char>(raw[begin])))
        ++begin;
    size_t end = raw.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1])))
        --end;
    std::string out = raw.substr(begin, end - begin);
    if (out.size() > 40)
        out.resize(40);
    return out;
}

int LegacySkinForBuiltinId(const std::string& id) {
    if (id == "graphite") return 1;
    if (id == "violet") return 2;
    return 0;
}

const char* BuiltinIdForLegacySkin(int skin) {
    if (skin == 1) return "graphite";
    if (skin == 2) return "violet";
    return "crimson";
}
} // namespace

const std::vector<ThemePreset>& BuiltinThemePresets() {
    static const std::vector<ThemePreset> kBuiltins{
        { "crimson",  "UN Crimson", ImVec4(16.f / 255.f, 17.f / 255.f, 21.f / 255.f, 1.f), ImVec4(228.f / 255.f, 17.f / 255.f, 67.f / 255.f, 1.f),  true },
        { "graphite", "Graphite",   ImVec4(21.f / 255.f, 23.f / 255.f, 25.f / 255.f, 1.f), ImVec4(190.f / 255.f, 200.f / 255.f, 220.f / 255.f, 1.f), true },
        { "violet",   "ED Violet",  ImVec4(15.f / 255.f, 16.f / 255.f, 19.f / 255.f, 1.f), ImVec4(171.f / 255.f, 147.f / 255.f, 255.f / 255.f, 1.f), true },
    };
    return kBuiltins;
}

const ThemePreset* FindThemePreset(const std::string& id) {
    if (id.empty()) return nullptr;
    for (const auto& preset : BuiltinThemePresets())
        if (preset.id == id) return &preset;
    for (const auto& preset : values.customThemes)
        if (preset.id == id) return &preset;
    return nullptr;
}

bool ApplyThemePreset(const std::string& id) {
    const ThemePreset* found = FindThemePreset(id);
    if (!found) return false;
    values.activeThemeId = found->id;
    values.baseDark = ClampOpaqueColor(found->baseDark);
    values.accent = ClampOpaqueColor(found->accent);
    values.skin = found->builtin ? LegacySkinForBuiltinId(found->id) : 0;
    values.customAccent = !found->builtin;
    return true;
}

std::string AddCustomThemePreset(const std::string& rawName, ImVec4 baseDark, ImVec4 accent) {
    if (values.customThemes.size() >= kMaxCustomThemePresets)
        return {};
    std::string name = TrimName(rawName);
    if (name.empty()) {
        char fallback[32]{};
        std::snprintf(fallback, sizeof(fallback), "Custom %zu", values.customThemes.size() + 1);
        name = fallback;
    }
    const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::string id = "custom-" + std::to_string(static_cast<unsigned long long>(nowUs));
    int suffix = 1;
    while (FindThemePreset(id) != nullptr) {
        id = "custom-" + std::to_string(static_cast<unsigned long long>(nowUs)) + "-" + std::to_string(suffix++);
    }
    ThemePreset created{};
    created.id = id;
    created.name = std::move(name);
    created.baseDark = ClampOpaqueColor(baseDark);
    created.accent = ClampOpaqueColor(accent);
    created.builtin = false;
    values.customThemes.push_back(created);
    ApplyThemePreset(id);
    return id;
}

bool DeleteCustomThemePreset(const std::string& id) {
    const auto it = std::find_if(values.customThemes.begin(), values.customThemes.end(),
        [&](const ThemePreset& p) { return !p.builtin && p.id == id; });
    if (it == values.customThemes.end())
        return false;
    values.customThemes.erase(it);
    if (values.activeThemeId == id)
        values.activeThemeId.clear();
    return true;
}

bool Loaded() { return loaded; }
const std::string& Error() { return error; }

void ResetForTesting() {
    loaded = false;
    error.clear();
    values = Values{};
    activeVisibilityMode.store(0, std::memory_order_release);
}

int ActiveVisibilityMode() noexcept {
    return activeVisibilityMode.load(std::memory_order_acquire);
}

void SetActiveVisibilityMode(int value) noexcept {
    const int clamped = std::clamp(value, 0, kVisibilityModeCount - 1);
    values.visibilityMode = clamped;
    values.rayBackend = (clamped == static_cast<int>(VisibilityMode::Ray)) ? 1 : 0;
    activeVisibilityMode.store(clamped, std::memory_order_release);
}

int ActiveRayBackend() noexcept {
    return ActiveVisibilityMode() == static_cast<int>(VisibilityMode::Ray) ? 1 : 0;
}

void SetActiveRayBackend(int value) noexcept {
    const int clamped = std::clamp(value, 0, kRayBackendCount - 1);
    if (clamped == 1) {
        SetActiveVisibilityMode(static_cast<int>(VisibilityMode::Ray));
    } else if (ActiveVisibilityMode() == static_cast<int>(VisibilityMode::Ray)) {
        SetActiveVisibilityMode(static_cast<int>(VisibilityMode::Lite));
    } else {
        values.rayBackend = 0;
    }
}

bool Load(int legacyLanguage) {
    if (loaded) return error.empty();
    loaded = true;
    values.language = std::clamp(legacyLanguage, 0, 1);
    ApplyThemePreset("crimson");
    SetActiveVisibilityMode(values.visibilityMode);
    std::ifstream file(Path(), std::ios::binary | std::ios::ate);
    if (!file) return true;
    const auto size = file.tellg();
    if (size < 0 || size > 131072) { error = "Invalid UI preferences file size"; return false; }
    file.seekg(0);
    const std::string data((std::istreambuf_iterator<char>(file)), {});
    rapidjson::Document doc;
    doc.Parse(data.data(), data.size());
    if (doc.HasParseError() || !doc.IsObject()) { error = "Invalid UI preferences JSON"; return false; }
    int fileVersion = kLegacyVersion;
    if (doc.HasMember("version")) {
        if (!doc["version"].IsInt() || doc["version"].GetInt() < kLegacyVersion || doc["version"].GetInt() > kCurrentVersion) {
            error = "Invalid UI preferences version";
            return false;
        }
        fileVersion = doc["version"].GetInt();
    }
    auto number = [&](const char* name, int& value, int low, int high) {
        if (doc.HasMember(name) && doc[name].IsInt()) value = std::clamp(doc[name].GetInt(), low, high);
    };
    auto boolean = [&](const char* name, bool& value) {
        if (doc.HasMember(name) && doc[name].IsBool()) value = doc[name].GetBool();
    };
    number("language", values.language, 0, 1);

    const bool hasVisMode = doc.HasMember("visibilityMode") && doc["visibilityMode"].IsUint();
    const unsigned savedVisMode = hasVisMode ? doc["visibilityMode"].GetUint() : 0u;
    const bool hasRayBackend = doc.HasMember("rayBackend") && doc["rayBackend"].IsUint();
    const unsigned savedRayBackend = hasRayBackend ? doc["rayBackend"].GetUint() : 0u;
    int migratedMode = 0;
    if (MigrateVisibilityMode(fileVersion, hasVisMode, savedVisMode, hasRayBackend, savedRayBackend, migratedMode))
        SetActiveVisibilityMode(migratedMode);
    else
        SetActiveVisibilityMode(values.visibilityMode);

    number("skin", values.skin, 0, 2);
    number("windowWidth", values.windowWidth, 720, 3840);
    number("windowHeight", values.windowHeight, 480, 2160);
    boolean("reducedMotion", values.reducedMotion);
    boolean("loginGlass", values.loginGlass);
    boolean("consoleLayers", values.consoleLayers);
    boolean("sidebarCollapsed", values.sidebarCollapsed);
    boolean("customAccent", values.customAccent);

    // Custom theme presets (v3+).
    values.customThemes.clear();
    if (doc.HasMember("customThemes") && doc["customThemes"].IsArray()) {
        for (const auto& entry : doc["customThemes"].GetArray()) {
            if (values.customThemes.size() >= kMaxCustomThemePresets)
                break;
            if (!entry.IsObject())
                continue;
            if (!entry.HasMember("id") || !entry["id"].IsString() ||
                !entry.HasMember("name") || !entry["name"].IsString())
                continue;
            const std::string id = entry["id"].GetString();
            const std::string name = TrimName(entry["name"].GetString());
            if (id.empty() || name.empty() || FindThemePreset(id) != nullptr)
                continue;
            ThemePreset preset{};
            preset.id = id;
            preset.name = name;
            preset.builtin = false;
            if (!ReadColorArray(entry, "baseDark", preset.baseDark, false) ||
                !ReadColorArray(entry, "accent", preset.accent, false))
                continue;
            values.customThemes.push_back(std::move(preset));
        }
    }

    // Seed baseDark/accent from legacy skin or activeThemeId, then apply explicit overrides.
    const bool storedCustomAccent = values.customAccent;
    ApplyThemePreset(BuiltinIdForLegacySkin(values.skin));
    if (doc.HasMember("activeThemeId") && doc["activeThemeId"].IsString()) {
        const std::string storedId = doc["activeThemeId"].GetString();
        if (storedId.empty()) {
            values.activeThemeId.clear();
        } else if (!ApplyThemePreset(storedId)) {
            values.activeThemeId.clear();
        }
    }
    const bool readBaseDark = ReadColorArray(doc, "baseDark", values.baseDark, false);
    const bool readAccent = ReadColorArray(doc, "accent", values.accent, true);
    if (fileVersion < kCurrentVersion) {
        if (storedCustomAccent && readAccent) {
            values.customAccent = true;
            values.activeThemeId.clear();
        } else {
            ApplyThemePreset(BuiltinIdForLegacySkin(values.skin));
        }
    } else if (values.activeThemeId.empty()) {
        values.customAccent = true;
    } else if (const ThemePreset* active = FindThemePreset(values.activeThemeId)) {
        if (!readBaseDark) values.baseDark = active->baseDark;
        if (!readAccent) values.accent = active->accent;
    }
    values.baseDark = ClampOpaqueColor(values.baseDark);
    values.accent = ClampOpaqueColor(values.accent);
    return true;
}

bool Save() {
    if (!loaded) return true;
    // Preserve a malformed original for recovery instead of overwriting it with defaults.
    if (!error.empty() && error.rfind("Invalid UI preferences", 0) == 0) return false;
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    w.StartObject();
    w.Key("version"); w.Int(kCurrentVersion);
    w.Key("language"); w.Int(std::clamp(values.language, 0, 1));
    const int mode = std::clamp(values.visibilityMode, 0, kVisibilityModeCount - 1);
    w.Key("visibilityMode"); w.Int(mode);
    w.Key("rayBackend"); w.Int(mode == static_cast<int>(VisibilityMode::Ray) ? 1 : 0);
    w.Key("skin"); w.Int(std::clamp(values.skin, 0, 2));
    w.Key("activeThemeId"); w.String(values.activeThemeId.c_str(), static_cast<rapidjson::SizeType>(values.activeThemeId.size()));
    w.Key("reducedMotion"); w.Bool(values.reducedMotion);
    w.Key("loginGlass"); w.Bool(values.loginGlass);
    w.Key("consoleLayers"); w.Bool(values.consoleLayers);
    w.Key("sidebarCollapsed"); w.Bool(values.sidebarCollapsed);
    w.Key("windowWidth"); w.Int(values.windowWidth);
    w.Key("windowHeight"); w.Int(values.windowHeight);
    w.Key("customAccent"); w.Bool(values.customAccent);
    WriteColorArray(w, "baseDark", values.baseDark);
    WriteColorArray(w, "accent", values.accent);
    w.Key("customThemes");
    w.StartArray();
    for (const auto& preset : values.customThemes) {
        if (preset.builtin || preset.id.empty() || preset.name.empty())
            continue;
        w.StartObject();
        w.Key("id"); w.String(preset.id.c_str(), static_cast<rapidjson::SizeType>(preset.id.size()));
        w.Key("name"); w.String(preset.name.c_str(), static_cast<rapidjson::SizeType>(preset.name.size()));
        WriteColorArray(w, "baseDark", preset.baseDark);
        WriteColorArray(w, "accent", preset.accent);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    const bool ok = V2::ConfigStorage::WriteAtomicBytes(Path(), buffer.GetString(), buffer.GetSize());
    error = ok ? "" : V2::ConfigStorage::PersistenceError();
    return ok;
}
}
