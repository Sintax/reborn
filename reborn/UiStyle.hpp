#pragma once
#include <algorithm>

// One look for every overlay window: dark panels, white text, the game's teal for buttons and
// headers, one font size and one button height. The size math is inline so it can be unit tested
// without ImGui.
namespace UiStyle {
    // Every window is 40 % of the screen width.
    inline float WindowWidth(float screenW) { return screenW * 2.0f / 5.0f; }

    // Buttons (and every other frame) are 4 % of the screen height, kept within 28..56 px.
    inline float ButtonHeight(float screenH) { return std::clamp(screenH / 25.0f, 28.0f, 56.0f); }

    // The one text size, as a multiple of ImGui's 13 px font: 2x at 1080p, never below 1x.
    inline float FontScale(float screenH) { return std::clamp(screenH / 540.0f, 1.0f, 2.5f); }

    // Sets the shared colours and frame sizes; call once per frame before any window.
    void Apply();

    // Centres the next window and gives it the shared width; heightShare 0 fits the height to
    // the contents, otherwise it is that share of the screen height.
    void NextWindow(float heightShare = 0.0f);

    // Inside a window, right after Begin: the shared text size.
    void WindowFont();

    // A button of the shared height. width 0 fits the label, -1 fills the row.
    bool Button(const char* label, float width = -1.0f);

    // The width of one of n buttons sharing the rest of the row.
    float ButtonWidth(int n);
}
