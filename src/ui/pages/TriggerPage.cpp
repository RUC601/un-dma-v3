#include <Windows.h>
#include <shellapi.h>

#include "Features/UI.hpp"
#include "ui/WorkspaceShell.hpp"
#include "ui/SlotSelector.hpp"
#include "ui/Text.hpp"
#include "ui/Theme.hpp"
#include "config/UiPreferences.hpp"
#include "vision/ContourService.hpp"
using UI::Text::UiText;
using UI::Text::kUiLanguageOptions;
#include "output/DeviceCapabilities.hpp"
#include "game/profiles/ProfileState.hpp"
#include "Features/UiLabels.hpp"
#include "Features/UiValueFormat.hpp"
using namespace UI::ValueFormat;
#include "output/MockDiagnostics.hpp"
#include "output/OutputRuntime.hpp"
#include "Utils/Config.hpp"
#include "Game/HeroSkills.hpp"
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

#include "ui/EditorSupport.hpp"
#include "ui/AimPlanEditor.hpp"
using namespace UI::Theme;

void UI::TriggerPage() {
    state.selectedTypeIndex = ClampHeroSelectionIndex(state.selectedTypeIndex);
    const HeroOption* selectedHero = &kHeroOptions[state.selectedTypeIndex];
    constexpr ActionSlotKind slotKind = ActionSlotKind::Trigger;

    const int previousSlot = state.triggerHeroSegActive;
    const bool slotSelectorChanged = UIActionSlotSelector(slotKind, selectedHero->heroId);
    state.triggerHeroSegActive = ActiveHeroPresetSlotIndex(slotKind);
    if (slotSelectorChanged || previousSlot != state.triggerHeroSegActive)
        ApplySelectedTypePreset();

    bool presetChanged = false;
    bool slotEnabledChanged = false;
    OW::Config::HeroPreset activePreset{};

    auto refreshActivePreset = [&]() {
        state.selectedTypeIndex = ClampHeroSelectionIndex(state.selectedTypeIndex);
        selectedHero = &kHeroOptions[state.selectedTypeIndex];
        const bool isGlobal = selectedHero->heroId == 0;
        activePreset = isGlobal
            ? OW::Config::GetSystemHeroPreset()
            : GetHeroActionPresetOrDefault(slotKind, selectedHero->heroId, state.triggerHeroSegActive);
    };
    refreshActivePreset();

    PushHeroActionSlotControlId(slotKind, selectedHero->heroId, state.triggerHeroSegActive);

    UIGroupPair("Trigger Slot", [&]() {
        // The outer Trigger Slot switch is the only enable state for hero slots.
        bool triggerEnabled = selectedHero->heroId == 0
            ? activePreset.trigger.enabled
            : IsHeroActionSlotEnabled(slotKind, selectedHero->heroId, state.triggerHeroSegActive);
        SettingRow("Enable Trigger");
        if (UICheckbox("##triggerEnabled", &triggerEnabled)) {
            activePreset.trigger.enabled = triggerEnabled;
            if (selectedHero->heroId != 0)
                OW::Config::SetHeroTriggerSlotEnabled(selectedHero->heroId, state.triggerHeroSegActive, triggerEnabled);
            presetChanged = slotEnabledChanged = true;
        }
        SettingRow("Action");
        PushControlWidth();
        presetChanged |= UIAttackActionSelect("##triggerSlotAction", &activePreset.trigger.action, selectedHero->heroId);
        ImGui::PopItemWidth();

        SettingRow("Mode");
        PushControlWidth();
        if (UISelect("##triggerSlotMode", &activePreset.trigger.mode,
                     OW::Labels::kTriggerbotModes, OW::Labels::TriggerbotModeCount())) {
            if (activePreset.trigger.mode == 2)
                activePreset.trigger.key = OW::Labels::kAimActivationKeyNoneIndex;
            presetChanged = true;
        }
        ImGui::PopItemWidth();

        SettingRow("Activation Key");
        PushControlWidth();
        const bool triggerAlways = activePreset.trigger.mode == 2;
        if (triggerAlways) {
            activePreset.trigger.key = OW::Labels::kAimActivationKeyNoneIndex;
            ImGui::BeginDisabled();
        }
        presetChanged |= UIKeyBinding("##triggerSlotKey", &activePreset.trigger.key,
                                      KeyBindingStorage::AimActivationIndex);
        if (triggerAlways)
            ImGui::EndDisabled();
        ImGui::PopItemWidth();
    }, "Trigger Conditions", [&]() {
        SettingRow("Shot Interval");
        PushControlWidth();
        presetChanged |= UISlider("##triggerSlotInterval", &activePreset.trigger.shotInterval,
                                  0.0f, 100.0f, AtOrBelow("Auto", 0, Number(0, " ms", 5)));
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", L("0 uses trigger_cycle_interval_ms for the current WeaponSpec. A non-zero value remains the manual override."));

        SettingRow("Disable During Reload");
        presetChanged |= UICheckbox(
            "##triggerSlotDisableWhileReloading",
            &activePreset.trigger.disableWhileReloading);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", L("Skip this Trigger Slot while the local hero is reloading."));

        SettingRow("Skeleton");
        PushControlWidth();
        presetChanged |= UISkeletonBoneMultiSelect(
            "##triggerSlotSkeleton",
            &activePreset.trigger.boneMask);
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", L("Choose All or combine groups, for example Head + Feet."));
    });

    UIGroupPair("Target Filter", [&]() {
        SettingRow("Target Team");
        PushControlWidth();
        presetChanged |= UISelect("##triggerSlotTeam", &activePreset.targetTeam,
                                  kTeam, IM_ARRAYSIZE(kTeam));
        ImGui::PopItemWidth();

        if (activePreset.targetTeam != 0) {
            SettingRow(L("Ally key"));
            PushControlWidth();
            presetChanged |= UIKeyBinding("##allyKey", &activePreset.allyKey,
                                          KeyBindingStorage::AimActivationIndex);
            ImGui::PopItemWidth();
            SettingRow(L("Ally HP limit"));
            PushControlWidth();
            presetChanged |= UISlider("##allyHp", &activePreset.allyHealthMaxPercent,0.f,100.f,Number(0," %"));
            ImGui::PopItemWidth();
            SettingRow(L("Ally skeleton"));
            PushControlWidth();
            presetChanged |= UISkeletonBoneMultiSelect("##allyBones",&activePreset.allyBoneMask);
            ImGui::PopItemWidth();
        }
        SettingRow(L("Protection filter"));
        presetChanged |= UICheckbox("##targetableFilter",&activePreset.requireTargetable);
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",L("Existing protection signals; full targetability pending validation."));
        SettingRow(L("Visibility"));
        PushControlWidth();
        static const char* const kVisibility[] = { "Visible Only", "All" };
        int visibility = activePreset.trigger.ignoreInvisible ? 0 : 1;
        if (UISelect("##triggerSlotVisibility", &visibility, kVisibility, IM_ARRAYSIZE(kVisibility))) {
            activePreset.trigger.ignoreInvisible = visibility == 0;
            presetChanged = true;
        }
        ImGui::PopItemWidth();

        {
            const bool zh = OW::Config::uiLanguage == 1;
            const int visMode = UI::Preferences::ActiveVisibilityMode();
            const char* modeDesc =
                visMode == static_cast<int>(UI::Preferences::VisibilityMode::Ray)
                    ? (zh ? "射线 (Ray) · 严格 Fresh Clear" : "Ray · strict Fresh Clear")
                : visMode == static_cast<int>(UI::Preferences::VisibilityMode::Vision)
                    ? (zh ? "轮廓 (Vision) · 全局作用于 Trigger" : "Vision (Contour) · global for Trigger")
                    : (zh ? "实体 (Lite) · 仅实体级可见性" : "Lite · entity Vis only");
            SettingRow(zh ? "可见性来源" : "Visibility source");
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", modeDesc);
            if (visMode == static_cast<int>(UI::Preferences::VisibilityMode::Vision)) {
                V2::Vision::Settings captureSettings;
                const auto captureFrame = V2::Vision::GetService().Snapshot(captureSettings);
                const auto captureStatus = V2::Vision::GetService().Status();
                const char* captureMessage = nullptr;
                if (!captureSettings.enabled || captureStatus.state == V2::Vision::ServiceState::Stopped)
                    captureMessage = zh ? "轮廓采集已停止：请从左下角名片或采集页启动。" : "Contour capture stopped: start it from the account menu or Capture page.";
                else if (captureStatus.state == V2::Vision::ServiceState::Faulted)
                    captureMessage = zh ? "轮廓采集失败：请检查采集卡设备。" : "Contour capture failed: check the capture device.";
                else if (captureStatus.state == V2::Vision::ServiceState::Starting)
                    captureMessage = zh ? "轮廓采集连接中…" : "Contour capture starting...";
                else if (!captureFrame)
                    captureMessage = zh ? "轮廓采集等待新鲜画面…" : "Contour capture waiting for a fresh frame...";
                if (captureMessage) {
                    SettingRow(zh ? "采集状态" : "Capture status");
                    ImGui::AlignTextToFramePadding();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.64f, 0.25f, 1.0f));
                    ImGui::TextWrapped("%s", captureMessage);
                    ImGui::PopStyleColor();
                }
            }
            if (visMode == static_cast<int>(UI::Preferences::VisibilityMode::Ray)) {
                SettingRow(L("Barrier policy"));
                const char* barrierPolicies[] = { L("Ignore barrier"), L("Wait barrier drop") };
                activePreset.barrierPolicy = static_cast<int>(OW::ClampBarrierPolicy(activePreset.barrierPolicy));
                PushControlWidth();
                presetChanged |= UISelect("##triggerBarrierPolicy", &activePreset.barrierPolicy,
                                          barrierPolicies, IM_ARRAYSIZE(barrierPolicies));
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", L("Wait barrier drop holds Trigger on dynamic shields (Slot 3) and fires once the barrier drops."));
                const uint64_t barrierMask = OW::RayRuntime::BarrierRayMask();
                const int activeRayBackend = UI::Preferences::ActiveRayBackend();
                if (OW::RayPolicyNeedsBarrierNotice(activeRayBackend,
                                                    OW::ClampBarrierPolicy(activePreset.barrierPolicy),
                                                    barrierMask)) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.30f, 1.0f));
                    ImGui::TextWrapped("%s",
                        zh ? "当前构建尚无实机验证的动态屏障掩码（rayBarrierMask=0），Trigger 仅执行世界层射线校验。"
                           : "Active build has no live-verified barrier mask yet (rayBarrierMask=0); Trigger uses world-only ray checks.");
                    ImGui::PopStyleColor();
                }
            }
        }

        SettingRow("Fast Target Limit");
        PushControlWidth();
        presetChanged |= UISlider("##triggerMaxTargetSpeed", &activePreset.maxTargetSpeedMps,
                                  0.0f, 100.0f, AtOrBelow("Unlimited", 0, Number(1, " m/s")));
        ImGui::PopItemWidth();
    }, "Target preference", [&]() {
        presetChanged |= UI::AimPlanEditor::Policy(activePreset, false);
        SettingRow(OW::Config::uiLanguage == 1 ? "高阶参数" : "Advanced");
        const float avail = MaxFloat(80.0f, ImGui::GetContentRegionAvail().x - kControlRightPadding);
        if (ImGui::Button(L("Edit advanced parameters..."), ImVec2(avail, ControlHeight())))
            ImGui::OpenPopup("##TriggerAdvanced");
    });

    const auto viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowSizeConstraints(ImVec2(300,180),ImVec2(viewport->WorkSize.x-24,viewport->WorkSize.y-24));
    ImGui::SetNextWindowSize(ImVec2((std::min)(650.f,viewport->WorkSize.x-24),380),ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##TriggerAdvanced",ImGuiWindowFlags_NoSavedSettings)) {
        UIGroupPair("Timing & Prediction", [&]() {
            presetChanged |= UIPredictionAlgorithmSelect("##triggerPredictionAlgorithm",
                &activePreset.predictionAlgorithm);
            SettingRow("Charge Aware");
            presetChanged |= UICheckbox("##triggerSlotChargeAware", &activePreset.trigger.chargeAware);

            if (activePreset.trigger.chargeAware) {
                SettingRow("Min Charge");
                PushControlWidth();
                presetChanged |= UISlider("##triggerSlotMinCharge", &activePreset.trigger.minCharge,
                                          0.0f, 100.0f, Number(2, " %"));
                ImGui::PopItemWidth();
            }
        }, "Range & Target", [&]() {
            SettingRow("Detection Scale");
            PushControlWidth();
            presetChanged |= UISlider("##triggerSlotHitbox", &activePreset.hitbox,
                                      OW::Config::kMinHitboxScalePercent,
                                      OW::Config::kMaxHitboxScalePercent, Number(0, " %"));
            ImGui::PopItemWidth();
        });
        ImGui::EndPopup();
    }

    if (selectedHero->heroId == 0) {
        if (presetChanged)
            OW::Config::SetSystemTriggerPreset(activePreset);
    } else {
        if (presetChanged) {
            SetHeroActionPreset(slotKind, selectedHero->heroId, state.triggerHeroSegActive, activePreset);
            ApplyHeroActionPresetToGlobals(slotKind, activePreset);
        } else if (slotEnabledChanged &&
                   IsHeroActionSlotEnabled(slotKind, selectedHero->heroId, state.triggerHeroSegActive)) {
            ApplyHeroActionPresetToGlobals(slotKind, activePreset);
        }
    }

    ImGui::PopID();
}
