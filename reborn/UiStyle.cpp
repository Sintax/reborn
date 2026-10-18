#include "UiStyle.hpp"

#include "imgui/imgui.h"

#include <cmath>

namespace UiStyle {
    namespace {
        const ImVec4 kPanel(0.06f, 0.08f, 0.10f, 0.92f);
        const ImVec4 kText(1.0f, 1.0f, 1.0f, 1.0f);
        const ImVec4 kTeal(0.10f, 0.72f, 0.80f, 1.0f);
        const ImVec4 kTealHover(0.30f, 0.84f, 0.90f, 1.0f);
        const ImVec4 kTealActive(0.07f, 0.58f, 0.66f, 1.0f);
        const ImVec4 kFrame(0.17f, 0.24f, 0.29f, 1.0f);
        const ImVec4 kFrameHover(0.22f, 0.33f, 0.39f, 1.0f);
        const float kBaseFont = 13.0f;   // ImGui's built-in font

        ImVec4 WithAlpha(ImVec4 c, float a) { c.w = a; return c; }

        float SrgbToLinear(float v) {
            return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        }

        float Undo(float v) { return SrgbToLinear(SrgbToLinear(v)); }

        // The game brightens what the overlay draws as if it were sRGB-encoded twice (measured
        // in a screenshot: teal 0.10/0.72/0.80 showed as 0.58/0.93/0.96 and the dark panel as
        // light grey). Undo that so the colours on screen are the ones written here. Alpha is
        // undone the same way for things drawn over the dark panel.
        ImVec4 Shown(ImVec4 c) { return ImVec4(Undo(c.x), Undo(c.y), Undo(c.z), Undo(c.w)); }

        // Window backgrounds: alpha is how much of the game shows through, undone from the other side.
        ImVec4 ShownBg(ImVec4 c) { return ImVec4(Undo(c.x), Undo(c.y), Undo(c.z), 1.0f - Undo(1.0f - c.w)); }
    }

    void Apply() {
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        ImGuiStyle& s = ImGui::GetStyle();

        // Frames are as tall as a button, so inputs, combos, sliders and buttons line up.
        const float fontPx = kBaseFont * FontScale(screen.y);
        s.FramePadding = ImVec2(10.0f, std::max(3.0f, (ButtonHeight(screen.y) - fontPx) * 0.5f));
        s.WindowPadding = ImVec2(16.0f, 16.0f);
        s.ItemSpacing = ImVec2(10.0f, 10.0f);
        s.WindowRounding = 6.0f;
        s.FrameRounding = 4.0f;
        s.GrabRounding = 4.0f;
        s.WindowBorderSize = 1.0f;
        s.DisabledAlpha = Undo(0.4f);

        auto set = [&s](ImGuiCol idx, ImVec4 colour) { s.Colors[idx] = Shown(colour); };
        auto setBg = [&s](ImGuiCol idx, ImVec4 colour) { s.Colors[idx] = ShownBg(colour); };
        set(ImGuiCol_Text, kText);
        set(ImGuiCol_TextDisabled, WithAlpha(kText, 0.4f));
        setBg(ImGuiCol_WindowBg, kPanel);
        set(ImGuiCol_ChildBg, WithAlpha(kPanel, 0.0f));
        setBg(ImGuiCol_PopupBg, WithAlpha(kPanel, 0.98f));
        set(ImGuiCol_Border, WithAlpha(kTeal, 0.35f));
        set(ImGuiCol_FrameBg, kFrame);
        set(ImGuiCol_FrameBgHovered, kFrameHover);
        set(ImGuiCol_FrameBgActive, kFrameHover);
        set(ImGuiCol_TitleBg, kPanel);
        set(ImGuiCol_TitleBgActive, WithAlpha(kTealActive, 0.9f));
        set(ImGuiCol_TitleBgCollapsed, kPanel);
        set(ImGuiCol_Button, kTeal);
        set(ImGuiCol_ButtonHovered, kTealHover);
        set(ImGuiCol_ButtonActive, kTealActive);
        set(ImGuiCol_Header, WithAlpha(kTeal, 0.55f));
        set(ImGuiCol_HeaderHovered, WithAlpha(kTealHover, 0.75f));
        set(ImGuiCol_HeaderActive, kTealActive);
        set(ImGuiCol_CheckMark, kTeal);
        set(ImGuiCol_SliderGrab, kTeal);
        set(ImGuiCol_SliderGrabActive, kTealHover);
        set(ImGuiCol_Tab, WithAlpha(kTeal, 0.35f));
        set(ImGuiCol_TabHovered, kTealHover);
        set(ImGuiCol_TabSelected, kTeal);
        set(ImGuiCol_TableHeaderBg, WithAlpha(kTeal, 0.35f));
        set(ImGuiCol_TableBorderStrong, WithAlpha(kTeal, 0.35f));
        set(ImGuiCol_TableBorderLight, WithAlpha(kTeal, 0.15f));
        set(ImGuiCol_TableRowBgAlt, WithAlpha(kText, 0.04f));
        set(ImGuiCol_Separator, WithAlpha(kTeal, 0.35f));
        set(ImGuiCol_ScrollbarGrab, WithAlpha(kTeal, 0.5f));
        set(ImGuiCol_ScrollbarGrabHovered, kTealHover);
        set(ImGuiCol_ScrollbarGrabActive, kTealActive);
    }

    void NextWindow(float heightShare) {
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        // Height 0 lets the window fit its contents while the width stays fixed, so wrapped text
        // has a width to wrap into.
        ImGui::SetNextWindowSize(ImVec2(WindowWidth(screen.x), screen.y * heightShare), ImGuiCond_Always);
    }

    void WindowFont() {
        ImGui::SetWindowFontScale(FontScale(ImGui::GetIO().DisplaySize.y));
    }

    bool Button(const char* label, float width) {
        return ImGui::Button(label, ImVec2(width, ButtonHeight(ImGui::GetIO().DisplaySize.y)));
    }

    float ButtonWidth(int n) {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        return (ImGui::GetContentRegionAvail().x - spacing * (n - 1)) / n;
    }
}
