#pragma once
#include <functional>
#include <span>
#include <string>
#include <imgui.h>

namespace UI::Workspace {
struct Page { const char* label; const char* group; bool visible = true; };
// 壳只拥有布局；配置编辑、保存、名片弹窗和页面业务通过回调交给各自所有者。
struct Model {
    std::span<const Page> pages;
    int selected = 0;
    const char* saveLabel = "保存全部";
    std::string status;
    bool saveFailed = false;
    ImTextureID brandIcon = 0;
    float headerWidth = 0;
    float headerColumnWidth = 0;
    float statusAge = 0;
    bool connected = false;
    std::string region;
    std::string buildVersion;
    int pid = 0;
    std::string visibilityShort;
    ImU32 visibilityDotColor = IM_COL32(113, 211, 151, 255);
};
void Render(Model& model, const std::function<void()>& save,
            const std::function<void(int)>& body,
            const std::function<void(float, bool)>& header = {},
            const std::function<void(ImVec2, ImVec2, float, bool)>& accountPopup = {});
}
