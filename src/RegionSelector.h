// RegionSelector.h - modal full-virtual-screen overlay for rubber band selection.
#pragma once

#include "PluginSettings.h"

struct SelectionResult
{
    bool confirmed{false};
    RECT rect{}; // physical pixels, virtual screen coordinates
};

class CRegionSelector
{
public:
    CRegionSelector();
    ~CRegionSelector();

    CRegionSelector(const CRegionSelector&) = delete;
    CRegionSelector& operator=(const CRegionSelector&) = delete;

    // Blocks on a modal message loop until the user confirms or cancels.
    // hCancelEvent is polled every 100ms so a DLL shutdown interrupts it.
    SelectionResult Select(HANDLE hCancelEvent);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static bool EnsureClass();

    bool CreateOverlay();
    void DestroyOverlay();

    void FillMaskRect(const RECT& rect, bool insideSelection);
    void ComposeFull();
    void Repaint(bool full, const RECT& dirty);

    void DrawBand();
    void DrawCornerAccents();
    void DrawCrosshair();
    RECT DrawLabel(POINT anchor);
    void FixupAlpha(const RECT& rect);
    void Present(const RECT* dirty);

    void UpdateSelection(POINT current);
    void UpdateCrosshair(POINT current);
    RECT CurrentSelectionRect() const;
    void RequestFinish(bool confirmed);

    HWND m_hwnd{nullptr};
    HDC m_memDC{nullptr};
    HBITMAP m_hBitmap{nullptr};
    HGDIOBJ m_oldBitmap{nullptr};
    void* m_bits{nullptr};
    int m_width{0};
    int m_height{0};
    RECT m_virtualScreen{};
    bool m_layered{true};

    bool m_dragging{false};
    bool m_finished{false};
    bool m_confirmed{false};
    POINT m_origin{};
    POINT m_current{};
    RECT m_sel{};
    bool m_hasSelection{false};
    RECT m_lastLabelRect{};
    bool m_hasLabelRect{false};
    RECT m_cursorRect{};

    // 拖拽开始前的十字线光标（v1.2.0）：系统光标被隐藏，十字线画在覆盖层上。
    bool m_crosshairValid{false};
    POINT m_crosshair{-1, -1};
};
