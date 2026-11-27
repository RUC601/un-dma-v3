#include "ui/Search.hpp"
#include <Windows.h>
#include <shellapi.h>

#include "Features/UI.hpp"
#include "ui/WorkspaceShell.hpp"
#include "ui/SlotSelector.hpp"
#include "ui/Text.hpp"
#include "ui/Theme.hpp"
#include "ui/ColorPicker.hpp"
#include "ui/vision/ContourPanel.hpp"
#include "vision/ContourService.hpp"
#include "runtime/LauncherBridge.hpp"
#include "config/UiPreferences.hpp"
#include "ui/EditorSupport.hpp"
using UI::Text::UiText;
using UI::Text::kUiLanguageOptions;
#include "output/DeviceRegistry.hpp"
#include "game/profiles/ProfileState.hpp"
#include "Features/UiLabels.hpp"
#include "Features/UiValueFormat.hpp"
using namespace UI::ValueFormat;
#include "output/OutputRuntime.hpp"
#include "output/AutomaticDeviceSelection.hpp"
#include "Utils/Config.hpp"
#include "Game/HeroSkills.hpp"
#include "Game/HeroSettings.hpp"
#include "Game/Offsets.hpp"
#include "Game/Overwatch.hpp"
#include "Game/Target.hpp"
#include "Game/Structs.hpp"
#include "Game/WeaponSpec.hpp"
#include "Renderer/Overlay.hpp"
#include "Renderer/Renderer.hpp"
#include "Utils/Diagnostics.hpp"
#include "Utils/InputLabels.hpp"
#include "Utils/ProcessConnection.hpp"
#include "resource.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void ApplySelectedTypePreset();
void DrawRuntimeReadinessDetails();
void DrawHeroKeyPage();

// =====================================================================
// LOCAL UI STATE
// =====================================================================
namespace UI::EditorCommands {





    struct FirePatternRecorderEvent {
        long long timeMs = 0;
        int previousMask = 0;
        int mask = 0;
    };

    constexpr int kFirePatternRecorderMaxMs = 8000;
    std::atomic<bool> g_firePatternRecorderRunning{ false };
    std::jthread g_firePatternRecorderThread;
    std::mutex g_firePatternRecorderMutex;
    std::string g_firePatternRecorderStatus = "Idle";
    bool g_firePatternRecorderInterrupted = false;

    enum class RecorderSampleResult {
        Ready,
        ConnectionChanged,
        SampleUnavailable
    };

    int MouseButtonMask(bool left, bool right)
    {
        return (left ? 0x01 : 0) | (right ? 0x02 : 0);
    }

    RecorderSampleResult ReadRecorderMouseButtons(uint64_t connectionEpoch, int& mask)
    {
        const auto sameConnection = [=] {
            return connectionEpoch != 0 && OW::ProcessConnection::IsConnected() &&
                OW::ProcessConnection::ConnectionEpoch() == connectionEpoch;
        };
        if (!sameConnection())
            return RecorderSampleResult::ConnectionChanged;

        // Both buttons must belong to the same complete, fresh DMA bitmap.
        std::lock_guard<std::mutex> lock(KeyState::keyStateMutex);
        if (!KeyState::HasFreshSample(GetTickCount64()))
            return RecorderSampleResult::SampleUnavailable;
        const auto left = KeyState::SampleVkInBitmap(KeyState::keyStateBitmap, VK_LBUTTON);
        const auto right = KeyState::SampleVkInBitmap(KeyState::keyStateBitmap, VK_RBUTTON);
        if (!left.valid || !left.available || !right.valid || !right.available)
            return RecorderSampleResult::SampleUnavailable;
        if (!sameConnection())
            return RecorderSampleResult::ConnectionChanged;
        mask = MouseButtonMask(left.down, right.down);
        return RecorderSampleResult::Ready;
    }

    const char* RecorderDmaSourceName()
    {
        return "dma_keystate";
    }

    std::string MaskChangeText(int previousMask, int mask)
    {
        const int changed = previousMask ^ mask;
        std::string text;
        auto append = [&](const char* item) {
            if (!text.empty())
                text += "+";
            text += item;
        };

        if (changed & 0x01)
            append((mask & 0x01) ? "L_down" : "L_up");
        if (changed & 0x02)
            append((mask & 0x02) ? "R_down" : "R_up");
        if (text.empty())
            text = "none";
        return text;
    }

    void SetFirePatternRecorderStatus(const std::string& status, bool interrupted = false)
    {
        std::lock_guard<std::mutex> lock(g_firePatternRecorderMutex);
        g_firePatternRecorderStatus = status;
        g_firePatternRecorderInterrupted = interrupted;
    }

std::string GetFirePatternRecorderStatus(bool& interrupted) {
        std::lock_guard<std::mutex> lock(g_firePatternRecorderMutex);
        interrupted = g_firePatternRecorderInterrupted;
        return g_firePatternRecorderStatus;
    }

    void LogFirePatternRecorderSummary(const std::vector<FirePatternRecorderEvent>& events,
                                       long long totalMs,
                                       const char* sourceName)
    {
        Diagnostics::Aim("fire_pattern.record stop source=%s events=%zu totalMs=%lld",
            sourceName,
            events.size(),
            totalMs);

        if (events.empty()) {
            Diagnostics::Aim("fire_pattern.record result=no_mouse_edges");
            SetFirePatternRecorderStatus("No events captured");
            return;
        }

        const long long firstEventMs = events.front().timeMs;
        std::string candidate = "fire_pattern.steps_candidate ";
        for (size_t index = 0; index < events.size(); ++index) {
            const FirePatternRecorderEvent& event = events[index];
            const long long nextMs = index + 1 < events.size()
                ? events[index + 1].timeMs
                : totalMs;
            const long long durationMs = (std::max)(0ll, nextMs - event.timeMs);
            const long long cumulativeMs = (std::max)(0ll, nextMs - firstEventMs);
            const std::string action = MaskChangeText(event.previousMask, event.mask);

            Diagnostics::Aim("fire_pattern.raw event=%zu action=%s mask=0x%02X nextDeltaMs=%lld cumulativeMs=%lld",
                index + 1,
                action.c_str(),
                event.mask,
                durationMs,
                cumulativeMs);

            Diagnostics::Aim("fire_pattern.step step=%zu mask=0x%02X durationMs=%lld boundary=%s",
                index + 1,
                event.mask,
                durationMs,
                index + 1 < events.size() ? "next_event" : "stop");

            char stepText[48] = {};
            std::snprintf(stepText, sizeof(stepText), "{0x%02X,%lld}%s",
                event.mask,
                durationMs,
                index + 1 < events.size() ? "," : "");
            candidate += stepText;
        }

        Diagnostics::Aim("%s", candidate.c_str());

        char status[96] = {};
        std::snprintf(status, sizeof(status), "Captured %zu events", events.size());
        SetFirePatternRecorderStatus(status);
    }

    void FirePatternRecorderLoop(std::stop_token stopToken, uint64_t connectionEpoch)
    {
        timeBeginPeriod(1);

        std::vector<FirePatternRecorderEvent> events;
        events.reserve(64);

        const char* sourceName = RecorderDmaSourceName();
        const auto started = std::chrono::steady_clock::now();
        int previousMask = 0;
        RecorderSampleResult sampleResult = ReadRecorderMouseButtons(connectionEpoch, previousMask);

        if (sampleResult == RecorderSampleResult::Ready) {
            Diagnostics::Aim("fire_pattern.record start source=%s epoch=%llu initialMask=0x%02X maxMs=%d",
                sourceName,
                static_cast<unsigned long long>(connectionEpoch),
                previousMask,
                kFirePatternRecorderMaxMs);
        }

        long long elapsedMs = 0;
        while (sampleResult == RecorderSampleResult::Ready && !stopToken.stop_requested()) {
            const auto now = std::chrono::steady_clock::now();
            elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
            if (elapsedMs >= kFirePatternRecorderMaxMs)
                break;

            int mask = 0;
            sampleResult = ReadRecorderMouseButtons(connectionEpoch, mask);
            if (sampleResult != RecorderSampleResult::Ready)
                break;
            if (mask != previousMask) {
                events.push_back({ elapsedMs, previousMask, mask });
                Diagnostics::Aim("fire_pattern.edge tMs=%lld action=%s previousMask=0x%02X mask=0x%02X",
                    elapsedMs,
                    MaskChangeText(previousMask, mask).c_str(),
                    previousMask,
                    mask);
                previousMask = mask;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const auto finished = std::chrono::steady_clock::now();
        elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
        // A stop request may race device recovery; check the final boundary too.
        if (sampleResult == RecorderSampleResult::Ready) {
            int finalMask = 0;
            sampleResult = ReadRecorderMouseButtons(connectionEpoch, finalMask);
        }
        if (sampleResult == RecorderSampleResult::Ready) {
            LogFirePatternRecorderSummary(events, elapsedMs, sourceName);
        } else {
            const bool connectionChanged = sampleResult == RecorderSampleResult::ConnectionChanged;
            Diagnostics::Aim("fire_pattern.record interrupted source=%s reason=%s epoch=%llu currentEpoch=%llu events=%zu totalMs=%lld steps_candidate_discarded=1",
                sourceName,
                connectionChanged ? "connection_changed" : "dma_sample_unavailable",
                static_cast<unsigned long long>(connectionEpoch),
                static_cast<unsigned long long>(OW::ProcessConnection::ConnectionEpoch()),
                events.size(),
                elapsedMs);
            SetFirePatternRecorderStatus(connectionChanged
                ? "Interrupted: connection changed. Partial recording discarded; reconnect and start again."
                : "Interrupted: DMA key sample unavailable. Partial recording discarded; check DMA and start again.", true);
        }
        timeEndPeriod(1);
        g_firePatternRecorderRunning.store(false, std::memory_order_release);
    }

bool IsFirePatternRecorderRunning() {
        return g_firePatternRecorderRunning.load(std::memory_order_acquire);
    }

void StartFirePatternRecorder() {
        if (IsFirePatternRecorderRunning())
            return;

        if (g_firePatternRecorderThread.joinable())
            g_firePatternRecorderThread.join();

        const uint64_t connectionEpoch = OW::ProcessConnection::ConnectionEpoch();
        SetFirePatternRecorderStatus("Recording...");
        g_firePatternRecorderRunning.store(true, std::memory_order_release);
        g_firePatternRecorderThread = std::jthread([connectionEpoch](std::stop_token stopToken) {
            FirePatternRecorderLoop(stopToken, connectionEpoch);
        });
    }

void StopFirePatternRecorder() {
        if (!g_firePatternRecorderThread.joinable())
            return;

        g_firePatternRecorderThread.request_stop();
        g_firePatternRecorderThread.join();
    }

    std::string CurrentDirectoryPath()
    {
        char buffer[MAX_PATH]{};
        DWORD length = GetCurrentDirectoryA(MAX_PATH, buffer);
        if (length == 0)
            return ".";

        if (length < MAX_PATH)
            return std::string(buffer, length);

        std::vector<char> dynamicBuffer(static_cast<size_t>(length) + 1);
        length = GetCurrentDirectoryA(static_cast<DWORD>(dynamicBuffer.size()), dynamicBuffer.data());
        if (length == 0 || static_cast<size_t>(length) >= dynamicBuffer.size())
            return ".";

        return std::string(dynamicBuffer.data(), length);
    }

    std::string JoinPath(const std::string& directory, const char* child)
    {
        if (directory.empty())
            return child;

        const char tail = directory.back();
        if (tail == '\\' || tail == '/')
            return directory + child;

        return directory + "\\" + child;
    }

    bool RegularFileExists(const std::string& path)
    {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

OutputConnectionTestResult TestOutputConnection() {
        const V2::Output::OutputRuntimeAppliedState applied =
            V2::Output::RuntimeController().Applied();
        const int lastError = V2::Output::RuntimeController().LastError();

        if (!V2::Output::RuntimeController().IsOutputGateOpen() ||
            applied.descriptor.backend == V2::Output::OutputRuntimeBackend::None) {
            if (lastError != success) {
                char message[96]{};
                std::snprintf(message, sizeof(message), "Inactive: error %d", lastError);
                return { false, message };
            }
            return { false, "Runtime inactive" };
        }

        const char* backend = V2::Output::OutputDeviceName(applied.descriptor);

        const OutputConnectionState connectionState =
            V2::Output::ActiveRuntimeConnectionState();
        if (connectionState != OutputConnectionState::Connected) {
            char message[96]{};
            std::snprintf(message, sizeof(message), "Active %s: %s",
                backend,
                ToString(connectionState));
            return { false, message };
        }

        char message[96]{};
        std::snprintf(message, sizeof(message), "Connected: %s (gen %llu)",
            backend,
            static_cast<unsigned long long>(applied.generation));
        return { true, message };
    }

OutputConnectionTestResult ApplyOutputRuntimeFromCurrentConfig(
        std::chrono::milliseconds timeout) {
        const int status = V2::Output::ApplyOutputConfiguration(timeout);
        const V2::Output::OutputRuntimeAppliedState applied =
            V2::Output::RuntimeController().Applied();

        Diagnostics::Info(
            "Output UI runtime reconcile completed. status=%d enabled=%d desiredType=%d activeBackend=%d generation=%llu",
            status,
            OW::Config::outputEnabled ? 1 : 0,
            OW::Config::outputDeviceType,
            static_cast<int>(applied.descriptor.backend),
            static_cast<unsigned long long>(applied.generation));
        Diagnostics::Aim(
            "output.ui_reconcile status=%d enabled=%d desiredType=%d activeBackend=%d generation=%llu",
            status,
            OW::Config::outputEnabled ? 1 : 0,
            OW::Config::outputDeviceType,
            static_cast<int>(applied.descriptor.backend),
            static_cast<unsigned long long>(applied.generation));

        if (status != success) {
            char message[96]{};
            std::snprintf(message, sizeof(message), "Apply failed: %d", status);
            return { false, message };
        }
        if (!OW::Config::outputEnabled)
            return { true, "Disabled" };
        if (OW::Config::outputAutoDetect &&
            (OW::Config::outputDeviceType == 0 || OW::Config::outputDeviceType == 3 ||
             OW::Config::outputDeviceType == 4 || OW::Config::outputDeviceType == 5))
            return { true, "Detecting connected devices..." };
        if (!V2::Output::RuntimeController().IsOutputGateOpen() ||
            applied.descriptor.backend == V2::Output::OutputRuntimeBackend::None) {
            return { false, "Runtime inactive" };
        }
        return TestOutputConnection();
    }

void CopyConfigProfileName(char (&destination)[kConfigProfileNameBufferSize], const std::string& name) {
        std::snprintf(destination, kConfigProfileNameBufferSize, "%s", name.c_str());
    }

std::string NormalizeProfileFileName(const char* rawName) {
        std::string name = rawName ? rawName : "";

        const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
        name.erase(name.begin(), std::find_if(name.begin(), name.end(),
            [&](unsigned char ch) { return !isSpace(ch); }));
        name.erase(std::find_if(name.rbegin(), name.rend(),
            [&](unsigned char ch) { return !isSpace(ch); }).base(), name.end());

        const size_t slash = name.find_last_of("\\/");
        if (slash != std::string::npos)
            name = name.substr(slash + 1);

        for (char& ch : name) {
            const unsigned char value = static_cast<unsigned char>(ch);
            if (value < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
                ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
                ch = '_';
            }
        }

        if (name.empty())
            name = "config";

        std::string lowered = name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (lowered.size() < 4 || lowered.substr(lowered.size() - 4) != ".ini")
            name += ".ini";

        return name;
    }

    OW::Config::ConfigLoadResult ReloadConfigProfile()
    {
        const std::string savedName = OW::Config::configFileName;
        const std::string path = OW::Config::ConfigPath();
        const auto loadResult = OW::Config::LoadConfig(path);
        if (loadResult == OW::Config::ConfigLoadResult::Failed) return loadResult;
        OW::Config::configFileName = savedName;
        OW::RefreshScreenSizeFromConfig();
        const OutputConnectionTestResult runtimeResult =
            ApplyOutputRuntimeFromCurrentConfig();
        if (!runtimeResult.ok) {
            Diagnostics::Warn(
                "KMBox profile reconcile failed. profile=%s result=%s",
                savedName.c_str(),
                runtimeResult.message.c_str());
        }
        return loadResult;
    }

bool SelectConfigProfile(const char* name, char (&profileName)[kConfigProfileNameBufferSize],
                             bool saveCurrent) {
        if (saveCurrent && !OW::Config::SaveAllConfig(OW::Config::ConfigPath()))
            return false;
        const std::string previousName = OW::Config::configFileName;
        OW::Config::configFileName = name ? name : "config.ini";
        const auto loadResult = ReloadConfigProfile();
        if (loadResult == OW::Config::ConfigLoadResult::Failed) {
            OW::Config::configFileName = previousName;
            CopyConfigProfileName(profileName, previousName);
            return false;
        }
        CopyConfigProfileName(profileName, OW::Config::configFileName);
        OW::Config::lastConfigProfile = OW::Config::configFileName;
        ApplySelectedTypePreset();
        return loadResult == OW::Config::ConfigLoadResult::Loaded;
    }

std::vector<std::string> EnumerateConfigProfiles() {
        std::vector<std::string> profiles;
        WIN32_FIND_DATAA findData{};
        const std::string searchPath = JoinPath(OW::Config::ConfigDirectoryPath(), "*.ini");
        HANDLE findHandle = FindFirstFileA(searchPath.c_str(), &findData);
        if (findHandle != INVALID_HANDLE_VALUE) {
            do {
                if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                    _stricmp(findData.cFileName, "app-state.ini") != 0)
                    profiles.emplace_back(findData.cFileName);
            } while (FindNextFileA(findHandle, &findData));
            FindClose(findHandle);
        }

        if (std::find(profiles.begin(), profiles.end(), "config.ini") == profiles.end())
            profiles.emplace_back("config.ini");
        if (!OW::Config::configFileName.empty() &&
            std::find(profiles.begin(), profiles.end(), OW::Config::configFileName) == profiles.end())
            profiles.emplace_back(OW::Config::configFileName);

        std::sort(profiles.begin(), profiles.end());
        profiles.erase(std::unique(profiles.begin(), profiles.end()), profiles.end());
        return profiles;
    }

bool CreateConfigProfileFromCurrent(const std::string& profileName) {
        const std::string targetPath = JoinPath(OW::Config::ConfigDirectoryPath(), profileName.c_str());
        return OW::Config::CreateConfigProfileFromCurrent(targetPath);
    }

int FindConfigProfileIndex(const std::vector<std::string>& profiles, const std::string& name) {
        for (int i = 0; i < static_cast<int>(profiles.size()); ++i) {
            if (profiles[static_cast<size_t>(i)] == name)
                return i;
        }
        return 0;
    }

bool PersistLastConfigProfile() {
        return OW::Config::PersistActiveConfigProfile(OW::Config::configFileName);
    }

} // namespace UI::EditorCommands
using namespace UI::EditorCommands;

// =====================================================================
// SELECT OPTION STRINGS (matching React selectOptions)
// =====================================================================
const char* T(UiText id) { return UI::Text::Get(id,OW::Config::uiLanguage==OW::Config::kUiLanguageChineseSimplified); }
const char* L(const char* value) { return UI::Text::Label(value,OW::Config::uiLanguage==OW::Config::kUiLanguageChineseSimplified); }
static const char* HeroScopeName(const HeroOption& hero) {
    return hero.heroId == 0 ? L("Current settings") : L(hero.label);
}

static void DrawProbeState(const char* label, bool available, bool down) {
    const ImVec4 color = !available
        ? ImVec4(0.55f, 0.58f, 0.62f, 1.0f)
        : (down ? ImVec4(0.25f, 1.0f, 0.45f, 1.0f) : ImVec4(0.86f, 0.88f, 0.92f, 1.0f));
    ImGui::TextColored(color, "%s: %s", label,
        !available ? T(UiText::StatusNa) : (down ? T(UiText::StatusDown) : T(UiText::StatusUp)));
}

static void DrawKeyStateSample(const char* label, const KeyState::KeyStateVkSample& sample) {
    const bool available = sample.valid && sample.available;
    const ImVec4 color = !available
        ? ImVec4(0.55f, 0.58f, 0.62f, 1.0f)
        : (sample.down ? ImVec4(0.25f, 1.0f, 0.45f, 1.0f) : ImVec4(0.86f, 0.88f, 0.92f, 1.0f));
    ImGui::TextColored(color, "%s: %s  vk=0x%02X byte[%zu]=0x%02X mask=0x%02X",
        label,
        !available ? T(UiText::StatusNa) : (sample.down ? T(UiText::StatusDown) : T(UiText::StatusUp)),
        sample.valid ? sample.vk : 0,
        sample.byteIndex,
        static_cast<unsigned>(sample.rawByte),
        static_cast<unsigned>(sample.downMask));
}

static void DrawDmaMouseButtonSamples() {
    struct MouseSample {
        const char* label;
        int vk;
    };
    constexpr MouseSample kMouseSamples[] = {
        { "Left Mouse", VK_LBUTTON },
        { "Right Mouse", VK_RBUTTON },
        { "Middle Mouse", VK_MBUTTON },
        { "Mouse 4 / X1", VK_XBUTTON1 },
        { "Mouse 5 / X2", VK_XBUTTON2 },
    };

    ImGui::TextUnformatted(L("DMA mouse VK samples"));
    ImGui::Indent();
    for (const MouseSample& entry : kMouseSamples)
        DrawKeyStateSample(entry.label, KeyState::SampleVk(entry.vk));
    ImGui::Unindent();
}

void DrawAimHotkeyProbe() {
    const int keySetting = OW::Config::View_aim_key();
    const int vk = OW::get_bind_id(keySetting);
    const char* keyLabel = OW::Labels::AimActivationKeyName(keySetting);

    ImGui::Text("%s: %s  vk=0x%02X", T(UiText::HotkeyProbe), keyLabel, vk > 0 ? vk : 0);
    if (vk <= 0) {
        DrawProbeState("DMA KeyState", false, false);
        DrawDmaMouseButtonSamples();
        return;
    }

    DrawKeyStateSample("DMA KeyState", KeyState::SampleVk(vk));
    DrawDmaMouseButtonSamples();
}

static int ClampHeroPresetSlotIndex(int slotIndex) {
    return ImClamp(slotIndex, 0, OW::Config::kMaxHeroPresetSlots - 1);
}



static int& ActiveHeroPresetSlotRef(ActionSlotKind kind) {
    return kind == ActionSlotKind::Aim
        ? UI::state.aimHeroSegActive
        : UI::state.triggerHeroSegActive;
}

int ActiveHeroPresetSlotIndex(ActionSlotKind kind) {
    int& activeSlot = ActiveHeroPresetSlotRef(kind);
    activeSlot = ClampHeroPresetSlotIndex(activeSlot);
    return activeSlot;
}

bool IsHeroActionSlotEnabled(ActionSlotKind kind, uint64_t heroId, int slotIndex) {
    return kind == ActionSlotKind::Aim
        ? OW::Config::IsHeroAimSlotEnabled(heroId, slotIndex)
        : OW::Config::IsHeroTriggerSlotEnabled(heroId, slotIndex);
}

static void SetHeroActionSlotEnabled(ActionSlotKind kind, uint64_t heroId, int slotIndex, bool enabled) {
    if (kind == ActionSlotKind::Aim)
        OW::Config::SetHeroAimSlotEnabled(heroId, slotIndex, enabled);
    else
        OW::Config::SetHeroTriggerSlotEnabled(heroId, slotIndex, enabled);
}

static int GetHeroActionSlotCount(ActionSlotKind kind, uint64_t heroId) {
    return kind == ActionSlotKind::Aim
        ? OW::Config::GetHeroAimSlotCount(heroId)
        : OW::Config::GetHeroTriggerSlotCount(heroId);
}

OW::Config::HeroPreset MakeCurrentHeroActionPreset(ActionSlotKind kind) {
    return kind == ActionSlotKind::Aim
        ? OW::Config::MakeHeroAimPresetFromCurrent()
        : OW::Config::MakeHeroTriggerPresetFromCurrent();
}

static bool HasHeroActionPreset(ActionSlotKind kind, uint64_t heroId) {
    return kind == ActionSlotKind::Aim
        ? OW::Config::HasHeroAimPreset(heroId)
        : OW::Config::HasHeroTriggerPreset(heroId);
}

OW::Config::HeroPreset GetHeroActionPresetOrDefault(ActionSlotKind kind,
                                                           uint64_t heroId,
                                                           int slotIndex) {
    return kind == ActionSlotKind::Aim
        ? OW::Config::GetHeroAimPresetOrDefault(heroId, slotIndex)
        : OW::Config::GetHeroTriggerPresetOrDefault(heroId, slotIndex);
}

void SetHeroActionPreset(ActionSlotKind kind,
                                uint64_t heroId,
                                int slotIndex,
                                const OW::Config::HeroPreset& preset) {
    if (kind == ActionSlotKind::Aim)
        OW::Config::SetHeroAimPreset(heroId, slotIndex, preset);
    else
        OW::Config::SetHeroTriggerPreset(heroId, slotIndex, preset);
}

static int AddHeroActionSlot(ActionSlotKind kind,
                             uint64_t heroId,
                             const OW::Config::HeroPreset& seedPreset) {
    return kind == ActionSlotKind::Aim
        ? OW::Config::AddHeroAimSlot(heroId, seedPreset)
        : OW::Config::AddHeroTriggerSlot(heroId, seedPreset);
}

static bool DeleteHeroActionSlot(ActionSlotKind kind, uint64_t heroId, int slotIndex) {
    return kind == ActionSlotKind::Aim
        ? OW::Config::DeleteHeroAimSlot(heroId, slotIndex)
        : OW::Config::DeleteHeroTriggerSlot(heroId, slotIndex);
}

void ApplyHeroActionPresetToGlobals(ActionSlotKind kind, const OW::Config::HeroPreset& preset) {
    if (kind == ActionSlotKind::Aim)
        OW::Config::ApplyHeroAimPresetToGlobals(preset);
    else
        OW::Config::ApplyHeroTriggerPresetToGlobals(preset);
}

void PushHeroActionSlotControlId(ActionSlotKind kind, uint64_t heroId, int slotIndex) {
    char id[64] = {};
    std::snprintf(id, sizeof(id), "%s:%llX:%d",
                  kind == ActionSlotKind::Aim ? "AimSlot" : "TriggerSlot",
                  static_cast<unsigned long long>(heroId),
                  ClampHeroPresetSlotIndex(slotIndex));
    ImGui::PushID(id);
}

static bool IsConcreteHeroSelection(const HeroOption& hero) {
    return hero.heroId != 0;
}

static void ShowAimSlotSummaryTooltip(bool hasSpecificHero) {
    ImGui::SetItemTooltip("%s",
        hasSpecificHero
            ? T(UiText::TooltipHeroSpecific)
            : T(UiText::TooltipHeroFallback));
}

static void ShowSaveConfigTooltip(bool savesSelectedHero) {
    ImGui::SetItemTooltip("%s",
        savesSelectedHero
            ? T(UiText::TooltipSaveHero)
            : T(UiText::TooltipSaveFallback));
}

// =====================================================================
// GROUP BOX STATE (lazy border drawing)
// =====================================================================
static bool     g_gbOpen      = false;
static char     g_gbTitle[64] = "";
static ImVec2   g_gbStartPos(0, 0);
static float    g_gbWidth     = 0.0f;
static float    g_gbMinHeight = 0.0f;
static float    g_gbBottomGap = 7.0f;
static ImDrawListSplitter g_gbDrawSplitter;
static uint64_t s_lastSyncedDetectedHeroId = 0;
static std::string s_configSaveStatus;
static double s_configSaveStatusUntil = 0.0;
static double s_configSaveStatusStarted = 0.0;
static bool s_configSaveFailed = false;

void SetPersistenceStatus(bool saved, const char* successText) {
    s_configSaveFailed = !saved;
    s_configSaveStatus = saved ? std::string(successText) : OW::Config::LastPersistenceError();
    s_configSaveStatusStarted = ImGui::GetTime();
    s_configSaveStatusUntil = saved ? s_configSaveStatusStarted + 4.0 : 0.0;
}

void UI::AutoSave(bool flush) {
    static uint64_t lastPollMs = 0;
    const uint64_t nowMs = GetTickCount64();
    if (!flush && nowMs - lastPollMs < 100) return;
    lastPollMs = nowMs;
    if (!flush) V2::Output::PollAutomaticDeviceSelection();
    const auto result = OW::Config::PollAutoSave(OW::Config::ConfigPath(), nowMs, flush);
    if (result != OW::Config::AutoSaveResult::Idle && ImGui::GetCurrentContext()) {
        const bool chinese = OW::Config::uiLanguage == OW::Config::kUiLanguageChineseSimplified;
        SetPersistenceStatus(result == OW::Config::AutoSaveResult::Saved,
            chinese ? "已自动保存" : "Changes saved automatically");
    }
}

using namespace UI::Theme;
static ImGuiID s_preNewFrameInitHook = 0;
static ID3D11ShaderResourceView* s_logoTexture = nullptr;
static constexpr int kLogoTextureSize = 32;
static float s_measuredBodyHeightByPage[kMeasuredPageCount] = {};
static UI::MenuClientSize s_desiredMenuClientSize{ kShellWidth, 680.0f };

// Forward declarations

static void InitStyleBeforeNewFrame(ImGuiContext*, ImGuiContextHook*) {
    if (!UI::state.initialized)
        UI::InitStyle();
}

static void EnsurePreNewFrameInitHook() {
    if (s_preNewFrameInitHook != 0)
        return;

    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context)
        return;

    ImGuiContextHook hook;
    hook.Type = ImGuiContextHookType_NewFramePre;
    hook.Callback = InitStyleBeforeNewFrame;
    s_preNewFrameInitHook = ImGui::AddContextHook(context, &hook);
}

float MaxFloat(float a, float b) {
    return (a > b) ? a : b;
}

static float MinFloat(float a, float b) {
    return (a < b) ? a : b;
}

static ImU32 MixColor(ImU32 from, ImU32 to, float t) {
    t = ImClamp(t, 0.0f, 1.0f);
    ImVec4 a = ImGui::ColorConvertU32ToFloat4(from);
    ImVec4 b = ImGui::ColorConvertU32ToFloat4(to);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t
    ));
}

