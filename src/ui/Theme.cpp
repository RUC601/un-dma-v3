#include "Theme.hpp"
#include "config/UiPreferences.hpp"
#include "Utils/Config.hpp"
#include <algorithm>
#include <cmath>
namespace UI::Theme {
ImFont* s_regularFont = nullptr;
ImFont* s_boldFont = nullptr;
ImFont* s_titleFont = nullptr;

namespace {
ImU32 ShiftSurface(ImVec4 base, float delta, float alpha = 1.0f) noexcept {
    const float r = std::clamp(base.x + delta, 0.0f, 1.0f);
    const float g = std::clamp(base.y + delta, 0.0f, 1.0f);
    const float b = std::clamp(base.z + delta, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, std::clamp(alpha, 0.0f, 1.0f)));
}

ImVec4 ClampAccent(ImVec4 c) noexcept {
    const auto safe = [](float v) noexcept {
        return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    };
    return ImVec4(safe(c.x), safe(c.y), safe(c.z), 1.0f);
}
} // namespace

bool Initialize() {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigErrorRecoveryEnableTooltip = false;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    if (!s_regularFont && !io.Fonts->Locked) {
        ImFontConfig fontConfig;
        fontConfig.OversampleH = 3;
        fontConfig.OversampleV = 2;
        fontConfig.PixelSnapH = false;
        fontConfig.RasterizerMultiply = 1.12f;
        auto addUiFont = [&](const char* primaryPath, const char* fallbackPath, float size) {
            ImFont* font = io.Fonts->AddFontFromFileTTF(primaryPath, size, &fontConfig);
            if (font) {
                ImFontConfig fallbackConfig;
                fallbackConfig.MergeMode = true;
                fallbackConfig.OversampleH = fontConfig.OversampleH;
                fallbackConfig.OversampleV = fontConfig.OversampleV;
                fallbackConfig.PixelSnapH = fontConfig.PixelSnapH;
                fallbackConfig.RasterizerMultiply = fontConfig.RasterizerMultiply;
                io.Fonts->AddFontFromFileTTF(fallbackPath, size, &fallbackConfig,
                                             io.Fonts->GetGlyphRangesChineseFull());
            }
            return font;
        };
        s_regularFont = addUiFont("C:\\Windows\\Fonts\\segoeui.ttf",
                                  "C:\\Windows\\Fonts\\msyh.ttc", 15.0f);
        s_boldFont = addUiFont("C:\\Windows\\Fonts\\segoeuib.ttf",
                               "C:\\Windows\\Fonts\\msyhbd.ttc", 15.0f);
        s_titleFont = addUiFont("C:\\Windows\\Fonts\\segoeuib.ttf",
                                "C:\\Windows\\Fonts\\msyhbd.ttc", 18.0f);
        if (!s_titleFont)
            s_titleFont = s_boldFont;
        if (s_regularFont)
            io.FontDefault = s_regularFont;
    } else if (s_regularFont) {
        io.FontDefault = s_regularFont;
    }

    Preferences::Load(OW::Config::uiLanguage);
    Apply();
    return s_regularFont != nullptr || !io.Fonts->Locked;
}

