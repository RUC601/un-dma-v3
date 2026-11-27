#pragma once
#include <algorithm>
#include <cmath>
#include <imgui.h>
namespace UI::Theme {
inline ImU32 kColShell0       = IM_COL32(0x12, 0x13, 0x17, 0xFF);
inline ImU32 kColShell1       = IM_COL32(0x12, 0x13, 0x17, 0xFF);
inline ImU32 kColShell2       = IM_COL32(0x0b, 0x0c, 0x10, 0xFF);
inline ImU32 kColPanel        = IM_COL32(0x16, 0x17, 0x1c, 0xF8);
inline ImU32 kColPanelSoft    = IM_COL32(0x13, 0x14, 0x19, 0xF0);
inline ImU32 kColControl      = IM_COL32(0x1b, 0x20, 0x24, 0xFF);
inline ImU32 kColControlHover = IM_COL32(0x23, 0x28, 0x2e, 0xFF);
inline ImU32 kColControlHot   = IM_COL32(0x2a, 0x30, 0x37, 0xFF);
inline ImU32 kColStroke       = IM_COL32(0x34, 0x37, 0x40, 0xD0);
inline ImU32 kColStrokeDark   = IM_COL32(0x07, 0x08, 0x0a, 0xF0);
inline ImU32 kColText         = IM_COL32(0xff, 0xff, 0xff, 0xFF);
inline ImU32 kColTextMuted    = IM_COL32(0xe1, 0xe4, 0xea, 0xFF);
inline ImU32 kColTextDim      = IM_COL32(0xa8, 0xae, 0xb8, 0xFF);
inline ImU32 kColAccent       = IM_COL32(0xe4, 0x11, 0x43, 0xFF);
inline ImU32 kColAccentDark   = IM_COL32(0xa9, 0x0a, 0x2e, 0xFF);
inline ImU32 kColAccentSoft   = IM_COL32(0xe4, 0x11, 0x43, 0x58);
inline ImU32 kColAccentGlow   = IM_COL32(0xe4, 0x11, 0x43, 0x28);

extern ImFont* s_regularFont;
extern ImFont* s_boldFont;
extern ImFont* s_titleFont;

// Clamps base dark surface color luminance so light text stays readable regardless of picker choice.
inline ImVec4 SanitizeBaseDarkColor(ImVec4 color) noexcept {
    const auto safe = [](float v) noexcept {
        return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    };
    float r = safe(color.x);
    float g = safe(color.y);
    float b = safe(color.z);
    // Cap relative luminance at 0.175 so bright white/muted text always maintains high contrast.
    constexpr float kMaxBaseLuminance = 0.175f;
    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (lum > kMaxBaseLuminance && lum > 1e-5f) {
        const float scale = kMaxBaseLuminance / lum;
        r *= scale;
        g *= scale;
        b *= scale;
    }
    return ImVec4(r, g, b, 1.0f);
}

bool Initialize();
void Apply();
}