static float VisualTransition(ImGuiID id, bool enabled, float speed = 16.0f) {
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    if (!animations || UI::Preferences::values.reducedMotion) return enabled ? 1.0f : 0.0f;
    ImGuiStorage* storage = ImGui::GetStateStorage();
    float current = storage->GetFloat(id, enabled ? 1.0f : 0.0f);
    float target = enabled ? 1.0f : 0.0f;
    float step = ImClamp(ImGui::GetIO().DeltaTime * speed, 0.0f, 1.0f);
    current += (target - current) * step;
    storage->SetFloat(id, current);
    return current;
}

int ClampHeroSelectionIndex(int index) {
    return ImClamp(index, 0, IM_ARRAYSIZE(kHeroOptions) - 1);
}

const HeroOption& CurrentHeroOption() {
    UI::state.selectedTypeIndex = ClampHeroSelectionIndex(UI::state.selectedTypeIndex);
    return kHeroOptions[UI::state.selectedTypeIndex];
}

static int FindHeroOptionIndexById(uint64_t heroId) {
    if (heroId == 0)
        return 0;

    for (int i = 1; i < IM_ARRAYSIZE(kHeroOptions); ++i) {
        if (kHeroOptions[i].heroId == heroId)
            return i;
    }
    return -1;
}

static const HeroOption* FindHeroOptionById(uint64_t heroId) {
    const int index = FindHeroOptionIndexById(heroId);
    return index >= 0 ? &kHeroOptions[index] : nullptr;
}

static ID3D11ShaderResourceView* HeroAvatarForName(const char* label) {
    if (!label || label[0] == '\0')
        return nullptr;

    IconManager* icons = Render::GetIconManager();
    if (!icons)
        return nullptr;

    const std::string slug = OW::HeroDisplayNameToSlug(label);
    if (slug.empty() || slug == "all")
        return nullptr;

    return icons->LoadHeroAvatar(slug);
}

static ID3D11ShaderResourceView* HeroAvatarForOption(const HeroOption& hero) {
    if (hero.heroId == 0)
        return nullptr;
    return HeroAvatarForName(hero.label);
}

ID3D11ShaderResourceView* HeroSkillIconForDefinition(const OW::HeroSkillDefinition& definition) {
    IconManager* icons = Render::GetIconManager();
    if (!icons || !definition.heroSlug || !definition.iconSlug)
        return nullptr;

    const std::string key = std::string(definition.heroSlug) + "/" + definition.iconSlug;
    if (ID3D11ShaderResourceView* icon = icons->GetIcon(key))
        return icon;

    icons->LoadAbilityIcons(definition.heroSlug, { definition.iconSlug });
    return icons->GetIcon(key);
}

static std::string HeroDisplayNameForId(uint64_t heroId) {
    if (heroId == 0)
        return "Not detected";

    if (const HeroOption* option = FindHeroOptionById(heroId))
        return option->label;

    const OW::c_entity localSnapshot = OW::SnapshotLocalEntity();
    std::string name = OW::GetHeroEngNames(heroId, localSnapshot.LinkBase);
    if (name.empty() || name == "Unknown") {
        char unknown[64] = {};
        std::snprintf(unknown, sizeof(unknown), "Unknown (0x%llX)",
                      static_cast<unsigned long long>(heroId));
        return unknown;
    }
    return name;
}

static std::string SaveSelectedConfig() {
    const HeroOption& selectedHero = CurrentHeroOption();
    const std::string path = OW::Config::ConfigPath();
    const OW::c_entity localSnapshot = OW::SnapshotLocalEntity();
    OW::Config::NormalizeHeroPresets();

    if (IsConcreteHeroSelection(selectedHero)) {
        const std::string heroPath = OW::Config::HeroConfigPath(path);
        if (!OW::Config::SaveConfig(path)) {
            s_configSaveFailed = true;
            return OW::Config::LastPersistenceError();
        }
        if (!OW::Config::SaveConfigForHero(path, selectedHero.heroId, localSnapshot.LinkBase)) {
            s_configSaveFailed = true;
            return std::string(OW::Config::uiLanguage == OW::Config::kUiLanguageChineseSimplified
                ? "系统设置已保存，当前英雄保存失败：" : "System saved; current hero save failed: ") +
                OW::Config::LastPersistenceError();
        }
        s_configSaveFailed = false;
        std::string logStatus = "Saved ";
        logStatus += selectedHero.label;
        logStatus += " config";
        Diagnostics::Info("%s to %s.", logStatus.c_str(), heroPath.c_str());
        std::string status = T(UiText::SavedPrefix);
        status += selectedHero.label;
        status += T(UiText::ConfigSuffix);
        return status;
    }

    if (!OW::Config::SaveAllConfig(path)) {
        s_configSaveFailed = true;
        return OW::Config::LastPersistenceError();
    }
    s_configSaveFailed = false;
    const uint64_t savedHeroId = OW::Config::lastheroid > 0
        ? static_cast<uint64_t>(OW::Config::lastheroid)
        : localSnapshot.HeroID;
    const std::string savedName = savedHeroId != 0
        ? HeroDisplayNameForId(savedHeroId)
        : std::string("global");
    std::string logStatus = "Saved ";
    logStatus += savedName;
    logStatus += " config";
    Diagnostics::Info("%s to %s.", logStatus.c_str(), path.c_str());
    std::string status = T(UiText::SavedPrefix);
    status += savedHeroId != 0 ? savedName : T(UiText::GlobalConfig);
    status += T(UiText::ConfigSuffix);
    return status;
}

static uint64_t DetectedLocalHeroId() {
    const Diagnostics::StatusSnapshot snapshot = Diagnostics::Snapshot();
    return snapshot.localEntity.selectedHeroId;
}

void ApplySelectedTypePreset() {
    const HeroOption& hero = CurrentHeroOption();
    if (hero.heroId == 0)
        return;

    const int aimSlotIndex = ActiveHeroPresetSlotIndex(ActionSlotKind::Aim);
    if (IsHeroActionSlotEnabled(ActionSlotKind::Aim, hero.heroId, aimSlotIndex)) {
        const OW::Config::HeroPreset preset =
            GetHeroActionPresetOrDefault(ActionSlotKind::Aim, hero.heroId, aimSlotIndex);
        ApplyHeroActionPresetToGlobals(ActionSlotKind::Aim, preset);
    }

    const int triggerSlotIndex = ActiveHeroPresetSlotIndex(ActionSlotKind::Trigger);
    if (IsHeroActionSlotEnabled(ActionSlotKind::Trigger, hero.heroId, triggerSlotIndex)) {
        const OW::Config::HeroPreset preset =
            GetHeroActionPresetOrDefault(ActionSlotKind::Trigger, hero.heroId, triggerSlotIndex);
        ApplyHeroActionPresetToGlobals(ActionSlotKind::Trigger, preset);
    }
}

static bool SelectTypeIndex(int index) {
    index = ClampHeroSelectionIndex(index);
    if (UI::state.selectedTypeIndex == index)
        return false;

    UI::state.selectedTypeIndex = index;
    ApplySelectedTypePreset();
    return true;
}

static void SyncSelectedTypeWithDetectedHero() {
    if (!UI::state.followDetectedHero) return;
    const uint64_t detectedHeroId = DetectedLocalHeroId();
    if (detectedHeroId == 0) {
        s_lastSyncedDetectedHeroId = 0;
        return;
    }

    if (detectedHeroId == s_lastSyncedDetectedHeroId)
        return;

    s_lastSyncedDetectedHeroId = detectedHeroId;
    const int detectedIndex = FindHeroOptionIndexById(detectedHeroId);
    if (detectedIndex > 0)
        SelectTypeIndex(detectedIndex);
}

static bool SaveSelectedTypePreset() {
    const HeroOption& hero = CurrentHeroOption();
    if (hero.heroId == 0)
        return false;

    const int slotIndex = ActiveHeroPresetSlotIndex(ActionSlotKind::Aim);
    const OW::Config::HeroPreset preset = MakeCurrentHeroActionPreset(ActionSlotKind::Aim);
    SetHeroActionPreset(ActionSlotKind::Aim, hero.heroId, slotIndex, preset);
    return OW::Config::SaveHeroConfigForHero(OW::Config::ConfigPath(), hero.heroId);
}

static bool LoadSelectedTypePreset() {
    const HeroOption& hero = CurrentHeroOption();
    if (hero.heroId == 0)
        return false;

    ActiveHeroPresetSlotIndex(ActionSlotKind::Aim);
    ActiveHeroPresetSlotIndex(ActionSlotKind::Trigger);
    OW::Config::LoadHeroConfig(OW::Config::ConfigPath());
    ApplySelectedTypePreset();
    return true;
}

static std::string SkeletonBoneMaskPreview(OW::SkeletonBoneMask rawMask);

static const char* PresetAimBehaviorName(int behavior) {
    behavior = ImClamp(behavior, 0, IM_ARRAYSIZE(kAimBehavior) - 1);
    return kAimBehavior[behavior];
}

static void DrawPresetSummary(const HeroOption& hero,
                              const OW::Config::HeroPreset& preset,
                              bool hasStoredPreset,
                              ActionSlotKind kind) {
    char summary[320] = {};
    const char* scope = hero.heroId == 0
        ? "Global defaults"
        : (hasStoredPreset ? "Stored preset" : "Using global defaults");
    if (kind == ActionSlotKind::Aim) {
        const std::string skeleton = SkeletonBoneMaskPreview(preset.aimBoneMask);
        std::snprintf(summary, sizeof(summary),
                      "%s - %s | %s | Speed %.1f%% | FOV %.0f deg | %s | Hitbox %.0f%%",
                      hero.label, scope, PresetAimBehaviorName(preset.aimBehavior),
                      preset.smooth, preset.fov,
                      skeleton.c_str(), preset.hitbox);
    } else {
        std::snprintf(summary, sizeof(summary),
                      "%s - %s | Trigger %s | %s | %s | Hitbox %.0f%% | %s",
                      hero.label, scope,
                      preset.trigger.enabled ? "On" : "Off",
                      OW::AttackActionNameForHero(hero.heroId, preset.trigger.action),
                      OW::Labels::TriggerbotModeName(preset.trigger.mode),
                      preset.hitbox,
                      OW::Labels::AimModeName(preset.aimMode));
    }
    ImGui::TextUnformatted(summary);
}