void Apply() {
    const auto& preferences = Preferences::values;
    const ImVec4 base = SanitizeBaseDarkColor(preferences.baseDark);
    const ImVec4 accent = ClampAccent(preferences.accent);

    kColShell0       = ShiftSurface(base, 0.0f);
    kColShell1       = ShiftSurface(base, 0.012f);
    kColShell2       = ShiftSurface(base, -0.022f);
    kColPanel        = ShiftSurface(base, 0.036f);
    kColPanelSoft    = (kColPanel & ~IM_COL32_A_MASK) | ((preferences.consoleLayers ? 225u : 255u) << IM_COL32_A_SHIFT);
    kColControl      = ShiftSurface(base, 0.072f);
    kColControlHover = ShiftSurface(base, 0.106f);
    kColControlHot   = ShiftSurface(base, 0.138f);
    kColStroke       = ShiftSurface(base, 0.145f);
    kColText         = IM_COL32(240, 241, 245, 255);
    kColTextMuted    = IM_COL32(218, 221, 230, 255);
    kColTextDim      = IM_COL32(169, 174, 187, 255);

    kColAccent     = ImGui::ColorConvertFloat4ToU32(accent);
    kColAccentDark = ImGui::ColorConvertFloat4ToU32(ImVec4(
        std::clamp(base.x * 0.65f + accent.x * 0.28f, 0.0f, 1.0f),
        std::clamp(base.y * 0.65f + accent.y * 0.28f, 0.0f, 1.0f),
        std::clamp(base.z * 0.65f + accent.z * 0.28f, 0.0f, 1.0f), 1.0f));
    kColAccentSoft = (kColAccent & ~IM_COL32_A_MASK) | (56u << IM_COL32_A_SHIFT);
    kColAccentGlow = (kColAccent & ~IM_COL32_A_MASK) | (34u << IM_COL32_A_SHIFT);

    const ImVec4 accentHot(
        std::clamp(accent.x * 1.12f + 0.04f, 0.0f, 1.0f),
        std::clamp(accent.y * 1.12f + 0.04f, 0.0f, 1.0f),
        std::clamp(accent.z * 1.12f + 0.04f, 0.0f, 1.0f), 1.0f);

    ImGuiStyle& style   = ImGui::GetStyle();
    const float scale = (std::max)(1.f, style.FontScaleDpi);
    style.AntiAliasedLines = true;
    style.AntiAliasedFill  = true;
    style.WindowRounding    = 0.0f;
    style.FrameRounding     = 7.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 6.0f;
    style.WindowBorderSize  = 0.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupRounding     = 10.0f;
    style.PopupBorderSize   = 1.0f;
    style.WindowPadding     = ImVec2(0, 0);
    style.FramePadding      = ImVec2(8 * scale, 5 * scale);
    style.ItemSpacing       = ImVec2(8 * scale, 6 * scale);
    style.ItemInnerSpacing  = ImVec2(6 * scale, 4 * scale);
    style.ScrollbarSize     = 7.0f * scale;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]            = ImGui::ColorConvertU32ToFloat4(kColShell0);
    colors[ImGuiCol_FrameBg]             = ImGui::ColorConvertU32ToFloat4(kColControl);
    colors[ImGuiCol_FrameBgHovered]      = ImGui::ColorConvertU32ToFloat4(kColControlHover);
    colors[ImGuiCol_FrameBgActive]       = ImGui::ColorConvertU32ToFloat4(kColControlHot);
    colors[ImGuiCol_Button]              = ImGui::ColorConvertU32ToFloat4(kColControl);
    colors[ImGuiCol_ButtonHovered]       = ImGui::ColorConvertU32ToFloat4(kColControlHover);
    colors[ImGuiCol_ButtonActive]        = ImGui::ColorConvertU32ToFloat4(kColControlHot);
    colors[ImGuiCol_Header]              = ImGui::ColorConvertU32ToFloat4(kColControl);
    colors[ImGuiCol_HeaderHovered]       = ImGui::ColorConvertU32ToFloat4(kColAccent);
    colors[ImGuiCol_HeaderActive]        = ImGui::ColorConvertU32ToFloat4(kColAccentDark);
    colors[ImGuiCol_CheckMark]           = ImGui::ColorConvertU32ToFloat4(kColAccent);
    colors[ImGuiCol_SliderGrab]          = ImGui::ColorConvertU32ToFloat4(kColAccent);
    colors[ImGuiCol_SliderGrabActive]    = accentHot;
    colors[ImGuiCol_Text]                = ImGui::ColorConvertU32ToFloat4(kColText);
    colors[ImGuiCol_TextDisabled]        = ImGui::ColorConvertU32ToFloat4(kColTextDim);
    colors[ImGuiCol_Border]              = ImGui::ColorConvertU32ToFloat4(kColStroke);
    colors[ImGuiCol_TitleBg]             = ImGui::ColorConvertU32ToFloat4(kColPanel);
    colors[ImGuiCol_TitleBgActive]       = ImGui::ColorConvertU32ToFloat4(kColPanel);
    colors[ImGuiCol_Separator]           = ImGui::ColorConvertU32ToFloat4(kColStroke);
    colors[ImGuiCol_ScrollbarBg]         = ImGui::ColorConvertU32ToFloat4(IM_COL32(0x0a, 0x0b, 0x0e, 0xE0));
    colors[ImGuiCol_ScrollbarGrab]       = ImGui::ColorConvertU32ToFloat4(IM_COL32(0x34, 0x37, 0x40, 0xD8));
    colors[ImGuiCol_ScrollbarGrabHovered]= ImGui::ColorConvertU32ToFloat4(kColAccent);
    colors[ImGuiCol_PopupBg]             = ImGui::ColorConvertU32ToFloat4(kColPanel);
    colors[ImGuiCol_NavCursor]           = ImVec4(1.0f, 0.86f, 0.40f, 1.0f);
    colors[ImGuiCol_TextSelectedBg]      = ImGui::ColorConvertU32ToFloat4(kColAccentSoft);
}

}