// Shared avatar browser for the editing hero and target-hero sets.
static bool HeroBrowserTile(const HeroOption& hero, const ImVec2& size,
                            bool selected, const char* assignedTo = nullptr, const bool* alertEnabled = nullptr) {
    ImGui::BeginDisabled(assignedTo != nullptr);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(HeroScopeName(hero), size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    draw->AddRectFilled(pos, end, ImGui::GetColorU32(selected ? kColAccentDark : hovered ? kColControlHover : kColControl), kControlRounding);
    draw->AddRect(pos, end, ImGui::GetColorU32(selected ? kColAccent : kColStroke), kControlRounding, 0, selected ? 2.0f : 1.0f);
    const float iconSize = 42.0f;
    const ImVec2 iconMin(pos.x + (size.x - iconSize) * .5f, pos.y + 7.0f);
    if (auto* avatar = HeroAvatarForOption(hero))
        draw->AddImage(reinterpret_cast<ImTextureID>(avatar), iconMin,
            ImVec2(iconMin.x + iconSize, iconMin.y + iconSize), ImVec2(0,0), ImVec2(1,1),
            alertEnabled && !*alertEnabled ? ImGui::GetColorU32(ImVec4(.72f,.72f,.72f,.43f)) : ImGui::GetColorU32(ImGuiCol_Text));
    else
        draw->AddText(ImVec2(iconMin.x + 12, iconMin.y + 10), ImGui::GetColorU32(kColText), hero.heroId ? "?" : L("All"));
    const auto textSize = ImGui::CalcTextSize(HeroScopeName(hero), nullptr, false, size.x - 8.0f);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
        ImVec2(pos.x + (std::max)(4.0f, (size.x - textSize.x) * .5f), pos.y + 55.0f),
        ImGui::GetColorU32(kColText), HeroScopeName(hero), nullptr, size.x - 8.0f);
    if (selected)
        ImGui::RenderCheckMark(draw, ImVec2(end.x - 17, pos.y + 5), ImGui::GetColorU32(kColText), 11.0f);
    ImGui::RenderNavCursor(ImRect(pos, end), ImGui::GetItemID());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused()) {
        if (assignedTo) ImGui::SetTooltip("%s\n%s: %s", HeroScopeName(hero), L("Assigned to"), assignedTo);
        else if (alertEnabled) ImGui::SetTooltip("%s\n%s", HeroScopeName(hero), L(*alertEnabled ? "On" : "Off"));
        else ImGui::SetTooltip("%s", HeroScopeName(hero));
    }
    ImGui::EndDisabled();
    return pressed;
}

static bool HeroBrowserGrid(const char* search, bool includeCurrent, float footerHeight,
                            const std::function<bool(const HeroOption&)>& selected,
                            const std::function<const char*(const HeroOption&)>& assigned,
                            const std::function<void(int)>& activate,
                            const std::function<bool(uint64_t)>& alertEnabled = {}) {
    bool changed = false;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
    const float itemW = (std::max)(82.0f, ImGui::GetFontSize() * 5.5f);
    float nameHeight = ImGui::GetTextLineHeight();
    for (const auto& hero : kHeroOptions)
        nameHeight = (std::max)(nameHeight, ImGui::CalcTextSize(HeroScopeName(hero), nullptr, false, itemW - 8).y);
    const float itemH = 63 + nameHeight;
    if (ImGui::BeginChild("##heroBrowserGrid", ImVec2(0, -footerHeight), true)) {
        const int columns = (std::max)(1, int((ImGui::GetContentRegionAvail().x + 8) / (itemW + 8)));
        bool any = false;
        for (const char* category : {"Global", "Tank", "Damage", "Support"}) {
            bool heading = false;
            int column = 0;
            for (int i = 0; i < IM_ARRAYSIZE(kHeroOptions); ++i) {
                const auto& hero = kHeroOptions[i];
                if ((!includeCurrent && !hero.heroId) || std::strcmp(hero.category, category)) continue;
                if (*search && !ImStristr(hero.label, nullptr, search, nullptr) &&
                    !ImStristr(UI::Text::Label(hero.label, true), nullptr, search, nullptr) &&
                    !ImStristr(HeroScopeName(hero), nullptr, search, nullptr)) continue;
                if (!heading) { ImGui::TextDisabled("%s", L(category)); heading = true; }
                if (column) ImGui::SameLine();
                ImGui::PushID(i);
                const bool enabled = alertEnabled && alertEnabled(hero.heroId);
                if (HeroBrowserTile(hero, ImVec2(itemW, itemH), selected(hero), assigned(hero), alertEnabled ? &enabled : nullptr)) {
                    activate(i);
                    changed = true;
                }
                ImGui::PopID();
                column = (column + 1) % columns;
                any = true;
            }
            if (heading) ImGui::Spacing();
        }
        if (!any) ImGui::TextWrapped("%s", L("No matching heroes. Clear the search to see all heroes."));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    return changed;
}

static void HeroBrowserSize() {
    const auto size = ImGui::GetMainViewport()->Size;
    const float scale = (std::max)(1.0f, ImGui::GetFontSize() / 16.0f);
    ImGui::SetNextWindowSize(ImVec2((std::min)(590.0f * scale, size.x - 24),
                                   (std::min)(520.0f * scale, size.y - 24)), ImGuiCond_Always);
}

static void HeroBrowserSearch(char (&search)[128]) {
    if (ImGui::IsWindowAppearing()) { search[0] = 0; ImGui::SetKeyboardFocusHere(); }
    ImGui::TextUnformatted(L("Search heroes"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##heroSearch", search, sizeof(search));
}

bool UIAlertHeroSelect(const char* id, uint64_t& heroId, const std::function<bool(uint64_t)>& enabled) {
    const bool zh = OW::Config::uiLanguage == OW::Config::kUiLanguageChineseSimplified;
    const auto* hero = FindHeroOptionById(heroId);
    auto* avatar = hero ? HeroAvatarForOption(*hero) : nullptr;
    SettingRow(zh ? "目标英雄" : "Target hero");
    if (avatar) {
        const float size = ControlHeight();
        ImGui::Image(reinterpret_cast<ImTextureID>(avatar), ImVec2(size, size));
        ImGui::SameLine(0, 8);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(hero ? HeroScopeName(*hero) : (zh ? "全部英雄" : "All heroes"));
    ImGui::SameLine(0, 10);
    const auto label = std::string(zh ? "显示所有英雄###" : "Show all heroes###") + id;
    if (ImGui::Button(label.c_str(), ImVec2(0, ControlHeight())))
        ImGui::OpenPopup("##alertHeroBrowser");
    SettingTooltip(zh ? "亮色：已开启预警规则　淡色：未开启" : "Bright: alerts enabled   Dim: alerts off");
    HeroBrowserSize();
    if (!ImGui::BeginPopup("##alertHeroBrowser")) return false;
    static char search[128]{};
    HeroBrowserSearch(search);
    const bool changed = HeroBrowserGrid(search, false, ControlHeight() + 8,
        [&](const HeroOption& item) { return item.heroId == heroId; },
        [](const HeroOption&) -> const char* { return nullptr; },
        [&](int index) { heroId = kHeroOptions[index].heroId; ImGui::CloseCurrentPopup(); },
        enabled);
    if (ImGui::Button(L("Done"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return changed;
}

static bool TypePickerPanel(int selectedIndex, bool includeCurrent,
                            const std::function<void(int)>& selectHero) {
    HeroBrowserSize();
    if (!ImGui::BeginPopup("TypePickerPopup")) return false;
    static char search[128]{};
    ImGui::TextUnformatted(L("Browse all heroes"));
    HeroBrowserSearch(search);
    const bool changed = HeroBrowserGrid(search, includeCurrent, ControlHeight() + 8,
        [&](const HeroOption& hero) { return kHeroOptions[selectedIndex].heroId == hero.heroId; },
        [](const HeroOption&) -> const char* { return nullptr; },
        [&](int i) { selectHero(i); ImGui::CloseCurrentPopup(); });
    if (ImGui::Button(L("Done"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return changed;
}

bool UIHeroMultiSelect(const char* id, std::vector<uint64_t>& heroes,
                       const OW::AimPlan::Plan* plan, uint64_t ownGroup) {
    std::string summary;
    for (const auto& hero : kHeroOptions) if (OW::AimPlan::Contains(heroes, hero.heroId)) {
        if (!summary.empty()) summary += ", ";
        summary += L(hero.label);
    }
    char count[96]{};
    std::snprintf(count, sizeof(count), L("Selected heroes: %d"), int(heroes.size()));
    const std::string preview = heroes.empty() ? L("Choose target heroes") : std::string(count) + "  " + summary;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, ControlHeight());
    const bool open = ImGui::Button(id, size);
    const ImGuiID itemId = ImGui::GetItemID();
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(pos.x+8,pos.y+5),
        ImVec2(pos.x+size.x-26,pos.y+size.y), pos.x+size.x-26, preview.c_str(), nullptr, nullptr);
    ImGui::RenderArrow(ImGui::GetWindowDrawList(), ImVec2(pos.x+size.x-20,pos.y+6), ImGui::GetColorU32(ImGuiCol_Text), ImGuiDir_Down, .8f);
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) ImGui::SetTooltip("%s\n%s", L("Browse all heroes"), summary.c_str());
    ImGuiContext& g = *ImGui::GetCurrentContext();
    IMGUI_TEST_ENGINE_ITEM_INFO(itemId,id,g.LastItemData.StatusFlags);
    ImGui::PushID(id);
    if (open) ImGui::OpenPopup("##targetHeroBrowser");
    HeroBrowserSize();
    bool changed = false;
    if (ImGui::BeginPopup("##targetHeroBrowser")) {
        static char search[128]{};
        ImGui::TextUnformatted(L("Browse all heroes"));
        HeroBrowserSearch(search);
        if (plan) SettingTooltip(L("Unavailable heroes already belong to another exception."));
        changed = HeroBrowserGrid(search, false, ControlHeight() + 8,
            [&](const HeroOption& hero) { return OW::AimPlan::Contains(heroes, hero.heroId); },
            [&](const HeroOption& hero) -> const char* {
                if (plan) for (const auto& group : plan->groups)
                    if (group.id != ownGroup && OW::AimPlan::Contains(group.heroes, hero.heroId)) return group.name.c_str();
                return nullptr;
            },
            [&](int i) {
                const auto hero = kHeroOptions[i].heroId;
                if (OW::AimPlan::Contains(heroes, hero)) std::erase(heroes, hero);
                else heroes.push_back(hero);
            });
        if (ImGui::Button(L("Done"))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::BeginDisabled(heroes.empty());
        if (ImGui::Button(L("Clear selection"))) { heroes.clear(); changed = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text(L("Selected heroes: %d"), int(heroes.size()));
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

static bool CreateTextureFromIconResource(ID3D11Device* device, int resourceId, int size,
                                          ID3D11ShaderResourceView** outTexture) {
    if (!device || !outTexture || size <= 0)
        return false;

    *outTexture = nullptr;

    HICON icon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resourceId),
                   IMAGE_ICON, size, size, LR_DEFAULTCOLOR)
    );
    if (!icon)
        return false;

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = size;
    bitmapInfo.bmiHeader.biHeight = -size;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = screenDc ? CreateCompatibleDC(screenDc) : nullptr;
    void* bgraBits = nullptr;
    HBITMAP bitmap = screenDc
        ? CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bgraBits, nullptr, 0)
        : nullptr;
    if (screenDc)
        ReleaseDC(nullptr, screenDc);

    if (!memoryDc || !bitmap || !bgraBits) {
        if (bitmap)
            DeleteObject(bitmap);
        if (memoryDc)
            DeleteDC(memoryDc);
        DestroyIcon(icon);
        return false;
    }

    const size_t pixelBytes = static_cast<size_t>(size) * static_cast<size_t>(size) * 4;
    std::memset(bgraBits, 0, pixelBytes);

    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    const BOOL drewIcon = DrawIconEx(memoryDc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    if (oldBitmap)
        SelectObject(memoryDc, oldBitmap);

    if (!drewIcon) {
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        DestroyIcon(icon);
        return false;
    }

    std::vector<std::uint8_t> rgba(pixelBytes);
    const auto* bgra = static_cast<const std::uint8_t*>(bgraBits);
    bool hasAlpha = false;
    for (int i = 0; i < size * size; ++i) {
        rgba[i * 4 + 0] = bgra[i * 4 + 2];
        rgba[i * 4 + 1] = bgra[i * 4 + 1];
        rgba[i * 4 + 2] = bgra[i * 4 + 0];
        rgba[i * 4 + 3] = bgra[i * 4 + 3];
        hasAlpha = hasAlpha || rgba[i * 4 + 3] != 0;
    }

    if (!hasAlpha) {
        for (int i = 0; i < size * size; ++i) {
            if (rgba[i * 4 + 0] || rgba[i * 4 + 1] || rgba[i * 4 + 2])
                rgba[i * 4 + 3] = 0xFF;
        }
    }

    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    DestroyIcon(icon);

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = static_cast<UINT>(size);
    textureDesc.Height = static_cast<UINT>(size);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = rgba.data();
    initData.SysMemPitch = static_cast<UINT>(size * 4);

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = device->CreateTexture2D(&textureDesc, &initData, &texture);
    if (FAILED(hr) || !texture)
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    hr = device->CreateShaderResourceView(texture, &srvDesc, &srv);
    texture->Release();

    if (FAILED(hr) || !srv)
        return false;

    *outTexture = srv;
    return true;
}

// =====================================================================
// FORMAT SLIDER VALUE  (mirrors React formatSliderText)
// =====================================================================
static float StepFromDecimalPlaces(int decimals) {
    decimals = ImClamp(decimals, 0, 4);

    float step = 1.0f;
    for (int i = 0; i < decimals; ++i)
        step *= 0.1f;
    return step;
}

static float SliderKeyboardSlowStep(float, float, const Spec& format, bool integralValue) {
    return integralValue ? 1.0f : StepFromDecimalPlaces(format.decimals);
}

static float RoundToSliderStep(float value, float step) {
    if (step <= 0.0f)
        return value;
    return std::round(value / step) * step;
}

// =====================================================================
// UIGroupBox  --  Opens a group box.  Closes any previously open group
//                 box (drawing its border lazily via CloseGroupBox).
// =====================================================================
bool UIGroupBox(const char* title, float minHeightPx, float bottomGapPx) {
    if (g_gbOpen)
        CloseGroupBox();
    UI::Search::Anchor(title);
    title = L(title);

    g_gbOpen = true;
    g_gbMinHeight = minHeightPx;
    g_gbBottomGap = bottomGapPx;
    std::strncpy(g_gbTitle, title, sizeof(g_gbTitle) - 1);
    g_gbTitle[sizeof(g_gbTitle) - 1] = '\0';

    if (s_boldFont)
        ImGui::PushFont(s_boldFont);
    ImVec2 textSize = ImGui::CalcTextSize(title);
    if (s_boldFont)
        ImGui::PopFont();

    ImVec2 pos = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    g_gbDrawSplitter.Split(dl, 2);
    g_gbDrawSplitter.SetCurrentChannel(dl, 1);

    ImGui::Dummy(ImVec2(0.0f, textSize.y + 8.0f));

    g_gbStartPos = pos;
    g_gbWidth    = ImGui::GetContentRegionAvail().x;

    ImGui::Indent(kGroupContentIndent);
    ImGui::BeginGroup();
    return true;
}

// =====================================================================
// CloseGroupBox  --  Draws the border and legend of the current group
//                    box and restores indentation.
// =====================================================================
struct GroupBoxFrame {
    ImVec2 min;
    ImVec2 max;
    std::string title;
};

static void DrawGroupBoxFrame(ImDrawList* draw, const GroupBoxFrame& frame) {
    draw->AddRectFilled(frame.min, frame.max, kColPanelSoft, kGroupRounding);
    draw->AddRect(frame.min, frame.max, kColStroke, kGroupRounding);
    if (s_boldFont)
        ImGui::PushFont(s_boldFont);
    const ImVec2 textMin(frame.min.x + kGroupContentIndent, frame.min.y + 4.0f);
    const ImVec2 textMax(frame.max.x - kControlRightPadding, textMin.y + ImGui::GetFontSize());
    ImGui::RenderTextEllipsis(draw, textMin, textMax, textMax.x, frame.title.c_str(), nullptr, nullptr);
    if (s_boldFont)
        ImGui::PopFont();
}

static GroupBoxFrame FinishGroupBox(bool drawFrame) {
    if (g_gbMinHeight > 0.0f) {
        const float desiredContentEndY = g_gbStartPos.y + g_gbMinHeight - 9.0f;
        const float currentY = ImGui::GetCursorScreenPos().y;
        if (currentY < desiredContentEndY)
            ImGui::Dummy(ImVec2(0.0f, desiredContentEndY - currentY));
    }

    ImGui::EndGroup();
    ImGui::Unindent(kGroupContentIndent);

    const GroupBoxFrame frame{
        g_gbStartPos,
        ImVec2(MaxFloat(g_gbStartPos.x + 1.0f, g_gbStartPos.x + g_gbWidth - kGroupBorderClipInset),
               MaxFloat(ImGui::GetItemRectMax().y + 9.0f, g_gbStartPos.y + g_gbMinHeight)),
        g_gbTitle
    };
    if (drawFrame) {
        g_gbDrawSplitter.SetCurrentChannel(ImGui::GetWindowDrawList(), 0);
        DrawGroupBoxFrame(ImGui::GetWindowDrawList(), frame);
    }

    const float cursorY = ImGui::GetCursorScreenPos().y;
    if (cursorY < frame.max.y + g_gbBottomGap)
        ImGui::Dummy(ImVec2(0.0f, frame.max.y + g_gbBottomGap - cursorY));

    g_gbDrawSplitter.Merge(ImGui::GetWindowDrawList());
    g_gbOpen = false;
    g_gbMinHeight = 0.0f;
    return frame;
}

void CloseGroupBox() {
    if (g_gbOpen)
        FinishGroupBox(true);
}

// =====================================================================
// UICheckbox  --  compact custom checkbox with premium dark-state styling.
// =====================================================================
static float g_settingRowCursorY = -1000.0f;

bool UICheckbox(const char* label, bool* value) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g    = *GImGui;
    const ImGuiID id   = window->GetID(label);
    const float sz = ToggleWidth();
    const float height = ControlHeight();
    const float spacing = g.Style.ItemInnerSpacing.x + 2.0f;
    const bool inSettingRow = std::abs(window->DC.CursorPos.y - g_settingRowCursorY) < 2.0f;
    if (inSettingRow)
        g_settingRowCursorY = -1000.0f;

    const char* displayLabel = label[0] == '#' ? label : L(label);
    const char* labelEnd = ImGui::FindRenderedTextEnd(displayLabel);
    const bool hasVisibleLabel = !inSettingRow && (labelEnd > displayLabel);
    const ImVec2 labelSize = hasVisibleLabel
        ? ImGui::CalcTextSize(displayLabel, labelEnd, false)
        : ImVec2(0.0f, 0.0f);
    ImVec2 pos = window->DC.CursorPos;

    ImRect bb(pos, ImVec2(pos.x + sz + (hasVisibleLabel ? spacing + labelSize.x : 0.0f), pos.y + height));

    ImGui::ItemSize(bb, g.Style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id)) return false;

    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed)
        *value = !*value;
    ImGui::RenderNavCursor(bb, id);
    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags);

    float hoverT = VisualTransition(id ^ 0x23a7, hovered || held, 18.0f);
    float checkedT = VisualTransition(id ^ 0x51d3, *value, 18.0f);
    const float trackHeight=16.f;
    ImVec2 boxMin(bb.Min.x,bb.Min.y+(height-trackHeight)*.5f);
    ImVec2 boxMax(boxMin.x+sz,boxMin.y+trackHeight);
    window->DrawList->AddRectFilled(boxMin,boxMax,MixColor(kColControlHot,kColAccent,checkedT),trackHeight*.5f);
    const float knobX=boxMin.x+8+(sz-16)*checkedT;
    window->DrawList->AddCircleFilled(ImVec2(knobX,boxMin.y+8),5.5f,MixColor(kColTextDim,kColText,checkedT),20);

    if (hasVisibleLabel) {
        ImVec2 labelPos(bb.Min.x + sz + spacing, bb.Min.y + (height - labelSize.y) * 0.5f);
        ImU32 labelCol = MixColor(kColTextMuted, kColText, MaxFloat(hoverT, checkedT * 0.5f));
        window->DrawList->AddText(labelPos, labelCol, displayLabel, labelEnd);
    }

    return pressed;
}

// =====================================================================
// UISlider  --  Custom dark track with accent fill and value text.
// =====================================================================
void PushControlWidth(float rightPaddingPx) {
    const float width = MaxFloat(1.0f, ImGui::GetContentRegionAvail().x - rightPaddingPx);
    ImGui::PushItemWidth(width);
}

bool UISlider(const char* label, float* value, float v_min, float v_max,
                     const Spec& format, bool integralValue) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    const ImGuiID id   = window->GetID(label);
    const float height = ControlHeight();

    ImVec2 pos  = window->DC.CursorPos;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    float width = ImGui::CalcItemWidth();
    if (width <= 0.0f || width > availableWidth)
        width = availableWidth;
    width = MaxFloat(1.0f, width);
    ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id, nullptr, ImGuiItemFlags_Inputable)) return false;

    // Normalise value to [0,1]
    float range  = (v_max - v_min);
    float v_norm = (range > 0.0f) ? (*value - v_min) / range : 0.0f;
    v_norm = ImClamp(v_norm, 0.0f, 1.0f);

    // Interaction
    ImGuiContext& g = *GImGui;
    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held,
                                         ImGuiButtonFlags_MouseButtonLeft |
                                         ImGuiButtonFlags_PressedOnClick);
    bool valueChanged = false;

    auto assignValue = [&](float next) {
        next = ImClamp(next, v_min, v_max);
        if (integralValue)
            next = static_cast<float>(ImClamp(static_cast<int>(std::lround(next)),
                                              static_cast<int>(std::lround(v_min)),
                                              static_cast<int>(std::lround(v_max))));
        if (std::fabs(*value - next) <= 0.000001f)
            return;

        *value = next;
        valueChanged = true;
    };

    auto setFromNorm = [&](float normalized) {
        normalized = ImClamp(normalized, 0.0f, 1.0f);
        assignValue(v_min + normalized * range);
    };

    const bool mousePressed = pressed && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool mouseDragging = held && g.ActiveId == id && ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (mousePressed) {
        ImGui::SetActiveID(id, window);
        ImGui::SetFocusID(id, window);
        ImGui::FocusWindow(window);
    }

    if (mousePressed || mouseDragging) {
        const ImVec2 mousePos = ImGui::GetMousePos();
        setFromNorm((mousePos.x - bb.Min.x) / (bb.Max.x - bb.Min.x));
    }

    const bool keyboardTarget = (ImGui::IsItemFocused() || hovered || g.ActiveId == id) &&
                                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (keyboardTarget && !ImGui::GetIO().WantTextInput) {
        ImGui::SetKeyOwner(ImGuiKey_LeftArrow, id);
        ImGui::SetKeyOwner(ImGuiKey_RightArrow, id);
        ImGui::SetKeyOwner(ImGuiKey_UpArrow, id);
        ImGui::SetKeyOwner(ImGuiKey_DownArrow, id);
        ImGui::SetKeyOwner(ImGuiKey_Home, id);
        ImGui::SetKeyOwner(ImGuiKey_End, id);

        const float slowStep = SliderKeyboardSlowStep(v_min, v_max, format, integralValue);
        const float fastStep = slowStep * 10.0f;
        auto nudgeValue = [&](float delta) {
            const float next = ImClamp(*value + delta, v_min, v_max);
            assignValue(RoundToSliderStep(next, slowStep));
        };

        const ImGuiInputFlags repeat = ImGuiInputFlags_Repeat;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, repeat, id)) {
            nudgeValue(slowStep);
        } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, repeat, id)) {
            nudgeValue(-slowStep);
        } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, repeat, id)) {
            nudgeValue(fastStep);
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, repeat, id)) {
            nudgeValue(-fastStep);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Home, repeat, id)) {
            assignValue(v_min);
        } else if (ImGui::IsKeyPressed(ImGuiKey_End, repeat, id)) {
            assignValue(v_max);
        }
    }

    v_norm = (range > 0.0f) ? (*value - v_min) / range : 0.0f;
    v_norm = ImClamp(v_norm, 0.0f, 1.0f);

    float hoverT = VisualTransition(id ^ 0x6f47, hovered || held || ImGui::IsItemFocused(), 18.0f);
    ImU32 trackCol = MixColor(kColControl, kColControlHover, hoverT);
    window->DrawList->AddRectFilled(bb.Min, bb.Max, IM_COL32(0x00, 0x00, 0x00, 0x28), kControlRounding);
    window->DrawList->AddRectFilled(ImVec2(bb.Min.x, bb.Min.y + 1.0f),
                                    bb.Max, trackCol, kControlRounding);
    window->DrawList->AddRect(bb.Min, bb.Max,
                              MixColor(kColStrokeDark, kColStroke, hoverT),
                              kControlRounding, 0, 1.0f);
    window->DrawList->AddLine(ImVec2(bb.Min.x + 5.0f, bb.Min.y + 1.0f),
                              ImVec2(bb.Max.x - 5.0f, bb.Min.y + 1.0f),
                              IM_COL32(0xff, 0xff, 0xff, 0x12), 1.0f);

    float fillW = v_norm * (bb.Max.x - bb.Min.x);
    if (fillW > 0.0f) {
        ImVec2 fillMax(bb.Min.x + fillW, bb.Max.y);
        window->DrawList->AddRectFilled(bb.Min, fillMax, kColAccent, kControlRounding);
    }

    // Value text
    char tmp[64];
    const char* displayText = L(Format(tmp, sizeof(tmp), *value, format));
    ImVec2 textSize = ImGui::CalcTextSize(displayText);
    ImVec2 textPos(bb.Max.x - textSize.x - 8.0f, bb.Min.y + (height - textSize.y) * 0.5f);
    window->DrawList->AddText(
        ImVec2(textPos.x + 1.0f, textPos.y + 1.0f),
        IM_COL32(0x00, 0x00, 0x00, 0x80), displayText);
    window->DrawList->AddText(textPos, kColText, displayText);

    if (valueChanged)
        ImGui::MarkItemEdited(id);

    ImGui::RenderNavCursor(bb, id);
    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags);
    return valueChanged;
}

bool UISlider(const char* label, int* value, float v_min, float v_max,
                     const Spec& format) {
    const int minValue = static_cast<int>(std::lround(v_min));
    const int maxValue = static_cast<int>(std::lround(v_max));
    const int clampedValue = ImClamp(*value, minValue, maxValue);
    float sliderValue = static_cast<float>(clampedValue);

    const bool sliderChanged = UISlider(label, &sliderValue, v_min, v_max, format, true);
    const int roundedValue = ImClamp(static_cast<int>(std::lround(sliderValue)), minValue, maxValue);
    if (roundedValue != *value) {
        *value = roundedValue;
        return true;
    }
    return sliderChanged;
}

// =====================================================================
// UISelect  --  Custom-styled dropdown using ImGui BeginCombo.
//               Dropdown items get a red #e41143 background on hover/active.
// =====================================================================
static bool BeginControlCombo(const char* label, const char* preview) {
    const auto& style = ImGui::GetStyle();
    const float paddingY = (ControlHeight() - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, paddingY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kControlRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    [[maybe_unused]] const ImGuiID id = ImGui::GetID(label);
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool open = ImGui::BeginCombo(label, preview, ImGuiComboFlags_HeightLargest);
    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags);
    ImGui::PopStyleVar(3);
    return open;
}

bool UISelect(const char* label, int* current, const char* const items[], int itemCount, bool translateItems) {
    if (*current < 0 || *current >= itemCount)
        *current = 0;

    const char* preview = translateItems ? L(items[*current]) : items[*current];
    bool changed = false;
    if (BeginControlCombo(label, preview)) {
        for (int i = 0; i < itemCount; i++) {
            const bool isSelected = (*current == i);
            std::string optionLabel = translateItems ? L(items[i]) : items[i];
            optionLabel += "###option";
            ImGui::PushID(i);
            if (ImGui::Selectable(optionLabel.c_str(), isSelected)) {
                *current = i;
                changed = true;
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    return changed;
}

namespace {

ImGuiID g_keyBindingCaptureId = 0;
bool g_keyBindingCaptureArmed = false;

std::string VirtualKeyLabel(int vk) {
    if (vk == 0)
        return L("None");
    if (vk == VK_LBUTTON) return L("Left Mouse");
    if (vk == VK_RBUTTON) return L("Right Mouse");
    if (vk == VK_MBUTTON) return L("Middle Mouse");
    if (vk == VK_XBUTTON1) return L("Mouse 4");
    if (vk == VK_XBUTTON2) return L("Mouse 5");

    wchar_t name[64]{};
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (scan && GetKeyNameTextW(static_cast<LONG>(scan) << 16, name, 64)) {
        char utf8[128]{};
        if (WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8,
                                static_cast<int>(sizeof(utf8)), nullptr, nullptr))
            return utf8;
    }

    char fallback[32]{};
    std::snprintf(fallback, sizeof(fallback), "VK 0x%02X", vk);
    return fallback;
}

std::string KeyBindingLabel(int value, UI::KeyBindingStorage storage) {
    if (storage == UI::KeyBindingStorage::AimActivationIndex) {
        if (!OW::Labels::IsCustomAimActivationKey(value))
            return L(OW::Labels::AimActivationKeyName(value));
        return VirtualKeyLabel(OW::Labels::AimActivationKeyVk(value));
    }
    return VirtualKeyLabel(value);
}

bool MouseButtonDown() {
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
}

} // namespace

bool UI::UIKeyBinding(const char* id,
                      int* value,
                      KeyBindingStorage storage,
                      bool allowSideButtons) {
    if (!value)
        return false;

    const bool disabled = (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    const ImGuiID bindingId = ImGui::GetID(id);
    const bool capturing = g_keyBindingCaptureId == bindingId;
    const std::string bindingLabel = capturing
        ? std::string(L("Press a key... Esc cancels"))
        : KeyBindingLabel(*value, storage);
    bool changed = false;

    const float width = MaxFloat(1.0f, (std::min)(ImGui::CalcItemWidth(), ImGui::GetContentRegionAvail().x));
    const std::string buttonLabel = bindingLabel + "###" + id;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kControlRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    if (ImGui::Button(buttonLabel.c_str(), ImVec2(width, ControlHeight()))) {
        g_keyBindingCaptureId = bindingId;
        g_keyBindingCaptureArmed = false;
    }
    ImGui::PopStyleVar(2);
    if (ImGui::CalcTextSize(bindingLabel.c_str()).x + 2.0f * ImGui::GetStyle().FramePadding.x > width)
        ImGui::SetItemTooltip("%s", bindingLabel.c_str());

    if (!disabled && g_keyBindingCaptureId == bindingId) {
        if (!g_keyBindingCaptureArmed) {
            if (!MouseButtonDown())
                g_keyBindingCaptureArmed = true;
        } else if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            UIKeyBindingCancelCapture();
        } else {
            for (int vk = 1; vk < 256; ++vk) {
                if (!allowSideButtons && (vk == VK_XBUTTON1 || vk == VK_XBUTTON2))
                    continue;
                if (!V2::HeroSettings::SupportedKey(vk) ||
                    !(GetAsyncKeyState(vk) & 0x8000))
                    continue;

                const int captured = storage == KeyBindingStorage::AimActivationIndex
                    ? OW::Labels::AimActivationKeyIndexFromVk(vk) : vk;
                if (*value != captured) {
                    *value = captured;
                    changed = true;
                }
                UIKeyBindingCancelCapture();
                break;
            }
        }
    }
    return changed;
}

void UI::UIKeyBindingCancelCapture() {
    g_keyBindingCaptureId = 0;
    g_keyBindingCaptureArmed = false;
}

static std::string SkeletonBoneMaskPreview(OW::SkeletonBoneMask rawMask) {
    const OW::SkeletonBoneMask mask = OW::NormalizeSkeletonBoneMask(rawMask);
    if (OW::IsAllSkeletonBonesSelected(mask))
        return L("All");

    std::string preview;
    int selectedCount = 0;
    for (const OW::SkeletonBoneGroupDefinition& group : OW::kSkeletonBoneGroups) {
        if (!OW::SkeletonBoneMaskIncludesGroup(mask, group.mask))
            continue;
        if (!preview.empty())
            preview += " + ";
        preview += L(group.label);
        ++selectedCount;
    }
    if (selectedCount == 1 && OW::Config::uiLanguage != OW::Config::kUiLanguageChineseSimplified)
        preview += " only";
    return preview.empty() ? L("Head only") : preview;
}

bool UISkeletonBoneMultiSelect(const char* label, OW::SkeletonBoneMask* rawMask) {
    if (!rawMask)
        return false;

    *rawMask = OW::NormalizeSkeletonBoneMask(*rawMask);
    const std::string preview = SkeletonBoneMaskPreview(*rawMask);
    bool changed = false;
    if (BeginControlCombo(label, preview.c_str())) {
        const bool allSelected = OW::IsAllSkeletonBonesSelected(*rawMask);
        if (ImGui::Selectable(L("All"), allSelected, ImGuiSelectableFlags_NoAutoClosePopups) &&
            !allSelected) {
            *rawMask = OW::kSkeletonBoneAllMask;
            changed = true;
        }
        if (allSelected)
            ImGui::SetItemDefaultFocus();

        ImGui::Separator();
        for (const OW::SkeletonBoneGroupDefinition& group : OW::kSkeletonBoneGroups) {
            const bool selected = OW::SkeletonBoneMaskIncludesGroup(*rawMask, group.mask);
            if (ImGui::Selectable(L(group.label), selected, ImGuiSelectableFlags_NoAutoClosePopups)) {
                const OW::SkeletonBoneMask next = OW::ToggleSkeletonBoneGroup(*rawMask, group.mask);
                if (next != *rawMask) {
                    *rawMask = next;
                    changed = true;
                }
            }
        }
        ImGui::EndCombo();
    }

    return changed;
}

int NextAimMethodPresetId() {
    int nextId = 1;
    for (const OW::Config::AimMethodPreset& preset : OW::Config::Read_aimMethodPresets())
        nextId = (std::max)(nextId, preset.id + 1);
    return nextId;
}

static int NextAimBehaviorPresetId() {
    auto& next=OW::Config::View_nextAimBehaviorPresetId();
    for(const auto& p:OW::Config::Read_aimBehaviorPresets()) next=(std::max)(next,p.id+1);
    return next++;
}

int NextDynamicFovPresetId() {
    int nextId = 1;
    for (const OW::Config::DynamicFovPreset& preset : OW::Config::Read_dynamicFovPresets())
        nextId = (std::max)(nextId, preset.id + 1);
    return nextId;
}

std::string NormalizeUiPresetName(const char* text, const char* fallback, int index) {
    std::string value = text ? text : "";
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    if (value.empty()) {
        value = fallback ? fallback : "Preset";
        if (index >= 0) {
            value += " ";
            value += std::to_string(index + 1);
        }
    }
    if (value.size() > 63)
        value.resize(63);
    return value;
}

void CopyPresetNameToBuffer(const std::string& name, char (&buffer)[64]) {
    std::snprintf(buffer, sizeof(buffer), "%s", name.c_str());
}

OW::Config::AimMethodPreset CaptureMethodPresetFromCurrent(int method, const char* name) {
    const int normalizedMethod = OW::Config::ClampAimMethodIndex(method);
    OW::Config::AimMethodPreset preset{};
    preset.id = NextAimMethodPresetId();
    preset.name = NormalizeUiPresetName(name, "Method Preset", static_cast<int>(OW::Config::View_aimMethodPresets().size()));
    preset.method = normalizedMethod;
    preset.angularSpeedScale = OW::Config::ClampAimMethodAngularSpeedScalePercent(
        OW::Config::aimMethodAngularSpeedScale[static_cast<size_t>(normalizedMethod)]);
    preset.pidP = std::clamp(OW::Config::View_aimPidP(), 0.0f, 2.0f);
    preset.pidI = std::clamp(OW::Config::View_aimPidI(), 0.0f, 0.5f);
    preset.pidD = std::clamp(OW::Config::View_aimPidD(), 0.0f, 1.0f);
    preset.pidMaxIntegral = std::clamp(OW::Config::View_aimPidMaxIntegral(), 1.0f, 50.0f);
    preset.pidDeadzone = std::clamp(OW::Config::View_aimPidDeadzone(), 0.0f, 10.0f);
    preset.bezierControlPoints = std::clamp(OW::Config::View_aimBezierControlPoints(), 2, 6);
    preset.bezierCurvature = std::clamp(OW::Config::View_aimBezierCurvature(), 0.0f, 1.0f);
    preset.bezierSpeed = std::clamp(OW::Config::View_aimBezierSpeed(), 1.0f, 200.0f);
    preset.piecewiseNearDegrees = OW::Config::AimPiecewiseNearDegrees();
    preset.piecewiseMidDegrees = OW::Config::AimPiecewiseMidDegrees();
    preset.piecewiseFarDegrees = OW::Config::AimPiecewiseFarDegrees();
    preset.piecewiseNearScale = OW::Config::AimPiecewiseNearScale();
    preset.piecewiseMidScale = OW::Config::AimPiecewiseMidScale();
    preset.piecewiseFarScale = OW::Config::AimPiecewiseFarScale();
    preset.accelLimitedAcceleration = OW::Config::AimMethodAcceleration(4);
    preset.constantAngularSpeedDeg = OW::Config::AimConstantAngularSpeedDeg();
    preset.hybridConstantSpeedDeg = OW::Config::aimHybridConstantSpeedDeg;
    preset.hybridMaxSpeedDeg = OW::Config::aimHybridMaxSpeedDeg;
    preset.hybridAccelerationDeg = OW::Config::aimHybridAccelerationDeg;
    preset.hybridDecelerationDeg = OW::Config::aimHybridDecelerationDeg;
    preset.hybridNearRadiusDeg = OW::Config::aimHybridNearRadiusDeg;
    preset.hybridTargetMotionGain = OW::Config::aimHybridTargetMotionGain;
    preset.hybridSuddenMotionBoost = OW::Config::aimHybridSuddenMotionBoost;
    preset.hybridDeadzoneDeg = OW::Config::aimHybridDeadzoneDeg;
    return preset;
}

OW::Config::AimBehaviorPreset CaptureBehaviorPresetFromCurrent(int behavior,
                                                                      int method,
                                                                      int methodPresetId,
                                                                      float baseSpeed,
                                                                      const OW::AimStartLimiterProfile& startLimiter,
                                                                      bool moveSplitEnabled,
                                                                      int moveSplitMaxPixels,
                                                                      int moveSplitDelayUs,
                                                                      const char* name) {
    OW::Config::AimBehaviorPreset preset{};
    preset.id = NextAimBehaviorPresetId();
    preset.name = NormalizeUiPresetName(name, "Behavior Preset", static_cast<int>(OW::Config::View_aimBehaviorPresets().size()));
    preset.behavior = OW::Config::ClampAimBehaviorIndex(behavior);
    preset.method = OW::Config::ClampAimMethodIndex(method);
    preset.methodPresetId = OW::Config::ClampAimMethodPresetId(methodPresetId);
    if (const OW::Config::AimMethodPreset* methodPreset = OW::Config::FindAimMethodPreset(preset.methodPresetId))
        preset.method = OW::Config::ClampAimMethodIndex(methodPreset->method);
    preset.baseSpeed = std::isfinite(baseSpeed) ? std::clamp(baseSpeed, 0.0f, 100.0f) : 100.0f;
    preset.startLimiter = OW::ValidateAimStartLimiterProfile(startLimiter);
    preset.moveSplitEnabled = moveSplitEnabled;
    preset.moveSplitMaxPixels = OW::Config::ClampMoveSplitMaxPixels(moveSplitMaxPixels);
    preset.moveSplitDelayUs = OW::Config::ClampMoveSplitDelayUs(moveSplitDelayUs);
    preset.motion=OW::Config::CaptureBehaviorMotion(behavior);preset.motion.method=preset.method;
    return preset;
}

static const char* AimBehaviorPresetPreview(int presetId, const char* noneText) {
    const OW::Config::AimBehaviorPreset* preset = OW::Config::FindAimBehaviorPreset(presetId);
    return preset ? preset->name.c_str() : noneText;
}

static const char* AimMethodPresetPreview(int presetId, const char* noneText) {
    const OW::Config::AimMethodPreset* preset = OW::Config::FindAimMethodPreset(presetId);
    return preset ? preset->name.c_str() : noneText;
}

static const char* DynamicFovPresetPreview(int presetId, const char* noneText) {
    const OW::Config::DynamicFovPreset* preset = OW::Config::FindDynamicFovPreset(presetId);
    return preset ? preset->name.c_str() : noneText;
}

bool UISelectAimMethodPreset(const char* label, int* presetId, int filterMethod) {
    if (!OW::Config::FindAimMethodPreset(*presetId))
        *presetId = -1;

    bool changed = false;
    const char* preview = AimMethodPresetPreview(*presetId, L("Current Method Config"));
    if (BeginControlCombo(label, preview)) {
        const bool noneSelected = *presetId < 0;
        if (ImGui::Selectable(L("Current Method Config"), noneSelected)) {
            *presetId = -1;
            changed = true;
        }
        if (noneSelected)
            ImGui::SetItemDefaultFocus();

        const int normalizedFilter = filterMethod >= 0 ? OW::Config::ClampAimMethodIndex(filterMethod) : -1;
        for (const OW::Config::AimMethodPreset& preset : OW::Config::Read_aimMethodPresets()) {
            const int method = OW::Config::ClampAimMethodIndex(preset.method);
            if (normalizedFilter >= 0 && method != normalizedFilter)
                continue;

            std::string labelText = preset.name;
            labelText += " / ";
            labelText += L(kAimMethod[method]);
            const bool selected = *presetId == preset.id;
            if (ImGui::Selectable(labelText.c_str(), selected)) {
                *presetId = preset.id;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool UISelectAimBehaviorPreset(const char* label, int* presetId, int filterBehavior, const char* noneText) {
    bool changed = false;
    if (!OW::Config::FindAimBehaviorPreset(*presetId)) {
        const int fallback = noneText ? -1 : OW::Config::DefaultAimBehaviorPresetId();
        changed = *presetId != fallback;
        *presetId = fallback;
    }
    const char* preview = AimBehaviorPresetPreview(*presetId, noneText ? L(noneText) : "");
    if (BeginControlCombo(label, preview)) {
        const bool noneSelected = *presetId < 0;
        if (noneText && ImGui::Selectable(L(noneText), noneSelected)) {
            *presetId = -1;
            changed = true;
        }
        if (noneSelected)
            ImGui::SetItemDefaultFocus();

        const int normalizedFilter = filterBehavior >= 0 ? OW::Config::ClampAimBehaviorIndex(filterBehavior) : -1;
        for (const OW::Config::AimBehaviorPreset& preset : OW::Config::Read_aimBehaviorPresets()) {
            const int behavior = OW::Config::ClampAimBehaviorIndex(preset.behavior);
            if (normalizedFilter >= 0 && behavior != normalizedFilter)
                continue;

            std::string labelText = preset.name;
            labelText += " / ";
            labelText += L(kAimBehavior[behavior]);
            if (const OW::Config::AimMethodPreset* methodPreset = OW::Config::FindAimMethodPreset(preset.methodPresetId)) {
                labelText += " / ";
                labelText += methodPreset->name;
            }
            else {labelText += " / ";labelText += L(kAimMethod[OW::Config::ClampAimMethodIndex(preset.method)]);}
            const bool selected = *presetId == preset.id;
            if (ImGui::Selectable(labelText.c_str(), selected)) {
                *presetId = preset.id;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool UISelectDynamicFovPreset(const char* label, int* presetId, bool allowNone) {
    if (!OW::Config::FindDynamicFovPreset(*presetId))
        *presetId = -1;

    bool changed = false;
    const char* preview = DynamicFovPresetPreview(*presetId, allowNone ? L("Fixed FOV") : L("Select Preset"));
    if (BeginControlCombo(label, preview)) {
        if (allowNone) {
            const bool noneSelected = *presetId < 0;
            if (ImGui::Selectable(L("Fixed FOV"), noneSelected)) {
                *presetId = -1;
                changed = true;
            }
            if (noneSelected)
                ImGui::SetItemDefaultFocus();
        }

        for (const OW::Config::DynamicFovPreset& preset : OW::Config::Read_dynamicFovPresets()) {
            const OW::Config::DynamicFovPreset normalized =
                OW::Config::ValidateDynamicFovPresetValue(preset);
            char labelText[160] = {};
            const float nearFov = OW::Config::EvaluateDynamicFovPreset(normalized, 0.0f, OW::Config::kDefaultFovDeg);
            const float farFov = OW::Config::EvaluateDynamicFovPreset(
                normalized,
                normalized.points[static_cast<size_t>(normalized.pointCount - 1)].distanceM,
                OW::Config::kDefaultFovDeg);
            std::snprintf(labelText,
                          sizeof(labelText),
                          "%s / %.0f->%.0f deg",
                          normalized.name.c_str(),
                          nearFov,
                          farFov);
            const bool selected = *presetId == preset.id;
            if (ImGui::Selectable(labelText, selected)) {
                *presetId = preset.id;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void ApplyAimBehaviorToHeroPreset(OW::Config::HeroPreset& preset, int behavior) {
    preset.aimBehavior = OW::Config::ClampAimBehaviorIndex(behavior);
    preset.aimMode = OW::Config::IsTrackingBehavior(preset.aimBehavior) ? 0 : 1;
    if (preset.firePolicy == 0 || preset.firePolicy == 1 || preset.firePolicy == 2) {
        preset.firePolicy = static_cast<int>(OW::DefaultFirePolicyForBehavior(
            OW::ClampAimBehavior(preset.aimBehavior)));
        preset.keepFiring = preset.firePolicy == static_cast<int>(OW::FirePolicyType::KeepFiring) ||
            preset.firePolicy == static_cast<int>(OW::FirePolicyType::TrackingWithHold);
        preset.autoshot = preset.firePolicy == static_cast<int>(OW::FirePolicyType::SingleShot);
    }
}

void UIDisabledSelect(const char* label, const char* previewText) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    if (!previewText)
        previewText = T(UiText::NotOpened);

    const ImGuiID id = window->GetID(label);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    float width = ImGui::CalcItemWidth();
    if (width <= 0.0f || width > availableWidth)
        width = availableWidth;
    width = MaxFloat(1.0f, width);
    const float height = ControlHeight();
    ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return;

    const ImU32 frameCol = MixColor(kColPanelSoft, kColControl, 0.45f);
    window->DrawList->AddRectFilled(bb.Min, bb.Max, IM_COL32(0x00, 0x00, 0x00, 0x2E), kControlRounding);
    window->DrawList->AddRectFilled(ImVec2(bb.Min.x, bb.Min.y + 1.0f), bb.Max,
                                    frameCol, kControlRounding);
    window->DrawList->AddRect(bb.Min, bb.Max,
                              MixColor(kColStrokeDark, kColStroke, 0.25f),
                              kControlRounding, 0, 1.0f);

    const ImVec2 textSize = ImGui::CalcTextSize(previewText);
    const ImVec2 textPos(bb.Min.x + 8.0f, bb.Min.y + (height - textSize.y) * 0.5f);
    ImGui::PushClipRect(bb.Min, bb.Max, true);
    window->DrawList->AddText(textPos, kColTextDim, previewText);
    ImGui::PopClipRect();

    ImGuiContext& g = *ImGui::GetCurrentContext();
    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", previewText);
}

static std::vector<int> AttackActionsForHero(uint64_t heroId) {
    std::vector<int> actions;

    auto addAction = [&](int action) {
        if (action < 0 || action >= OW::Labels::AttackActionCount())
            return;
        if (std::find(actions.begin(), actions.end(), action) == actions.end())
            actions.push_back(action);
    };

    if (heroId != 0) {
        for (const OW::WeaponSpec* spec = OW::WeaponSpecsBegin(); spec != OW::WeaponSpecsEnd(); ++spec) {
            if (spec->heroId == heroId)
                addAction(spec->action);
        }
    }

    if (actions.empty()) {
        const int actionCount = OW::Labels::AttackActionCount();
        for (int action = 0; action < actionCount; ++action)
            actions.push_back(action);
    }

    return actions;
}

bool UIAttackActionSelect(const char* label, int* action, uint64_t heroId, int scopeFilter) {
    std::vector<int> actionValues = AttackActionsForHero(heroId);
    if (scopeFilter >= 0) {
        std::erase_if(actionValues, [&](int value) {
            return static_cast<int>(OW::Config::DefaultAimScopeRequirementForHeroAction(heroId, value)) != scopeFilter;
        });
    }
    if (actionValues.empty()) {
        UIDisabledSelect(label, L("None"));
        return false;
    }

    bool changed = false;
    int currentIndex = 0;
    const auto current = std::find(actionValues.begin(), actionValues.end(), *action);
    if (current != actionValues.end()) {
        currentIndex = static_cast<int>(current - actionValues.begin());
    } else {
        *action = actionValues.front();
        changed = true;
    }

    if (scopeFilter >= 0 && actionValues.size() == 1) {
        UIDisabledSelect(label, L(OW::AttackActionNameForHero(heroId, *action)));
        return changed;
    }

    std::vector<const char*> actionLabels;
    actionLabels.reserve(actionValues.size());
    for (int value : actionValues)
        actionLabels.push_back(OW::AttackActionNameForHero(heroId, value));

    if (UISelect(label, &currentIndex, actionLabels.data(), static_cast<int>(actionLabels.size()))) {
        *action = actionValues[static_cast<size_t>(currentIndex)];
        changed = true;
    }

    return changed;
}

bool UIPredictionAlgorithmSelect(const char* label, int* algorithm) {
    const char* const labels[] = { L("Legacy Linear"), L("Ballistic Intercept") };
    int selection = OW::ClampPredictionAlgorithm(*algorithm);
    SettingRow(L("Prediction algorithm"));
    PushControlWidth();
    UISelect(label, &selection, labels, IM_ARRAYSIZE(labels));
    const bool changed = *algorithm != selection;
    *algorithm = selection;
    ImGui::SetItemTooltip("%s", L("Both algorithms use the same weapon and projectile data."));
    ImGui::PopItemWidth();
    return changed;
}

// =====================================================================
// UISegmented  --  Row of buttons, active one gets accent background.
//                  Returns the new active index (or old if unchanged).
// =====================================================================
static int UISegmented(const char* items[], int itemCount, int active) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();

    float width  = ImGui::GetContentRegionAvail().x;
    float height = (itemCount <= 5) ? 21.0f : 17.0f;

    ImVec2 pos = window->DC.CursorPos;

    window->DrawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                                    kColControl, kControlRounding);
    window->DrawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
                              kColStrokeDark, kControlRounding, 0, 1.0f);

    float segW = width / itemCount;
    int   result = active;

    for (int i = 0; i < itemCount; i++) {
        ImVec2 segMin(pos.x + i * segW, pos.y);
        ImVec2 segMax(pos.x + (i + 1) * segW, pos.y + height);

        ImGui::SetCursorScreenPos(segMin);
        ImGui::PushID(i);
        ImGui::InvisibleButton("##seg", ImVec2(segW, height));
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked())
            result = i;
        ImGui::PopID();

        float hoverT = VisualTransition(window->GetID(items[i]) ^ 0x7791, hovered, 16.0f);
        if (hovered && i != active) {
            window->DrawList->AddRectFilled(ImVec2(segMin.x + 1.0f, segMin.y + 1.0f),
                                            ImVec2(segMax.x - 1.0f, segMax.y - 1.0f),
                                            MixColor(IM_COL32(0x00, 0x00, 0x00, 0x00),
                                                     kColControlHover, hoverT),
                                            kControlRounding);
        }
        if (i == active) {
            window->DrawList->AddRectFilled(ImVec2(segMin.x + 1.0f, segMin.y + 1.0f),
                                            ImVec2(segMax.x - 1.0f, segMax.y - 1.0f),
                                            kColAccent, kControlRounding);
            window->DrawList->AddLine(ImVec2(segMin.x + 5.0f, segMin.y + 1.0f),
                                      ImVec2(segMax.x - 5.0f, segMin.y + 1.0f),
                                      IM_COL32(0xff, 0xff, 0xff, 0x28), 1.0f);
        }

        const char* txt = items[i];
        ImVec2 tsz = ImGui::CalcTextSize(txt);
        ImVec2 txtPos(segMin.x + (segW - tsz.x) * 0.5f,
                      segMin.y + (height - tsz.y) * 0.5f);
        ImU32 txtCol = (i == active)
            ? IM_COL32(0xff, 0xff, 0xff, 0xFF)
            : MixColor(kColTextMuted, kColText, hoverT);
        window->DrawList->AddText(txtPos, txtCol, txt);
    }

    ImGui::Dummy(ImVec2(0.0f, height + 8.0f));

    return result;
}

static std::string ActionSlotTabLabel(int slotIndex) {
    return std::to_string(ClampHeroPresetSlotIndex(slotIndex) + 1);
}

static std::string ActionSlotReadoutLabel(int slotIndex) {
    return std::string("Slot ") + ActionSlotTabLabel(slotIndex);
}

bool UIActionSlotSelector(ActionSlotKind kind, uint64_t heroId) {
    if (!heroId) return false;
    int& active=ActiveHeroPresetSlotRef(kind);
    const int count=ImClamp(GetHeroActionSlotCount(kind,heroId),1,OW::Config::kMaxHeroPresetSlots);
    active=ImClamp(active,0,count-1);
    std::vector<UI::Slots::Item> items;
    items.reserve(count);
    for(int i=0;i<count;++i) {
        OW::Config::HeroSlotPreset slot;
        if(kind==ActionSlotKind::Aim)OW::Config::TryGetHeroAimSlot(heroId,i,slot);
        else OW::Config::TryGetHeroTriggerSlot(heroId,i,slot);
        items.push_back({slot.stableId,slot.name,slot.enabled});
    }
    const auto command=UI::Slots::Render(items,active,OW::Config::kMaxHeroPresetSlots,
        OW::Config::uiLanguage==OW::Config::kUiLanguageChineseSimplified);
    switch(command.action) {
    case UI::Slots::Action::Select:active=command.index;return true;
    case UI::Slots::Action::Add: {
        const int added=AddHeroActionSlot(kind,heroId,GetHeroActionPresetOrDefault(kind,heroId,active));
        if(added<0)return false;
        active=added;return true;
    }
    case UI::Slots::Action::Remove:
        if(!DeleteHeroActionSlot(kind,heroId,active))return false;
        active=ImClamp(active,0,GetHeroActionSlotCount(kind,heroId)-1);return true;
    default:return false;
    }
}

// =====================================================================
// Paired cards share a bottom edge based on the content drawn this frame.
void UIGroupPair(const char* leftTitle, const std::function<void()>& left,
                 const char* rightTitle, const std::function<void()>& right) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 6.0f));
    if (ImGui::GetContentRegionAvail().x < 620.0f * ImGui::GetFontSize() / 15.0f) {
        UIGroupBox(leftTitle);
        left();
        CloseGroupBox();
        if (rightTitle) {
            UIGroupBox(rightTitle);
            right();
            CloseGroupBox();
        }
    } else {
        auto* draw = ImGui::GetWindowDrawList();
        ImDrawListSplitter rowLayers;
        rowLayers.Split(draw, 2);
        rowLayers.SetCurrentChannel(draw, 1);
        ImGui::Columns(2, nullptr, false);
        UIGroupBox(leftTitle);
        left();
        auto leftFrame = FinishGroupBox(false);
        GroupBoxFrame rightFrame{};
        if (rightTitle) {
            ImGui::NextColumn();
            UIGroupBox(rightTitle);
            right();
            rightFrame = FinishGroupBox(false);
            leftFrame.max.y = rightFrame.max.y = MaxFloat(leftFrame.max.y, rightFrame.max.y);
        }
        ImGui::Columns(1);

        rowLayers.SetCurrentChannel(draw, 0);
        DrawGroupBoxFrame(draw, leftFrame);
        if (rightTitle)
            DrawGroupBoxFrame(draw, rightFrame);
        rowLayers.Merge(draw);
    }
    ImGui::PopStyleVar();
}

// =====================================================================
// SettingRow  --  Renders a fixed-width label column + the next widget.
// =====================================================================
void SettingRow(const char* label, float labelWidthPx, float minControlWidthPx) {
    UI::Search::Anchor(label);
    label = L(label);
    const float available = ImGui::GetContentRegionAvail().x;
    labelWidthPx *= (std::max)(1.0f, ImGui::GetFontSize()/15.0f);
    if (minControlWidthPx <= 0.0f)
        minControlWidthPx = ImGui::GetFontSize() * 7.0f + kControlRightPadding;
    if (available < labelWidthPx + minControlWidthPx) {
        ImGui::TextWrapped("%s", label);
        return;
    }
    const ImVec2 cursor = ImGui::GetCursorPos();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::CalcTextSize(label);
    const ImVec2 labelMin(pos.x, pos.y + (ControlHeight() - size.y) * 0.5f);
    const ImVec2 labelMax(pos.x + labelWidthPx - 8.0f, labelMin.y + size.y);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), labelMin, labelMax, labelMax.x, label, nullptr, &size);
    if (size.x > labelMax.x - labelMin.x && ImGui::IsMouseHoveringRect(labelMin, labelMax))
        ImGui::SetTooltip("%s", label);
    ImGui::SetCursorPos(ImVec2(cursor.x + labelWidthPx, cursor.y));
    g_settingRowCursorY = ImGui::GetCursorScreenPos().y;
}

bool UIActionSlotEnabledCheckbox(ActionSlotKind kind,
                                        const HeroOption& hero,
                                        const char* checkboxId) {
    SettingRow("Slot Enabled");
    if (hero.heroId == 0) {
        PushControlWidth();
        UIDisabledSelect("##slotAlways", L("Always"));
        ImGui::PopItemWidth();
        return false;
    }

    const int slotIndex = ActiveHeroPresetSlotIndex(kind);
    bool slotEnabled = IsHeroActionSlotEnabled(kind, hero.heroId, slotIndex);
    if (!UICheckbox(checkboxId, &slotEnabled))
        return false;

    SetHeroActionSlotEnabled(kind, hero.heroId, slotIndex, slotEnabled);
    return true;
}

bool UIColorEdit(const char* label, ImVec4* value, float labelWidthPx) {
    SettingRow(label,labelWidthPx);
    return UI::ColorPicker(label,*value);
}

// =====================================================================
// DrawDivider  --  Thin separator line matching React .divider
// =====================================================================
static void DrawDivider() {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(
        pos, ImVec2(pos.x + width, pos.y), IM_COL32(0x08, 0x09, 0x0b, 0xB8));
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
}

void DrawCheckboxGrid3(const char* labels[], bool* values[], int rowCount,
                              float gapX, const float ratios[3]) {
    float avail = ImGui::GetContentRegionAvail().x;
    float ratioTotal = ratios[0] + ratios[1] + ratios[2];
    float usable = MaxFloat(0.0f, avail - gapX * 2.0f);
    float colW[3] = {
        usable * ratios[0] / ratioTotal,
        usable * ratios[1] / ratioTotal,
        usable * ratios[2] / ratioTotal
    };

    ImVec2 start = ImGui::GetCursorScreenPos();
    float colX[3] = {
        start.x,
        start.x + colW[0] + gapX,
        start.x + colW[0] + gapX + colW[1] + gapX
    };
    const float rowH = ControlHeight();
    const float rowGap = 10.0f;

    for (int row = 0; row < rowCount; ++row) {
        for (int col = 0; col < 3; ++col) {
            int idx = row * 3 + col;
            if (!labels[idx] || !values[idx])
                continue;

            ImGui::SetCursorScreenPos(ImVec2(colX[col], start.y + row * (rowH + rowGap)));
            UICheckbox(labels[idx], values[idx]);
        }
    }

    float gridH = (rowCount > 0) ? (rowCount * rowH + (rowCount - 1) * rowGap) : 0.0f;
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + gridH));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

static void DrawTopTabIcon(ImDrawList* drawList, int tabIndex, const ImVec2& min, ImU32 color) {
    ImVec2 c(min.x + 9.0f, min.y + 9.0f);

    if (tabIndex == 0) {
        drawList->AddCircle(c, 6.0f, color, 24, 1.5f);
        drawList->AddCircle(c, 2.0f, color, 16, 1.5f);
        drawList->AddLine(ImVec2(c.x - 8.0f, c.y), ImVec2(c.x - 4.5f, c.y), color, 1.5f);
        drawList->AddLine(ImVec2(c.x + 4.5f, c.y), ImVec2(c.x + 8.0f, c.y), color, 1.5f);
        drawList->AddLine(ImVec2(c.x, c.y - 8.0f), ImVec2(c.x, c.y - 4.5f), color, 1.5f);
        drawList->AddLine(ImVec2(c.x, c.y + 4.5f), ImVec2(c.x, c.y + 8.0f), color, 1.5f);
    } else if (tabIndex == 1) {
        drawList->AddEllipse(c, ImVec2(8.0f, 4.8f), color, 0.0f, 24, 1.5f);
        drawList->AddCircle(c, 2.4f, color, 16, 1.5f);
    } else if (tabIndex == 2) {
        drawList->AddCircle(c, 7.0f, color, 24, 1.5f);
        drawList->AddCircleFilled(ImVec2(c.x - 2.5f, c.y - 2.2f), 1.1f, color, 8);
        drawList->AddCircleFilled(ImVec2(c.x + 2.0f, c.y - 2.5f), 1.1f, color, 8);
        drawList->AddCircleFilled(ImVec2(c.x - 0.5f, c.y + 2.4f), 1.1f, color, 8);
        drawList->AddCircle(ImVec2(c.x + 4.2f, c.y + 3.4f), 2.1f, color, 10, 1.5f);
    } else {
        drawList->AddCircle(c, 3.3f, color, 16, 1.5f);
        for (int i = 0; i < 8; ++i) {
            float a = (3.14159265f * 2.0f * i) / 8.0f;
            ImVec2 inner(c.x + std::cos(a) * 5.1f, c.y + std::sin(a) * 5.1f);
            ImVec2 outer(c.x + std::cos(a) * 8.0f, c.y + std::sin(a) * 8.0f);
            drawList->AddLine(inner, outer, color, 1.5f);
        }
    }
}

static int CurrentPageKey() {
    switch (UI::state.activeTab) {
        case UI::TAB_ALERT: return kMeasuredPageCount - 1;
        case UI::TAB_AIMING:
            return ImClamp(UI::state.aimingSubTab, 0, kAimingSubTabCount - 1);
        case UI::TAB_VISUALS:
            return kVisualsPageKey;
        case UI::TAB_THEME:
            return ImClamp(UI::state.themeSubTab, 0, kThemeSubTabCount - 1) == 1 ? kThemePageKeyBase + 1 : kVisualsPageKey;
        case UI::TAB_MISC:
            return kMiscPageKeyBase + ImClamp(UI::state.miscSubTab, 0, kMiscSubTabCount - 1);
        default:
            return 0;
    }
}

static float CurrentSubBarHeight() {
    if (UI::state.activeTab == UI::TAB_AIMING)
        return 32.0f;
    if (UI::state.activeTab == UI::TAB_VISUALS)
        return 42.0f;
    if (UI::state.activeTab == UI::TAB_THEME)
        return 32.0f;
    if (UI::state.activeTab == UI::TAB_MISC)
        return 32.0f;
    return 0.0f;
}

static float CurrentBodyTopPad() {
    if (UI::state.activeTab == UI::TAB_AIMING)
        return 8.0f;
    if (UI::state.activeTab == UI::TAB_VISUALS)
        return 10.0f;
    if (UI::state.activeTab == UI::TAB_MISC)
        return 8.0f;
    return 10.0f;
}

static float CurrentShellFrameHeight() {
    return kShellBorder * 2.0f + kHeaderHeight + CurrentSubBarHeight() + CurrentBodyTopPad();
}

static float FallbackShellHeight() {
    if (UI::state.activeTab == UI::TAB_AIMING)
        return 548.0f;
    if (UI::state.activeTab == UI::TAB_VISUALS)
        return 476.0f;
    if (UI::state.activeTab == UI::TAB_THEME)
        return 548.0f;
    if (UI::state.activeTab == UI::TAB_MISC)
        return 548.0f;
    return 140.0f;
}

static float CurrentShellHeight() {
    return 680.0f;
}

static void UpdateDesiredMenuClientSize(float measuredBodyHeight) {
    s_measuredBodyHeightByPage[CurrentPageKey()] = MaxFloat(0.0f, measuredBodyHeight);
    s_desiredMenuClientSize.width = kShellWidth;
    s_desiredMenuClientSize.height = CurrentShellHeight();
}

void UI::InitializeResources(ID3D11Device* device) {
    if (s_logoTexture || !device)
        return;

    CreateTextureFromIconResource(device, IDI_UNLEASHED, kLogoTextureSize, &s_logoTexture);
}

void UI::ShutdownResources() {
    StopFirePatternRecorder();
    AutoSave(true);
    Preferences::Save();
    if (s_logoTexture) {
        s_logoTexture->Release();
        s_logoTexture = nullptr;
    }
}

UI::MenuClientSize UI::DesiredMenuClientSize() {
    return {0, 0};
}

// =====================================================================
// UI::InitStyle  --  Refined dark overlay styling.
// =====================================================================
void UI::InitStyle() { state.initialized = UI::Theme::Initialize(); }

// =====================================================================
// UI::AimbotPage
// =====================================================================
void DrawRuntimeReadinessDetails() {
    UIGroupBox("Connection status");
        const Diagnostics::StatusSnapshot runtimeStatus = Diagnostics::Snapshot();
        const bool connected = OW::ProcessConnection::IsConnected();
        const KeyState::KeyStateVkSample keySample = KeyState::SampleVk(VK_LBUTTON);
        const bool keysReady = keySample.valid && keySample.available;
        const bool cameraFresh = OW::HasFreshViewMatrices();
        const V2::Output::OutputRuntimeAppliedState device = V2::Output::RuntimeController().Applied();
        const bool deviceReady = OW::Config::outputEnabled &&
            V2::Output::RuntimeController().IsOutputGateOpen() &&
            device.descriptor.backend != V2::Output::OutputRuntimeBackend::None;
        const bool mockDevice = deviceReady &&
            device.descriptor.backend == V2::Output::OutputRuntimeBackend::Mock;
        const bool chinese = OW::Config::uiLanguage == OW::Config::kUiLanguageChineseSimplified;
        char readiness[512]{};
        std::snprintf(readiness, sizeof(readiness), chinese
            ? "DMA %s   游戏 %s   按键 %s   相机 %s   外设 %s"
            : "DMA %s   Game %s   Keys %s   Camera %s   Device %s",
            runtimeStatus.dmaReady ? (chinese ? "就绪" : "ready") : (chinese ? "未就绪" : "unavailable"),
            connected ? (chinese ? "已连接" : "connected") : (chinese ? "未连接" : "disconnected"),
            keysReady ? (chinese ? "可用" : "ready") : (chinese ? "不可用" : "unavailable"),
            cameraFresh ? (chinese ? "新鲜" : "fresh") : (chinese ? "过期" : "stale"),
            mockDevice ? (chinese ? "模拟" : "mock") : deviceReady ? (chinese ? "就绪" : "ready") :
                OW::Config::outputEnabled ? (chinese ? "未就绪" : "unavailable") : (chinese ? "未启用" : "disabled"));
        const char* nextStep = nullptr;
        if (!runtimeStatus.dmaReady)
            nextStep = chinese ? "下一步：检查 DMA 连接及设备设置，稍候自动重试或在设置→通用点击重建设备连接。" : "Next: check DMA and device settings, wait for automatic retry or use Settings > General > Rebuild DMA connection.";
        else if (!connected)
            nextStep = chinese ? "下一步：打开游戏，或在设置→通用点击重连。" : "Next: start the game, or use Settings > General > Reconnect.";
        else if (!keysReady)
            nextStep = chinese ? "下一步：在设置→诊断查看主机按键采样错误。" : "Next: check host key sampling under Settings > Diagnostics.";
        else if (!cameraFresh)
            nextStep = chinese ? "下一步：进入游戏场景；若仍未更新，在设置→通用重连。" : "Next: enter a game scene; reconnect if the camera remains stale.";
        else if (!deviceReady)
            nextStep = chinese ? "下一步：在设置→KMBox 设置外设并检查连接。" : "Next: configure and check the device under Settings > KMBox.";
        else if (mockDevice)
            nextStep = chinese ? "当前外设为模拟设备，未使用真实外设。" : "The active device is a mock; no physical device is in use.";
        else if (OW::Config::OutputSuppressedByMenu())
            nextStep = chinese ? "外设就绪；菜单打开时已暂停输出。" : "Device ready; output is paused while this menu is open.";
        else
            nextStep = chinese ? "采样及外设就绪；完整状态可在设置→诊断查看。" : "Samples and device ready. See Settings > Diagnostics for details.";
    ImGui::TextWrapped("%s", readiness);
    SettingTooltip(nextStep);
    CloseGroupBox();
}

static void DrawHeroStrip(int selectedIndex, bool includeCurrent,
                          const std::function<void(int)>& selectHero) {
    const float browseSize = ControlHeight() + 6;
    const float stripWidth = (std::max)(1.0f, ImGui::GetContentRegionAvail().x - browseSize - 8);
    ImGui::BeginChild("HeroStrip", ImVec2(stripWidth, 82), false,
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    auto* stripState = ImGui::GetStateStorage();
    const ImGuiID lastSelectionId = ImGui::GetID("##lastHero");
    const int lastSelection = stripState->GetInt(lastSelectionId, -1);
    const ImGuiID wheelOwner = ImGui::GetID("##heroWheel");
    if (ImGui::IsWindowHovered()) {
        // Reserve both wheel axes for this strip so the settings page stays still.
        ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, wheelOwner);
        ImGui::SetKeyOwner(ImGuiKey_MouseWheelX, wheelOwner);
        const auto& io = ImGui::GetIO();
        if (!io.KeyCtrl && (io.MouseWheel != 0 || io.MouseWheelH != 0))
            ImGui::SetScrollX(ImGui::GetScrollX() - (io.MouseWheel + io.MouseWheelH) * 120.0f);
    }
    const int firstHero = includeCurrent ? 0 : 1;
    for (int i = firstHero; i < IM_ARRAYSIZE(kHeroOptions); ++i) {
        if (i != firstHero) ImGui::SameLine(0, 8);
        const auto& hero = kHeroOptions[i];
        ImGui::PushID(i);
        const bool selected = selectedIndex == i;
        const auto pos = ImGui::GetCursorScreenPos();
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kColAccentDark));
        if (ImGui::Button("##hero", ImVec2(52,58))) selectHero(i);
        if (selected) ImGui::PopStyleColor();
        auto* draw = ImGui::GetWindowDrawList();
        if (auto* avatar = HeroAvatarForOption(hero))
            draw->AddImage(reinterpret_cast<ImTextureID>(avatar), ImVec2(pos.x+4,pos.y+4), ImVec2(pos.x+48,pos.y+48));
        else draw->AddText(ImVec2(pos.x+6,pos.y+18), kColText, hero.heroId ? "?" : L("All"));
        if (selected) draw->AddRectFilled(ImVec2(pos.x+4,pos.y+53), ImVec2(pos.x+48,pos.y+56), kColAccent);
        if (selected && lastSelection != i) ImGui::SetScrollHereX(.5f);
        ImGui::SetItemTooltip("%s\n%s", HeroScopeName(hero), L("Scroll the mouse wheel to browse heroes."));
        ImGui::PopID();
    }
    stripState->SetInt(lastSelectionId, selectedIndex);
    ImGui::EndChild();
    ImGui::SameLine(0, 8);
    const auto pos = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##browseHeroes", ImVec2(browseSize,browseSize))) ImGui::OpenPopup("TypePickerPopup");
    auto* draw = ImGui::GetWindowDrawList();
    const float origin = (browseSize - 16) * .5f;
    for (int y=0;y<2;++y) for (int x=0;x<2;++x) {
        const ImVec2 p(pos.x+origin+x*9, pos.y+origin+y*9);
        draw->AddRect(p,ImVec2(p.x+6,p.y+6),kColTextMuted,1);
    }
    ImGuiContext& g = *ImGui::GetCurrentContext();
    IMGUI_TEST_ENGINE_ITEM_INFO(ImGui::GetItemID(),"Browse all heroes",g.LastItemData.StatusFlags);
    ImGui::SetItemTooltip("%s",L("Browse all heroes"));
    TypePickerPanel(selectedIndex, includeCurrent, selectHero);
}

const HeroOption& DrawPredictionHeroPicker() {
    static int selectedIndex = 0;
    if (selectedIndex == 0)
        selectedIndex = (std::max)(1, ClampHeroSelectionIndex(UI::state.selectedTypeIndex));
    ImGui::PushID("PredictionHeroPicker");
    DrawHeroStrip(selectedIndex, false, [](int index) { selectedIndex = index; });
    ImGui::PopID();
    return kHeroOptions[selectedIndex];
}

struct HeroContextToolbarLayout {
    std::string heroName;
    std::string followLabel;
    ID3D11ShaderResourceView* avatar = nullptr;
    float pillWidth = 0;
    bool detected = false;
};

static HeroContextToolbarLayout MeasureHeroContextToolbar(float height) {
    HeroContextToolbarLayout layout;
    const float scale = (std::max)(1.0f, ImGui::GetFontSize() / 15.0f);
    const uint64_t heroId = DetectedLocalHeroId();
    const std::string detectedName = HeroDisplayNameForId(heroId);
    layout.heroName = L(detectedName.c_str());
    const HeroOption* hero = FindHeroOptionById(heroId);
    layout.avatar = hero ? HeroAvatarForOption(*hero) : nullptr;
    layout.detected = heroId != 0;
    const bool zh = OW::Config::uiLanguage == OW::Config::kUiLanguageChineseSimplified;
    layout.followLabel = zh ? "跟随" : "Follow";

    const float padL = 7.0f * scale;
    const float padR = 10.0f * scale;
    const float avatarBox = height - 10.0f * scale;
    const float switchW = 28.0f * scale;
    const float nameW = ImGui::CalcTextSize(layout.heroName.c_str()).x;
    const float followW = ImGui::CalcTextSize(layout.followLabel.c_str()).x;
    layout.pillWidth = padL + avatarBox + 8.0f * scale + nameW + 10.0f * scale + 1.0f +
                       10.0f * scale + followW + 8.0f * scale + switchW + padR;
    return layout;
}

static void DrawHeroContextToolbar(float height, bool /*stacked*/) {
    const auto layout = MeasureHeroContextToolbar(height);
    const float scale = (std::max)(1.0f, ImGui::GetFontSize() / 15.0f);
    const bool following = UI::state.followDetectedHero;
    const bool zh = OW::Config::uiLanguage == OW::Config::kUiLanguageChineseSimplified;

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(layout.pillWidth, height);
    const bool pressed = ImGui::InvisibleButton("###FollowCurrentHero", size);
    const bool hovered = ImGui::IsItemHovered();
    if (pressed) {
        UI::state.followDetectedHero = !following;
        if (UI::state.followDetectedHero) {
            s_lastSyncedDetectedHeroId = 0;
            SyncSelectedTypeWithDetectedHero();
        }
    }
    ImGui::SetItemTooltip("%s: %s\n%s",
        L("Current"), layout.heroName.c_str(),
        zh ? "点击切换是否自动跟随当前对局英雄" : "Click to toggle auto-following the active in-game hero");

    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    const float rounding = 8.0f * scale;
    const ImVec4 acc = ImGui::ColorConvertU32ToFloat4(kColAccent);
    const ImU32 bgCol = following
        ? ImGui::ColorConvertFloat4ToU32(ImVec4(acc.x, acc.y, acc.z, hovered ? 0.17f : 0.11f))
        : (hovered ? kColControlHover : kColControl);
    const ImU32 borderCol = following
        ? ImGui::ColorConvertFloat4ToU32(ImVec4(acc.x, acc.y, acc.z, hovered ? 0.72f : 0.52f))
        : kColStroke;
    draw->AddRectFilled(pos, end, bgCol, rounding);
    draw->AddRect(pos, end, borderCol, rounding, 0, 1.0f);

    float x = pos.x + 7.0f * scale;
    const float avatarBox = height - 10.0f * scale;
    const ImVec2 avMin(x, pos.y + (height - avatarBox) * 0.5f);
    const ImVec2 avMax(avMin.x + avatarBox, avMin.y + avatarBox);
    if (layout.avatar) {
        draw->AddImageRounded(reinterpret_cast<ImTextureID>(layout.avatar), avMin, avMax,
            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 5.0f * scale);
        draw->AddRect(avMin, avMax, IM_COL32(255, 255, 255, 34), 5.0f * scale);
    } else {
        draw->AddRectFilled(avMin, avMax, kColPanel, 5.0f * scale);
        draw->AddRect(avMin, avMax, kColStroke, 5.0f * scale);
        const char* badge = layout.detected ? "•" : "?";
        const ImVec2 bsz = ImGui::CalcTextSize(badge);
        draw->AddText(ImVec2(avMin.x + (avatarBox - bsz.x) * 0.5f, avMin.y + (avatarBox - bsz.y) * 0.5f),
                      kColTextDim, badge);
    }
    x += avatarBox + 8.0f * scale;

    const ImVec2 nameSize = ImGui::CalcTextSize(layout.heroName.c_str());
    draw->AddText(ImVec2(x, pos.y + (height - nameSize.y) * 0.5f),
                  layout.detected ? kColText : kColTextDim, layout.heroName.c_str());
    x += nameSize.x + 10.0f * scale;

    draw->AddLine(ImVec2(x, pos.y + 8.0f * scale), ImVec2(x, end.y - 8.0f * scale), kColStroke, 1.0f);
    x += 11.0f * scale;

    const ImVec2 flSize = ImGui::CalcTextSize(layout.followLabel.c_str());
    draw->AddText(ImVec2(x, pos.y + (height - flSize.y) * 0.5f),
                  following ? kColTextMuted : kColTextDim, layout.followLabel.c_str());
    x += flSize.x + 8.0f * scale;

    const float switchW = 28.0f * scale;
    const float switchH = 16.0f * scale;
    const ImVec2 swMin(x, pos.y + (height - switchH) * 0.5f);
    const ImVec2 swMax(swMin.x + switchW, swMin.y + switchH);
    const ImU32 trackCol = following ? kColAccent : IM_COL32(62, 66, 76, 220);
    draw->AddRectFilled(swMin, swMax, trackCol, switchH * 0.5f);
    draw->AddRect(swMin, swMax, IM_COL32(255, 255, 255, following ? 55 : 28), switchH * 0.5f);
    const float knobR = (switchH - 4.0f * scale) * 0.5f;
    const float knobX = following ? (swMax.x - 2.0f * scale - knobR) : (swMin.x + 2.0f * scale + knobR);
    draw->AddCircleFilled(ImVec2(knobX, swMin.y + switchH * 0.5f), knobR, IM_COL32_WHITE, 20);
}

struct VisibilityStatusSummary {
    const char* shortLabel = "";
    std::string detail;
    ImU32 dotColor = IM_COL32(108, 214, 150, 255);
    bool captureActive = false;
};

static VisibilityStatusSummary BuildVisibilityStatusSummary(bool zh) {
    VisibilityStatusSummary out{};
    const int mode = UI::Preferences::ActiveVisibilityMode();
    if (mode == static_cast<int>(UI::Preferences::VisibilityMode::Lite)) {
        out.shortLabel = zh ? "实体" : "Lite";
        out.detail = zh ? "仅实体 Vis · 零额外开销" : "Entity Vis only · zero overhead";
        out.dotColor = IM_COL32(108, 214, 150, 255);
        return out;
    }
    if (mode == static_cast<int>(UI::Preferences::VisibilityMode::Vision)) {
        out.shortLabel = zh ? "轮廓" : "Vision";
        V2::Vision::Settings capSettings;
        const auto capFrame = V2::Vision::GetService().Snapshot(capSettings);
        const auto capStatus = V2::Vision::GetService().Status();
        out.captureActive = capStatus.state == V2::Vision::ServiceState::Running ||
                            capStatus.state == V2::Vision::ServiceState::Starting;
        if (!capSettings.enabled || capStatus.state == V2::Vision::ServiceState::Stopped) {
            out.detail = zh ? "采集已停止 · 未收到画面" : "Capture stopped · no frames";
            out.dotColor = IM_COL32(246, 179, 71, 255);
        } else if (capStatus.state == V2::Vision::ServiceState::Faulted) {
            out.detail = zh ? "采集失败 · 请检查设备" : "Capture faulted · check device";
            out.dotColor = IM_COL32(242, 92, 110, 255);
        } else if (capStatus.state == V2::Vision::ServiceState::Starting || !capFrame) {
            out.detail = zh ? "采集连接中 · 等待画面" : "Capture starting · waiting";
            out.dotColor = IM_COL32(246, 179, 71, 255);
        } else {
            char buf[96]{};
            std::snprintf(buf, sizeof(buf), zh ? "采集中 %.0f fps · Aim & Trigger" : "Capture %.0f fps · Aim & Trigger", capStatus.fps);
            out.detail = buf;
            out.dotColor = IM_COL32(108, 214, 150, 255);
        }
        return out;
    }
    out.shortLabel = zh ? "射线" : "Ray";
    const bool serviceBound = OW::RayRuntime::HasAttachedService();
    const uint64_t sceneEpoch = OW::RayRuntime::SceneEpoch();
    if (!serviceBound) {
        out.detail = zh ? "UniCore 服务未附着" : "UniCore service not attached";
        out.dotColor = IM_COL32(242, 92, 110, 255);
    } else if (sceneEpoch == 0) {
        out.detail = zh ? "等待进局建立射线场景" : "Waiting for match ray scene";
        out.dotColor = IM_COL32(246, 179, 71, 255);
    } else {
        out.detail = zh ? "6 引擎就绪 · 场景已绑定" : "6-engine ready · scene bound";
        out.dotColor = IM_COL32(108, 214, 150, 255);
    }
    return out;
}

static void DrawNeverloseAccountPopup(
    const UI::Workspace::Model& model,
    const std::function<void(int)>& navigateToPage,
    ImVec2 popupPos,
    ImVec2 popupSize,
    float scale,
    bool zh)
{
    auto* draw = ImGui::GetWindowDrawList();
    const float availW = ImGui::GetContentRegionAvail().x;
    const float rowH = (std::max)(30.0f * scale, ImGui::GetFontSize() + 14.0f * scale);
    const auto& username = V2::Launcher::username;
    const char* name = username.empty() ? "—" : username.c_str();
    const ImU32 connColor = model.connected ? IM_COL32(108, 214, 150, 255) : IM_COL32(242, 92, 110, 255);

    // ---- Header: compact avatar + username + Till: Lifetime + read-only region/version ----
    {
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        const float av = 38.0f * scale;
        const ImVec2 avCenter(hp.x + 2.0f * scale + av * 0.5f, hp.y + av * 0.5f + 2.0f * scale);
        draw->AddCircleFilled(avCenter, av * 0.5f, kColControl, 28);
        draw->AddCircle(avCenter, av * 0.5f, kColStroke, 28, 1.0f);
        draw->AddCircleFilled(ImVec2(avCenter.x, avCenter.y - 4.5f * scale), 5.0f * scale, kColTextDim, 20);
        draw->PathLineTo(ImVec2(avCenter.x - 9.0f * scale, avCenter.y + 8.5f * scale));
        draw->PathBezierCubicCurveTo(ImVec2(avCenter.x - 8.2f * scale, avCenter.y + 0.5f * scale),
            ImVec2(avCenter.x + 8.2f * scale, avCenter.y + 0.5f * scale),
            ImVec2(avCenter.x + 9.0f * scale, avCenter.y + 8.5f * scale));
        draw->PathBezierCubicCurveTo(ImVec2(avCenter.x + 4.5f * scale, avCenter.y + 11.2f * scale),
            ImVec2(avCenter.x - 4.5f * scale, avCenter.y + 11.2f * scale),
            ImVec2(avCenter.x - 9.0f * scale, avCenter.y + 8.5f * scale));
        draw->PathFillConvex(kColTextDim);

        const float textX = hp.x + av + 11.0f * scale;
        const float smallFs = ImGui::GetFontSize() * 0.82f;
        const char* copyTxt = zh ? "复制" : "Copy";
        const float copyW = ImGui::CalcTextSize(copyTxt).x + 12.0f * scale;
        const float copyH = ImGui::GetFontSize() + 4.0f * scale;
        const float nameMaxX = hp.x + availW - copyW - 4.0f * scale;

        draw->PushClipRect(ImVec2(textX, hp.y), ImVec2((std::max)(textX, nameMaxX), hp.y + 52.0f * scale), true);
        draw->AddText(ImVec2(textX, hp.y), kColText, name);
        draw->PopClipRect();

        ImGui::SetCursorScreenPos(ImVec2(hp.x + availW - copyW, hp.y - 1.0f * scale));
        ImGui::BeginDisabled(username.empty());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * scale, 1.0f * scale));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColControlHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColControlHot);
        if (ImGui::Button(copyTxt, ImVec2(copyW, copyH))) {
            ImGui::SetClipboardText(username.c_str());
            SetPersistenceStatus(true, zh ? "已复制用户 ID" : "Copied user ID");
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        ImGui::EndDisabled();

        const float subY1 = hp.y + ImGui::GetFontSize() + 1.5f * scale;
        draw->AddText(ImGui::GetFont(), smallFs, ImVec2(textX, subY1), kColTextDim, "Till: Lifetime");

        std::string connLine;
        if (model.connected && !model.region.empty()) {
            connLine = model.region;
            if (!model.buildVersion.empty())
                connLine += " · " + model.buildVersion;
            if (model.pid > 0)
                connLine += " · PID " + std::to_string(model.pid);
        } else if (model.connected) {
            connLine = zh ? "已连接" : "Connected";
            if (model.pid > 0)
                connLine += " · PID " + std::to_string(model.pid);
        } else {
            connLine = zh ? "未连接" : "Disconnected";
        }
        const float subY2 = subY1 + smallFs + 2.0f * scale;
        draw->AddCircleFilled(ImVec2(textX + 3.2f * scale, subY2 + smallFs * 0.52f), 3.0f * scale, connColor, 16);
        draw->PushClipRect(ImVec2(textX + 9.0f * scale, subY2 - 2.0f), ImVec2(hp.x + availW, subY2 + smallFs + 4.0f), true);
        draw->AddText(ImGui::GetFont(), smallFs, ImVec2(textX + 9.5f * scale, subY2), connColor, connLine.c_str());
        draw->PopClipRect();

        ImGui::SetCursorScreenPos(ImVec2(hp.x, (std::max)(hp.y + av + 6.0f * scale, subY2 + smallFs + 6.0f * scale)));
        if (model.connected) {
            ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y));
            ImGui::Dummy(ImVec2(availW - copyW - 4.0f * scale, (std::max)(av + 4.0f * scale, subY2 + smallFs - hp.y + 4.0f * scale)));
            ImGui::SetItemTooltip("%s", connLine.c_str());
        } else {
            ImGui::Dummy(ImVec2(0, 0));
        }
    }

    ImGui::Separator();
    ImGui::Spacing();

    auto placeSubPopup = [&](float rowScreenY, float subWidth, float estHeight) {
        const auto* vp = ImGui::GetMainViewport();
        const float margin = 8.0f * scale;
        const float vpLeft = vp->WorkPos.x + margin;
        const float vpRight = vp->WorkPos.x + vp->WorkSize.x - margin;
        const float vpTop = vp->WorkPos.y + margin;
        const float vpBottom = vp->WorkPos.y + vp->WorkSize.y - margin;
        const float maxAvailH = (std::max)(80.0f * scale, vpBottom - vpTop);
        float sx = popupPos.x + popupSize.x + margin;
        if (sx + subWidth > vpRight)
            sx = (std::max)(vpLeft, popupPos.x - subWidth - margin);
        const bool alignBottom = (rowScreenY + estHeight + 16.0f * scale > vpBottom);
        const ImVec2 pos = alignBottom
            ? ImVec2(sx, vpBottom)
            : ImVec2(sx, (std::max)(vpTop, rowScreenY));
        const ImVec2 pivot = alignBottom ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
        ImGui::SetNextWindowSizeConstraints(ImVec2(subWidth, 0.0f), ImVec2(subWidth, maxAvailH));
        ImGui::SetNextWindowSize(ImVec2(subWidth, 0.0f), ImGuiCond_Always);
    };

    auto drawMenuRow = [&](const char* id, const char* label, const char* valueText,
                           bool hasDot, ImU32 dotCol, bool hasChevron, bool activeSub,
                           bool danger, const std::function<void(ImDrawList*, ImVec2, ImU32)>& drawGlyph,
                           float* outRowY = nullptr,
                           const std::function<void(ImDrawList*, float rightEdge, float centerY)>& drawExtraRight = {}) -> bool {
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        if (outRowY) *outRowY = rp.y;
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(availW, rowH));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 rend(rp.x + availW, rp.y + rowH);
        if (hovered || activeSub) {
            const ImU32 hoverBg = danger ? IM_COL32(242, 92, 110, 38) : kColControlHover;
            draw->AddRectFilled(rp, rend, hoverBg, 6.0f * scale);
        }
        const ImU32 fgCol = danger
            ? (hovered ? IM_COL32(255, 146, 158, 255) : kColTextMuted)
            : ((hovered || activeSub) ? kColText : kColTextMuted);
        const ImU32 iconCol = danger
            ? (hovered ? IM_COL32(255, 146, 158, 255) : kColTextDim)
            : ((hovered || activeSub) ? kColAccent : kColTextDim);
        const float cy = rp.y + rowH * 0.5f;
        if (drawGlyph) drawGlyph(draw, ImVec2(rp.x + 12.0f * scale, cy), iconCol);
        const float textX = rp.x + 28.0f * scale;
        float rightCursor = rend.x - 8.0f * scale;
        if (hasChevron) {
            const float cx = rightCursor - 3.0f * scale;
            draw->AddLine(ImVec2(cx - 2.2f * scale, cy - 3.5f * scale), ImVec2(cx + 1.2f * scale, cy), kColTextDim, 1.3f * scale);
            draw->AddLine(ImVec2(cx + 1.2f * scale, cy), ImVec2(cx - 2.2f * scale, cy + 3.5f * scale), kColTextDim, 1.3f * scale);
            rightCursor = cx - 8.0f * scale;
        }
        if (drawExtraRight) {
            drawExtraRight(draw, rightCursor, cy);
            rightCursor -= 18.0f * scale;
        }
        if (valueText && *valueText) {
            const float smallFs = ImGui::GetFontSize() * 0.86f;
            const float maxValW = (std::max)(40.0f * scale, availW * 0.42f);
            const ImVec2 vsz = ImGui::GetFont()->CalcTextSizeA(smallFs, FLT_MAX, 0.f, valueText);
            const float vw = (std::min)(vsz.x, maxValW);
            const float vx = rightCursor - vw;
            draw->PushClipRect(ImVec2(vx, rp.y), ImVec2(rightCursor, rend.y), true);
            draw->AddText(ImGui::GetFont(), smallFs, ImVec2(vx, cy - smallFs * 0.5f), kColTextDim, valueText);
            draw->PopClipRect();
            rightCursor = vx - 5.0f * scale;
            if (hasDot) {
                draw->AddCircleFilled(ImVec2(rightCursor - 2.5f * scale, cy), 3.0f * scale, dotCol, 16);
                rightCursor -= 9.0f * scale;
            }
        }
        draw->PushClipRect(ImVec2(textX, rp.y), ImVec2((std::max)(textX, rightCursor - 4.0f * scale), rend.y), true);
        draw->AddText(ImVec2(textX, cy - ImGui::GetFontSize() * 0.5f), fgCol, label);
        draw->PopClipRect();
        return clicked;
    };

    auto openSubStyle = [&]() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f * scale, 9.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, kColPanel);
        ImGui::PushStyleColor(ImGuiCol_Border, kColStroke);
    };
    auto closeSubStyle = [&]() {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    };

    // 1. Visibility mode (Lite / Vision / Ray)
    {
        const auto vis = BuildVisibilityStatusSummary(zh);
        float rowY = 0.0f;
        const bool subOpen = ImGui::IsPopupOpen("##SubVisMode", ImGuiPopupFlags_None);
        if (drawMenuRow("##rowVisMode", zh ? "可见性模式" : "Visibility", vis.shortLabel,
                        true, vis.dotColor, true, subOpen, false,
                        [scale](ImDrawList* d, ImVec2 c, ImU32 col) {
                            d->AddCircle(c, 5.2f * scale, col, 20, 1.3f);
                            d->AddCircleFilled(c, 2.0f * scale, col, 12);
                        }, &rowY)) {
            ImGui::OpenPopup("##SubVisMode");
        }
        const float subW = 224.0f * scale;
        const bool isVision = UI::Preferences::ActiveVisibilityMode() == static_cast<int>(UI::Preferences::VisibilityMode::Vision);
        placeSubPopup(rowY, subW, (isVision ? 270.0f : 200.0f) * scale);
        openSubStyle();
        if (ImGui::BeginPopup("##SubVisMode", ImGuiWindowFlags_NoMove)) {
            ImGui::TextDisabled("%s", zh ? "全局可见性来源" : "Global visibility source");
            ImGui::Spacing();
            const int curMode = UI::Preferences::ActiveVisibilityMode();
            const char* labels[] = {
                zh ? "实体 (Lite)" : "Entity (Lite)",
                zh ? "轮廓 (Vision)" : "Contour (Vision)",
                zh ? "射线 (Ray)" : "Ray (UniCore)"
            };
            const char* hints[] = {
                zh ? "仅读实体" : "Vis only",
                zh ? "全局 Aim+Trigger" : "Aim + Trigger",
                zh ? "6 引擎射线" : "6-engine ray"
            };
            for (int m = 0; m < 3; ++m) {
                ImGui::PushID(m);
                const bool sel = (curMode == m);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Header, kColAccentDark);
                if (ImGui::Selectable(labels[m], sel, ImGuiSelectableFlags_DontClosePopups, ImVec2(0, 26.0f * scale))) {
                    UI::Preferences::SetActiveVisibilityMode(m);
                    SetPersistenceStatus(UI::Preferences::Save(), T(UiText::SavedAllChanges));
                }
                if (sel) ImGui::PopStyleColor();
                const ImVec2 itemMax = ImGui::GetItemRectMax();
                const ImVec2 itemMin = ImGui::GetItemRectMin();
                const float hfs = ImGui::GetFontSize() * 0.80f;
                const float hw = ImGui::GetFont()->CalcTextSizeA(hfs, FLT_MAX, 0.f, hints[m]).x;
                ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), hfs,
                    ImVec2(itemMax.x - hw - 4.0f * scale, (itemMin.y + itemMax.y - hfs) * 0.5f),
                    kColTextDim, hints[m]);
                ImGui::PopID();
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            const auto latest = BuildVisibilityStatusSummary(zh);
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(sp.x + 4.0f * scale, sp.y + ImGui::GetFontSize() * 0.5f), 3.2f * scale, latest.dotColor, 16);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 13.0f * scale);
            ImGui::TextWrapped("%s", latest.detail.c_str());
            if (UI::Preferences::ActiveVisibilityMode() == static_cast<int>(UI::Preferences::VisibilityMode::Vision)) {
                ImGui::Spacing();
                if (!latest.captureActive) {
                    if (ImGui::Selectable(zh ? "启动采集卡" : "Start capture", false, ImGuiSelectableFlags_DontClosePopups)) {
                        auto cfg = V2::Vision::GetService().GetSettings();
                        cfg.enabled = true;
                        V2::Vision::GetService().Configure(cfg);
                        if (!V2::Vision::GetService().Start())
                            SetPersistenceStatus(false, "");
                        else
                            SetPersistenceStatus(true, zh ? "采集卡已启动" : "Capture started");
                    }
                    if (ImGui::Selectable(zh ? "打开采集与轮廓设置..." : "Open capture settings...")) {
                        if (navigateToPage) navigateToPage(13);
                        ImGui::CloseCurrentPopup();
                    }
                } else if (ImGui::Selectable(zh ? "停止采集卡" : "Stop capture", false, ImGuiSelectableFlags_DontClosePopups)) {
                    V2::Vision::GetService().Stop();
                    auto cfg = V2::Vision::GetService().GetSettings();
                    cfg.enabled = false;
                    V2::Vision::GetService().Configure(cfg);
                    UI::Vision::ReleasePreviewResources();
                    SetPersistenceStatus(true, zh ? "已停止采集卡" : "Capture stopped");
                }
            }
            ImGui::EndPopup();
        }
        closeSubStyle();
    }

    // 2. Active Config Profile
    {
        float rowY = 0.0f;
        const bool subOpen = ImGui::IsPopupOpen("##SubProfile", ImGuiPopupFlags_None);
        if (drawMenuRow("##rowProfile", zh ? "配置文件" : "Profile", OW::Config::configFileName.c_str(),
                        false, 0, true, subOpen, false,
                        [scale](ImDrawList* d, ImVec2 c, ImU32 col) {
                            for (int i = -1; i <= 1; ++i)
                                d->AddLine(ImVec2(c.x - 5.0f * scale, c.y + i * 3.5f * scale),
                                           ImVec2(c.x + 5.0f * scale, c.y + i * 3.5f * scale), col, 1.3f);
                        }, &rowY)) {
            ImGui::OpenPopup("##SubProfile");
        }
        const float subW = 216.0f * scale;
        placeSubPopup(rowY, subW, 220.0f * scale);
        openSubStyle();
        if (ImGui::BeginPopup("##SubProfile", ImGuiWindowFlags_NoMove)) {
            ImGui::TextDisabled("%s", zh ? "切换配置档" : "Switch profile");
            ImGui::Spacing();
            const std::vector<std::string> profiles = EnumerateConfigProfiles();
            static char profileBuf[kConfigProfileNameBufferSize] = "";
            for (const std::string& p : profiles) {
                const bool sel = (p == OW::Config::configFileName);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Header, kColAccentDark);
                if (ImGui::Selectable(p.c_str(), sel, 0, ImVec2(0, 25.0f * scale))) {
                    if (SelectConfigProfile(p.c_str(), profileBuf))
                        SetPersistenceStatus(PersistLastConfigProfile(), T(UiText::SavedAllChanges));
                    else
                        SetPersistenceStatus(false, "");
                }
                if (sel) ImGui::PopStyleColor();
            }
            ImGui::EndPopup();
        }
        closeSubStyle();
    }

    // 3. Language
    {
        float rowY = 0.0f;
        const bool subOpen = ImGui::IsPopupOpen("##SubLang", ImGuiPopupFlags_None);
        if (drawMenuRow("##rowLang", zh ? "语言" : "Language", zh ? "中文" : "English",
                        false, 0, true, subOpen, false,
                        [scale](ImDrawList* d, ImVec2 c, ImU32 col) {
                            d->AddCircle(c, 5.5f * scale, col, 18, 1.25f);
                            d->AddLine(ImVec2(c.x - 5.2f * scale, c.y), ImVec2(c.x + 5.2f * scale, c.y), col, 1.1f);
                            d->AddEllipse(c, ImVec2(2.6f * scale, 5.5f * scale), col, 0.f, 16, 1.1f);
                        }, &rowY)) {
            ImGui::OpenPopup("##SubLang");
        }
        const float subW = 176.0f * scale;
        placeSubPopup(rowY, subW, 112.0f * scale);
        openSubStyle();
        if (ImGui::BeginPopup("##SubLang", ImGuiWindowFlags_NoMove)) {
            ImGui::TextDisabled("%s", zh ? "界面语言" : "Language");
            ImGui::Spacing();
            const int curLang = OW::Config::ClampUiLanguage(OW::Config::uiLanguage);
            const char* langs[] = { "English", "简体中文" };
            for (int l = 0; l < 2; ++l) {
                const bool sel = (curLang == l);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Header, kColAccentDark);
                if (ImGui::Selectable(langs[l], sel, 0, ImVec2(0, 25.0f * scale))) {
                    OW::Config::uiLanguage = l;
                    UI::Preferences::values.language = l;
                    UI::Preferences::Save();
                }
                if (sel) ImGui::PopStyleColor();
            }
            ImGui::EndPopup();
        }
        closeSubStyle();
    }

    // 4. Dual-color Appearance & Custom Theme Presets
    {
        float rowY = 0.0f;
        const bool subOpen = ImGui::IsPopupOpen("##SubTheme", ImGuiPopupFlags_None);
        const auto* activePreset = UI::Preferences::FindThemePreset(UI::Preferences::values.activeThemeId);
        const char* themeLabel = activePreset ? activePreset->name.c_str() : (zh ? "自定义" : "Custom");
        const ImU32 baseCol = ImGui::ColorConvertFloat4ToU32(UI::Theme::SanitizeBaseDarkColor(UI::Preferences::values.baseDark));
        const ImU32 accCol = ImGui::ColorConvertFloat4ToU32(UI::Preferences::values.accent);
        if (drawMenuRow("##rowTheme", zh ? "外观风格" : "Style", themeLabel,
                        false, 0, true, subOpen, false,
                        [scale](ImDrawList* d, ImVec2 c, ImU32 col) {
                            d->AddCircle(c, 5.4f * scale, col, 20, 1.3f);
                            d->AddCircleFilled(ImVec2(c.x - 1.8f * scale, c.y - 1.4f * scale), 1.3f * scale, col);
                            d->AddCircleFilled(ImVec2(c.x + 1.8f * scale, c.y - 1.4f * scale), 1.3f * scale, col);
                            d->AddCircleFilled(ImVec2(c.x, c.y + 2.0f * scale), 1.3f * scale, col);
                        },
                        &rowY,
                        [scale, baseCol, accCol](ImDrawList* d, float rightEdge, float cy) {
                            const float r = 5.0f * scale;
                            d->AddCircleFilled(ImVec2(rightEdge - r - 6.0f * scale, cy), r, baseCol, 18);
                            d->AddCircle(ImVec2(rightEdge - r - 6.0f * scale, cy), r, IM_COL32(255, 255, 255, 70), 18, 1.0f);
                            d->AddCircleFilled(ImVec2(rightEdge - r, cy), r, accCol, 18);
                            d->AddCircle(ImVec2(rightEdge - r, cy), r, IM_COL32(255, 255, 255, 90), 18, 1.0f);
                        })) {
            ImGui::OpenPopup("##SubTheme");
        }
        const float subW = 262.0f * scale;
        const float themeEstH = (240.0f + static_cast<float>(3 + UI::Preferences::values.customThemes.size()) * 29.0f) * scale;
        placeSubPopup(rowY - 80.0f * scale, subW, themeEstH);
        openSubStyle();
        if (ImGui::BeginPopup("##SubTheme", ImGuiWindowFlags_NoMove)) {
            auto& prefs = UI::Preferences::values;
            ImGui::TextDisabled("%s", zh ? "外观预设" : "Theme presets");
            ImGui::Spacing();

            std::string pendingDeleteId;
            auto drawPresetEntry = [&](const UI::Preferences::ThemePreset& preset) {
                ImGui::PushID(preset.id.c_str());
                const bool sel = (prefs.activeThemeId == preset.id);
                const float delW = preset.builtin ? 0.0f : (22.0f * scale);
                const float itemW = ImGui::GetContentRegionAvail().x - delW - (preset.builtin ? 0.0f : 4.0f * scale);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (sel) ImGui::PushStyleColor(ImGuiCol_Header, kColAccentDark);
                if (ImGui::Selectable("##presetRow", sel, ImGuiSelectableFlags_DontClosePopups, ImVec2(itemW, 25.0f * scale))) {
                    UI::Preferences::ApplyThemePreset(preset.id);
                    UI::Theme::Apply();
                    UI::Preferences::Save();
                }
                if (sel) ImGui::PopStyleColor();
                auto* dl = ImGui::GetWindowDrawList();
                const float cy = p.y + 12.5f * scale;
                const ImU32 pb = ImGui::ColorConvertFloat4ToU32(UI::Theme::SanitizeBaseDarkColor(preset.baseDark));
                const ImU32 pa = ImGui::ColorConvertFloat4ToU32(preset.accent);
                dl->AddCircleFilled(ImVec2(p.x + 8.0f * scale, cy), 4.5f * scale, pb, 16);
                dl->AddCircle(ImVec2(p.x + 8.0f * scale, cy), 4.5f * scale, IM_COL32(255, 255, 255, 70), 16, 1.0f);
                dl->AddCircleFilled(ImVec2(p.x + 14.0f * scale, cy), 4.5f * scale, pa, 16);
                dl->AddCircle(ImVec2(p.x + 14.0f * scale, cy), 4.5f * scale, IM_COL32(255, 255, 255, 90), 16, 1.0f);
                dl->PushClipRect(ImVec2(p.x + 24.0f * scale, p.y), ImVec2(p.x + itemW - 4.0f * scale, p.y + 25.0f * scale), true);
                dl->AddText(ImVec2(p.x + 24.0f * scale, cy - ImGui::GetFontSize() * 0.5f), sel ? kColText : kColTextMuted, preset.name.c_str());
                dl->PopClipRect();
                if (!preset.builtin) {
                    ImGui::SameLine(0, 4.0f * scale);
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(242, 92, 110, 50));
                    if (ImGui::Button("x##delTheme", ImVec2(delW, 25.0f * scale)))
                        pendingDeleteId = preset.id;
                    ImGui::PopStyleColor(2);
                }
                ImGui::PopID();
            };

            for (const auto& p : UI::Preferences::BuiltinThemePresets())
                drawPresetEntry(p);
            for (const auto& p : prefs.customThemes)
                drawPresetEntry(p);
            if (!pendingDeleteId.empty() && UI::Preferences::DeleteCustomThemePreset(pendingDeleteId)) {
                UI::Preferences::Save();
                SetPersistenceStatus(true, zh ? "已删除自定义外观预设" : "Deleted custom theme preset");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Dual-color pickers: dark base + accent
            bool colorsEdited = false;
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", zh ? "暗底色" : "Base dark");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - UI::kColorSwatchWidth);
            if (UI::ColorPicker("##popupBaseDark", prefs.baseDark)) {
                prefs.baseDark.w = 1.0f;
                prefs.activeThemeId.clear();
                prefs.customAccent = true;
                colorsEdited = true;
            }

            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", zh ? "强调色" : "Accent");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - UI::kColorSwatchWidth);
            if (UI::ColorPicker("##popupAccent", prefs.accent)) {
                prefs.accent.w = 1.0f;
                prefs.activeThemeId.clear();
                prefs.customAccent = true;
                colorsEdited = true;
            }

            // Quick accent dots
            constexpr ImVec4 kQuickAccents[] = {
                {228.f/255.f, 17.f/255.f, 67.f/255.f, 1.f},
                {59.f/255.f, 130.f/255.f, 246.f/255.f, 1.f},
                {46.f/255.f, 229.f/255.f, 168.f/255.f, 1.f},
                {171.f/255.f, 147.f/255.f, 255.f/255.f, 1.f},
                {245.f/255.f, 158.f/255.f, 11.f/255.f, 1.f},
                {236.f/255.f, 72.f/255.f, 153.f/255.f, 1.f},
            };
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", zh ? "快选" : "Quick");
            for (int q = 0; q < IM_ARRAYSIZE(kQuickAccents); ++q) {
                ImGui::SameLine(q == 0 ? 54.0f * scale : 0.0f, 6.0f * scale);
                ImGui::PushID(q);
                const ImVec2 qp = ImGui::GetCursorScreenPos();
                const float qd = 16.0f * scale;
                if (ImGui::InvisibleButton("##qAcc", ImVec2(qd, qd))) {
                    prefs.accent = kQuickAccents[q];
                    prefs.activeThemeId.clear();
                    prefs.customAccent = true;
                    colorsEdited = true;
                }
                auto* dl = ImGui::GetWindowDrawList();
                dl->AddCircleFilled(ImVec2(qp.x + qd * 0.5f, qp.y + qd * 0.5f), qd * 0.48f,
                    ImGui::ColorConvertFloat4ToU32(kQuickAccents[q]), 18);
                dl->AddCircle(ImVec2(qp.x + qd * 0.5f, qp.y + qd * 0.5f), qd * 0.48f,
                    ImGui::IsItemHovered() ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 65), 18, 1.0f);
                ImGui::PopID();
            }

            if (colorsEdited) {
                UI::Theme::Apply();
                UI::Preferences::Save();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            static char newThemeName[48] = "";
            const bool full = prefs.customThemes.size() >= UI::Preferences::kMaxCustomThemePresets;
            const char* savePresetTxt = zh ? "存预设" : "Save";
            const float btnW = ImGui::CalcTextSize(savePresetTxt).x + 18.0f * scale;
            ImGui::BeginDisabled(full);
            ImGui::SetNextItemWidth((std::max)(80.0f * scale, ImGui::GetContentRegionAvail().x - btnW - 6.0f * scale));
            const bool submitted = ImGui::InputTextWithHint("##newThemeName",
                zh ? "新预设名称…" : "New preset name...", newThemeName, sizeof(newThemeName),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine(0, 6.0f * scale);
            if ((submitted || ImGui::Button(savePresetTxt, ImVec2(btnW, 0))) && !full) {
                const std::string createdId = UI::Preferences::AddCustomThemePreset(
                    newThemeName, prefs.baseDark, prefs.accent);
                if (!createdId.empty()) {
                    newThemeName[0] = '\0';
                    UI::Theme::Apply();
                    SetPersistenceStatus(UI::Preferences::Save(), zh ? "已保存自定义外观预设" : "Saved custom theme preset");
                }
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        closeSubStyle();
    }

    // 5. Overlay Canvas Background Mode
    {
        float rowY = 0.0f;
        const bool subOpen = ImGui::IsPopupOpen("##SubCanvasBg", ImGuiPopupFlags_None);
        const char* bgShort = OW::Config::overlayTransparentBackground
            ? (zh ? "透明" : "Transp.")
            : (zh ? "纯黑" : "Opaque");
        if (drawMenuRow("##rowCanvasBg", zh ? "画布背景" : "Canvas", bgShort,
                        false, 0, true, subOpen, false,
                        [scale](ImDrawList* d, ImVec2 c, ImU32 col) {
                            d->AddRect(ImVec2(c.x - 5.5f * scale, c.y - 4.2f * scale),
                                       ImVec2(c.x + 5.5f * scale, c.y + 4.2f * scale), col, 1.5f, 0, 1.3f);
                        }, &rowY)) {
            ImGui::OpenPopup("##SubCanvasBg");
        }
        const float subW = 196.0f * scale;
        placeSubPopup(rowY, subW, 112.0f * scale);
        openSubStyle();
        if (ImGui::BeginPopup("##SubCanvasBg", ImGuiWindowFlags_NoMove)) {
            ImGui::TextDisabled("%s", zh ? "画布背景模式" : "Canvas background");
            ImGui::Spacing();
            const char* modes[] = { zh ? "纯黑融合器" : "Opaque black", zh ? "透明背景" : "Transparent" };
            const int cur = OW::Config::overlayTransparentBackground ? 1 : 0;
            for (int m = 0; m < 2; ++m) {
                const bool sel = (cur == m);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Header, kColAccentDark);
                if (ImGui::Selectable(modes[m], sel, 0, ImVec2(0, 25.0f * scale))) {
                    const bool transparentBg = (m == 1);
                    if (!g_Overlay.SetCanvasTransparentBackground(transparentBg))
                        OW::Config::overlayTransparentBackground = g_Overlay.IsCanvasTransparentBackground();
                    else
                        OW::Config::overlayTransparentBackground = transparentBg;
                }
                if (sel) ImGui::PopStyleColor();
            }
            ImGui::EndPopup();
        }
        closeSubStyle();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Bottom quiet action rows (no DryRun, no region picker)
    const bool recoveryBusy = OW::ProcessConnection::IsRecoveryBusy();
    ImGui::BeginDisabled(recoveryBusy);
    if (drawMenuRow("##actRefresh", zh ? "刷新缓存" : "Refresh caches", nullptr, false, 0, false, false, false,
                    [scale](ImDrawList* d, ImVec2 c, ImU32 col) {
                        d->PathArcTo(c, 4.8f * scale, 0.4f, 5.4f, 16);
                        d->PathStroke(col, 0, 1.3f);
                    })) {
        StopFirePatternRecorder();
        OW::ProcessConnection::RequestCacheRefresh();
        ImGui::CloseCurrentPopup();
    }
    if (drawMenuRow("##actReconnect", zh ? "重连进程" : "Reconnect", nullptr, false, 0, false, false, false,
                    [scale](ImDrawList* d, ImVec2 c, ImU32 col) {
                        const ImVec2 pts[] = {
                            {c.x + 1.0f * scale, c.y - 5.5f * scale},
                            {c.x - 3.5f * scale, c.y + 0.5f * scale},
                            {c.x + 0.2f * scale, c.y + 0.5f * scale},
                            {c.x - 1.2f * scale, c.y + 5.5f * scale},
                            {c.x + 3.5f * scale, c.y - 0.8f * scale},
                            {c.x - 0.2f * scale, c.y - 0.8f * scale},
                        };
                        d->AddPolyline(pts, 6, col, ImDrawFlags_Closed, 1.2f);
                    })) {
        OW::ProcessConnection::RequestReconnect();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();

    if (drawMenuRow("##actMinimize", zh ? "最小化画布" : "Minimize overlay", nullptr, false, 0, false, false, false,
                    [scale](ImDrawList* d, ImVec2 c, ImU32 col) {
                        d->AddLine(ImVec2(c.x - 4.8f * scale, c.y + 2.0f * scale),
                                   ImVec2(c.x + 4.8f * scale, c.y + 2.0f * scale), col, 1.5f);
                    })) {
        g_Overlay.MinimizeToTaskbar();
        ImGui::CloseCurrentPopup();
    }
    if (drawMenuRow("##actExit", zh ? "退出 UNLEASHED" : "Exit UNLEASHED", nullptr, false, 0, false, false, true,
                    [scale](ImDrawList* d, ImVec2 c, ImU32 col) {
                        d->PathArcTo(c, 4.8f * scale, -IM_PI * 0.25f, IM_PI * 1.25f, 16);
                        d->PathStroke(col, 0, 1.3f);
                        d->AddLine(ImVec2(c.x, c.y - 5.8f * scale), ImVec2(c.x, c.y - 0.8f * scale), col, 1.4f);
                    })) {
        g_Overlay.RequestExit();
        ImGui::CloseCurrentPopup();
    }
}

static void DrawEditingContext() {
    if (UI::state.activeTab == UI::TAB_PREDICTION)
        return;
    const bool heroPage = UI::state.activeTab == UI::TAB_AIMING || UI::state.activeTab == UI::TAB_ALERT || UI::state.activeTab == UI::TAB_KEYS ||
        (UI::state.activeTab == UI::TAB_THEME && UI::state.themeSubTab == 1);
    if (!heroPage) {
        ImGui::TextDisabled("%s", L("Shared settings"));
        ImGui::Spacing();
        return;
    }
    CurrentHeroOption();
    DrawHeroStrip(UI::state.selectedTypeIndex, true, [](int index) {
        UI::state.followDetectedHero = false;
        SelectTypeIndex(index);
    });
    const HeroOption& hero = CurrentHeroOption();
    const bool ringPage = UI::state.activeTab == UI::TAB_THEME;
    const bool slotPage = UI::state.activeTab == UI::TAB_AIMING && UI::state.aimingSubTab < 2;
    const char* source = L("Hero-specific settings");
    const char* description = L("Current settings edits the live configuration. Select a hero to edit its own schemes.");
    if (ringPage) {
        source = L("Shared settings");
        description = L("Shared ring styles; hero selects the preview schemes.");
    }
    else if (hero.heroId == 0) source = L("Current runtime settings");
    else if (slotPage) {
        OW::Config::HeroSlotPreset slot{};
        const bool present = UI::state.aimingSubTab == 0
            ? OW::Config::TryGetHeroAimSlot(hero.heroId, UI::state.aimHeroSegActive, slot)
            : OW::Config::TryGetHeroTriggerSlot(hero.heroId, UI::state.triggerHeroSegActive, slot);
        source = present ? L("Hero-specific scheme") : L("Current runtime settings");
        if (!present)
            description = L("Uses current settings until an independent scheme is saved.");
    }
    ImGui::TextDisabled("%s", source);
    SettingTooltip(description);
    ImGui::Spacing();
}

// =====================================================================
// UI::Render  --  Main entry point, called each frame.
//                 Draws the top bar, sub-tabs, and page body.
// =====================================================================
void UI::Render() {
    OW::Config::ScopedConfigEdit configEdit;
    EnsurePreNewFrameInitHook();
    if (!state.initialized) InitStyle();
    Theme::Apply();
    OW::Config::uiLanguage = Preferences::values.language;
    if(!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel))SyncSelectedTypeWithDetectedHero();
    if (!OW::Config::Menu) return;
    int page = 0;
    if(state.activeTab==TAB_AIMING) page=ImClamp(state.aimingSubTab,0,3);
    else if(state.activeTab==TAB_ALERT) page=4;
    else if(state.activeTab==TAB_KEYS) page=16;
    else if(state.activeTab==TAB_VISUALS) page=5;
    else if(state.activeTab==TAB_PREDICTION) page=17;
    else if(state.activeTab==TAB_THEME) page=6+ImClamp(state.themeSubTab,0,1);
    else { const int mapping[]={14,15,8,9,10,11,12,13}; page=mapping[ImClamp(state.miscSubTab,0,7)]; }
    // Keep historical page keys while routing retired duplicate pages to their owner.
    if(page==6)page=5;
    else if(page==9)page=8;
    const bool chinese=OW::Config::uiLanguage==OW::Config::kUiLanguageChineseSimplified;
    const Workspace::Page pages[] = {
        {chinese?"瞄准":"Aim", chinese?"英雄方案":"Hero schemes"},
        {chinese?"扳机":"Trigger", chinese?"英雄方案":"Hero schemes"},
        {chinese?"技能":"Skills", chinese?"英雄方案":"Hero schemes"},
        {chinese?"序列":"Sequences", chinese?"英雄方案":"Hero schemes"},
        {chinese?"预警":"Alert", chinese?"英雄方案":"Hero schemes"},
        {chinese?"画面":"Visuals", chinese?"显示":"Display"},
        {chinese?"主题":"Theme", chinese?"显示":"Display", false},
        {chinese?"范围圈":"FOV rings", chinese?"显示":"Display"},
        {chinese?"行为模式":"Behavior modes", chinese?"共享参数":"Shared settings"},
        {chinese?"运动方法":"Methods", chinese?"共享参数":"Shared settings", false},
        {chinese?"动态范围":"Dynamic FOV", chinese?"共享参数":"Shared settings"},
        {chinese?"输入与设备":"Input and devices", chinese?"系统":"System"},
        {chinese?"屏幕与字体":"Overlay and fonts", chinese?"系统":"System"},
        {chinese?"采集与轮廓":"Capture and contours", chinese?"系统":"System"},
        {chinese?"配置与界面":"Configuration", chinese?"系统":"System"},
        {chinese?"诊断":"Diagnostics", chinese?"系统":"System"},
        {chinese?"按键映射":"Action keys", chinese?"系统":"System"},
        {chinese?"预测数据":"Prediction Data", chinese?"共享运行数据":"Shared runtime data"}
    };
    if (!s_configSaveFailed && ImGui::GetTime() >= s_configSaveStatusUntil) s_configSaveStatus.clear();
    Workspace::Model model{pages,page,T(UiText::SaveAllChanges),s_configSaveStatus.c_str(),s_configSaveFailed};
    model.statusAge = float(ImGui::GetTime() - s_configSaveStatusStarted);
    model.connected = OW::ProcessConnection::IsConnected();
    const auto layout = V2::Profiles::Snapshot();
    const auto* matched = V2::Profiles::MatchedProfile(layout.detection);
    if (model.connected && matched) {
        model.region = matched->region;
        model.buildVersion = matched->build;
    }
    if (model.connected)
        model.pid = OW::ProcessConnection::ConnectedPid();
    const auto visSummary = BuildVisibilityStatusSummary(chinese);
    model.visibilityShort = visSummary.shortLabel;
    model.visibilityDotColor = visSummary.dotColor;
    model.brandIcon = reinterpret_cast<ImTextureID>(s_logoTexture);
    const float toolbarH = (std::max)(32.0f * (std::max)(1.0f, ImGui::GetFontSize() / 15.0f), ImGui::GetFrameHeight());
    const auto toolbar = MeasureHeroContextToolbar(toolbarH);
    model.headerWidth = toolbar.pillWidth;
    model.headerColumnWidth = toolbar.pillWidth;
    auto routePage = [&](int targetPage) {
        page = targetPage == 6 ? 5 : targetPage == 9 ? 8 : targetPage;
        model.selected = page;
        if (page<4) { state.activeTab=TAB_AIMING; state.aimingSubTab=page; }
        else if (page==4) state.activeTab=TAB_ALERT;
        else if (page==5) state.activeTab=TAB_VISUALS;
        else if (page==16) state.activeTab=TAB_KEYS;
        else if (page==17) state.activeTab=TAB_PREDICTION;
        else if (page<8) { state.activeTab=TAB_THEME; state.themeSubTab=page-6; }
        else { state.activeTab=TAB_MISC; const int mapping[]={2,3,4,5,6,7,0,1}; state.miscSubTab=mapping[page-8]; }
    };
    Workspace::Render(model, [] {
        SetPersistenceStatus(OW::Config::SaveAllConfig(OW::Config::ConfigPath()), T(UiText::SavedAllChanges));
    }, [&](int selected) {
        routePage(selected);
        DrawEditingContext();
        switch(page) {
        case 0: AimbotPage(); break; case 1: TriggerPage(); break;
        case 2: SkillsPage(); break; case 3: SequencesPage(); break;
        case 4: AlertPage(); break;
        case 5: case 6: VisualsPage(); break;
        case 7: DrawThemeFovRingsPage(); break; case 8: DrawMiscBehaviorPage(); break;
        case 9: DrawMiscBehaviorPage(); break; case 10: DrawMiscDynamicFovPage(); break;
        case 11: DrawMiscOutputPage(); break; case 12: DrawMiscScreenPage(); break;
        case 13: DrawMiscCapturePage(); break; case 14: DrawMiscGeneralPage(); break; case 15: DrawMiscDiagnosticsPage(); break;
        case 16: DrawHeroKeyPage(); break; case 17: DrawPredictionDataPage(); break;
        }
        CloseGroupBox();
    }, DrawHeroContextToolbar, [&](ImVec2 popupPos, ImVec2 popupSize, float scale, bool zh) {
        DrawNeverloseAccountPopup(model, routePage, popupPos, popupSize, scale, zh);
    });
    page=model.selected;
}
