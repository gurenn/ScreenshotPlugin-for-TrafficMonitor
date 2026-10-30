// RegionSelector.cpp - layered overlay window, rubber band, modal loop.
#include "RegionSelector.h"

namespace
{

RECT ExpandRect(const RECT& rect, int margin)
{
    RECT out;
    out.left = rect.left - margin;
    out.top = rect.top - margin;
    out.right = rect.right + margin;
    out.bottom = rect.bottom + margin;
    return out;
}

void ClampRectTo(RECT& rect, int width, int height)
{
    if (rect.left < 0)
    {
        rect.left = 0;
    }
    if (rect.top < 0)
    {
        rect.top = 0;
    }
    if (rect.right > width)
    {
        rect.right = width;
    }
    if (rect.bottom > height)
    {
        rect.bottom = height;
    }
    if (rect.right < rect.left)
    {
        rect.right = rect.left;
    }
    if (rect.bottom < rect.top)
    {
        rect.bottom = rect.top;
    }
}

bool RectIsEmpty(const RECT& rect)
{
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

} // namespace

CRegionSelector::CRegionSelector()
{
}

CRegionSelector::~CRegionSelector()
{
    DestroyOverlay();
}

bool CRegionSelector::EnsureClass()
{
    static bool registered = false;
    if (registered)
    {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &CRegionSelector::WndProc;
    wc.hInstance = g_hDllInstance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32515)); // IDC_CROSS
    wc.lpszClassName = TMSNAP_OVERLAY_CLASS;
    if (RegisterClassExW(&wc) == 0)
    {
        DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS)
        {
            SNAP_LOGE(kErrInternal, L"RegisterClassEx(%s) failed: %d", TMSNAP_OVERLAY_CLASS,
                      static_cast<int>(error));
            return false;
        }
    }
    registered = true;
    return true;
}

bool CRegionSelector::CreateOverlay()
{
    m_virtualScreen = snap::VirtualScreenRect();
    m_width = static_cast<int>(m_virtualScreen.right - m_virtualScreen.left);
    m_height = static_cast<int>(m_virtualScreen.bottom - m_virtualScreen.top);
    if (m_width <= 0 || m_height <= 0)
    {
        SNAP_LOGE(kErrInternal, L"virtual screen is empty");
        return false;
    }

    m_layered = static_cast<SIZE_T>(m_width) * static_cast<SIZE_T>(m_height) * 4 <= snap::kMaxOverlayBytes;

    HDC screenDC = GetDC(nullptr);
    if (screenDC == nullptr)
    {
        SNAP_LOGE(kErrCaptureDC, L"GetDC(NULL) failed for the overlay");
        return false;
    }

    m_memDC = CreateCompatibleDC(screenDC);
    if (m_memDC == nullptr)
    {
        ReleaseDC(nullptr, screenDC);
        SNAP_LOGE(kErrNoMemory, L"CreateCompatibleDC failed for the overlay");
        return false;
    }

    if (m_layered)
    {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = m_width;
        bmi.bmiHeader.biHeight = -m_height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        m_hBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &m_bits, nullptr, 0);
        if (m_hBitmap == nullptr || m_bits == nullptr)
        {
            m_layered = false;
            if (m_hBitmap != nullptr)
            {
                DeleteObject(m_hBitmap);
                m_hBitmap = nullptr;
            }
            m_bits = nullptr;
        }
        else
        {
            m_oldBitmap = SelectObject(m_memDC, m_hBitmap);
        }
    }

    ReleaseDC(nullptr, screenDC);

    if (!m_layered)
    {
        SNAP_LOG(L"overlay falls back to LWA_ALPHA (DIB allocation failed or screen too large)");
    }

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    m_hwnd = CreateWindowExW(exStyle, TMSNAP_OVERLAY_CLASS, L"", WS_POPUP, static_cast<int>(m_virtualScreen.left),
                             static_cast<int>(m_virtualScreen.top), m_width, m_height, nullptr, nullptr,
                             g_hDllInstance, this);
    if (m_hwnd == nullptr)
    {
        SNAP_LOGE(kErrInternal, L"CreateWindowEx(%s) failed", TMSNAP_OVERLAY_CLASS);
        return false;
    }
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    if (m_layered)
    {
        ComposeFull();
    }
    else
    {
        SetLayeredWindowAttributes(m_hwnd, 0, 120, LWA_ALPHA);
    }
    return true;
}

void CRegionSelector::DestroyOverlay()
{
    m_bits = nullptr;
    if (m_memDC != nullptr)
    {
        // The DIB must be deselected, otherwise DeleteDC / DeleteObject both fail
        // and every region selection would leak one DC plus one bitmap.
        if (m_oldBitmap != nullptr)
        {
            SelectObject(m_memDC, m_oldBitmap);
            m_oldBitmap = nullptr;
        }
        DeleteDC(m_memDC);
        m_memDC = nullptr;
    }
    if (m_hBitmap != nullptr)
    {
        DeleteObject(m_hBitmap);
        m_hBitmap = nullptr;
    }
    if (m_hwnd != nullptr)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void CRegionSelector::FillMaskRect(const RECT& rectIn, bool insideSelection)
{
    if (m_bits == nullptr)
    {
        return;
    }
    RECT rect = rectIn;
    ClampRectTo(rect, m_width, m_height);
    if (RectIsEmpty(rect))
    {
        return;
    }
    const unsigned char alpha = insideSelection ? 0 : 120;
    for (int y = rect.top; y < rect.bottom; ++y)
    {
        unsigned char* row = static_cast<unsigned char*>(m_bits) + static_cast<size_t>(y) * static_cast<size_t>(m_width) * 4;
        for (int x = rect.left; x < rect.right; ++x)
        {
            unsigned char* pixel = row + static_cast<size_t>(x) * 4;
            pixel[0] = 0;
            pixel[1] = 0;
            pixel[2] = 0;
            pixel[3] = alpha;
        }
    }
}

void CRegionSelector::FixupAlpha(const RECT& rectIn)
{
    if (m_bits == nullptr)
    {
        return;
    }
    RECT rect = rectIn;
    ClampRectTo(rect, m_width, m_height);
    if (RectIsEmpty(rect))
    {
        return;
    }
    // GDI never writes the alpha byte of a 32bpp DIB.  Every pixel we painted
    // (band, label background, label text) has a non-zero RGB value, therefore
    // "RGB != 0 -> A = 255" turns exactly those pixels opaque and leaves the
    // semi transparent mask untouched.
    for (int y = rect.top; y < rect.bottom; ++y)
    {
        unsigned char* row = static_cast<unsigned char*>(m_bits) + static_cast<size_t>(y) * static_cast<size_t>(m_width) * 4;
        for (int x = rect.left; x < rect.right; ++x)
        {
            unsigned char* pixel = row + static_cast<size_t>(x) * 4;
            if ((pixel[0] | pixel[1] | pixel[2]) != 0)
            {
                pixel[3] = 255;
            }
        }
    }
}

void CRegionSelector::DrawBand()
{
    if (!m_dragging || RectIsEmpty(m_sel))
    {
        return;
    }
    HPEN whitePen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HPEN darkPen = CreatePen(PS_SOLID, 1, RGB(48, 48, 48));
    HGDIOBJ oldPen = SelectObject(m_memDC, whitePen);
    HGDIOBJ oldBrush = SelectObject(m_memDC, GetStockObject(NULL_BRUSH));

    Rectangle(m_memDC, m_sel.left, m_sel.top, m_sel.right, m_sel.bottom);
    SelectObject(m_memDC, darkPen);
    Rectangle(m_memDC, m_sel.left + 1, m_sel.top + 1, m_sel.right - 1, m_sel.bottom - 1);

    SelectObject(m_memDC, oldBrush);
    SelectObject(m_memDC, oldPen);
    DeleteObject(whitePen);
    DeleteObject(darkPen);

    DrawCornerAccents();
}

void CRegionSelector::DrawCornerAccents()
{
    // v1.2.0：四角 L 形高亮角标（选择手柄），套在白色描边之上，现代截图工具的标配视觉。
    if (!m_dragging || RectIsEmpty(m_sel))
    {
        return;
    }
    const LONG length = 20;
    const LONG thick = 3;
    const COLORREF accent = RGB(0, 145, 255);
    HPEN pen = CreatePen(PS_SOLID, thick, accent);
    HGDIOBJ oldPen = SelectObject(m_memDC, pen);
    HGDIOBJ oldBrush = SelectObject(m_memDC, GetStockObject(NULL_BRUSH));

    const LONG left = m_sel.left - 1;
    const LONG top = m_sel.top - 1;
    const LONG right = m_sel.right + 1;
    const LONG bottom = m_sel.bottom + 1;

    MoveToEx(m_memDC, left, top + length, nullptr);
    LineTo(m_memDC, left, top);
    LineTo(m_memDC, left + length, top);

    MoveToEx(m_memDC, right - length, top, nullptr);
    LineTo(m_memDC, right, top);
    LineTo(m_memDC, right, top + length);

    MoveToEx(m_memDC, right, bottom - length, nullptr);
    LineTo(m_memDC, right, bottom);
    LineTo(m_memDC, right - length, bottom);

    MoveToEx(m_memDC, left + length, bottom, nullptr);
    LineTo(m_memDC, left, bottom);
    LineTo(m_memDC, left, bottom - length);

    SelectObject(m_memDC, oldBrush);
    SelectObject(m_memDC, oldPen);
    DeleteObject(pen);
}

void CRegionSelector::DrawCrosshair()
{
    // v1.2.0：拖拽开始前的全屏十字线光标（系统光标已隐藏）。
    // 白色 1px 主线 + 深灰 1px 投影（在任何底色上都清晰），中心挖空半径 10px 并画
    // 一个圆环 + 中心点，避免线压住用户想看清的像素。全部不透明，走 FixupAlpha 规则。
    if (m_dragging || !m_crosshairValid)
    {
        return;
    }
    const LONG x = m_crosshair.x;
    const LONG y = m_crosshair.y;
    const LONG gap = 10;

    HPEN whitePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN shadowPen = CreatePen(PS_SOLID, 1, RGB(40, 40, 40));
    HGDIOBJ oldPen = SelectObject(m_memDC, shadowPen);
    HGDIOBJ oldBrush = SelectObject(m_memDC, GetStockObject(NULL_BRUSH));

    // 影线（偏移 1px）
    MoveToEx(m_memDC, 0, y + 1, nullptr);
    LineTo(m_memDC, m_width, y + 1);
    MoveToEx(m_memDC, x + 1, 0, nullptr);
    LineTo(m_memDC, x + 1, m_height);

    SelectObject(m_memDC, whitePen);
    // 主线：左右两段 + 上下两段，中间留 gap
    MoveToEx(m_memDC, 0, y, nullptr);
    LineTo(m_memDC, x - gap, y);
    MoveToEx(m_memDC, x + gap, y, nullptr);
    LineTo(m_memDC, m_width, y);
    MoveToEx(m_memDC, x, 0, nullptr);
    LineTo(m_memDC, x, y - gap);
    MoveToEx(m_memDC, x, y + gap, nullptr);
    LineTo(m_memDC, x, m_height);

    // 中心圆环
    Ellipse(m_memDC, x - gap, y - gap, x + gap, y + gap);

    // 中心点
    SetPixelV(m_memDC, x, y, RGB(255, 255, 255));

    SelectObject(m_memDC, oldBrush);
    SelectObject(m_memDC, oldPen);
    DeleteObject(whitePen);
    DeleteObject(shadowPen);
}

RECT CRegionSelector::DrawLabel(POINT anchor)
{
    RECT box{};
    if (RectIsEmpty(m_sel))
    {
        return box;
    }
    const std::wstring text = snap::FormatString(L"%d \u00D7 %d", static_cast<int>(m_sel.right - m_sel.left),
                                                 static_cast<int>(m_sel.bottom - m_sel.top));

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ oldFont = SelectObject(m_memDC, font);

    SIZE extent{};
    GetTextExtentPoint32W(m_memDC, text.c_str(), static_cast<int>(text.size()), &extent);

    const int boxWidth = static_cast<int>(extent.cx) + 14;
    const int boxHeight = static_cast<int>(extent.cy) + 10;
    int x = anchor.x + 16;
    int y = anchor.y + 16;
    if (x + boxWidth > m_width)
    {
        x = anchor.x - 16 - boxWidth;
    }
    if (y + boxHeight > m_height)
    {
        y = anchor.y - 16 - boxHeight;
    }
    if (x < 0)
    {
        x = 0;
    }
    if (y < 0)
    {
        y = 0;
    }
    box.left = x;
    box.top = y;
    box.right = x + boxWidth;
    box.bottom = y + boxHeight;

    // v1.2.0：尺寸提示升级为圆角胶囊（深色底 + 细灰边），与十字线/角标同一套视觉语言。
    HBRUSH background = CreateSolidBrush(RGB(20, 20, 20));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(96, 96, 96));
    HGDIOBJ oldBrush = SelectObject(m_memDC, background);
    HGDIOBJ oldPen = SelectObject(m_memDC, border);
    RoundRect(m_memDC, box.left, box.top, box.right + 1, box.bottom + 1, 10, 10);
    SelectObject(m_memDC, oldBrush);
    SelectObject(m_memDC, oldPen);
    DeleteObject(background);
    DeleteObject(border);

    SetBkMode(m_memDC, TRANSPARENT);
    SetTextColor(m_memDC, RGB(255, 255, 255));
    RECT textRect = box;
    textRect.left += 8;
    textRect.top += 5;
    DrawTextW(m_memDC, text.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(m_memDC, oldFont);
    return box;
}

void CRegionSelector::Present(const RECT* dirty)
{
    if (m_hwnd == nullptr)
    {
        return;
    }
    if (!m_layered)
    {
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    HDC screenDC = GetDC(nullptr);
    if (screenDC == nullptr)
    {
        return;
    }

    POINT source{0, 0};
    POINT destination{static_cast<LONG>(m_virtualScreen.left), static_cast<LONG>(m_virtualScreen.top)};
    SIZE size{static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UPDATELAYEREDWINDOWINFO info{};
    info.cbSize = sizeof(info);
    info.hdcDst = screenDC;
    info.pptDst = &destination;
    info.psize = &size;
    info.hdcSrc = m_memDC;
    info.pptSrc = &source;
    info.crKey = 0;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = dirty;

    UpdateLayeredWindowIndirect(m_hwnd, &info);
    ReleaseDC(nullptr, screenDC);
}

void CRegionSelector::Repaint(bool full, const RECT& dirtyIn)
{
    if (m_memDC == nullptr)
    {
        return;
    }
    RECT dirty = dirtyIn;
    if (full)
    {
        dirty.left = 0;
        dirty.top = 0;
        dirty.right = m_width;
        dirty.bottom = m_height;
    }
    ClampRectTo(dirty, m_width, m_height);
    if (RectIsEmpty(dirty))
    {
        return;
    }

    // 1) Base mask for the dirty area.
    FillMaskRect(dirty, false);
    if (m_hasSelection)
    {
        RECT inner{};
        if (IntersectRect(&inner, &dirty, &m_sel) != FALSE)
        {
            FillMaskRect(inner, true);
        }
    }

    if (m_bits != nullptr)
    {
        // 2) Rubber band + size label, clipped to the dirty area.
        HRGN clip = CreateRectRgn(dirty.left, dirty.top, dirty.right, dirty.bottom);
        if (clip != nullptr)
        {
            SelectClipRgn(m_memDC, clip);
        }
        DrawBand();
        if (m_dragging)
        {
            RECT labelRect = DrawLabel(m_current);
            m_lastLabelRect = labelRect;
            m_hasLabelRect = !RectIsEmpty(labelRect);
        }
        else
        {
            m_hasLabelRect = false;
            DrawCrosshair();
        }
        if (clip != nullptr)
        {
            SelectClipRgn(m_memDC, nullptr);
            DeleteObject(clip);
        }

        // 3) The critical alpha fix-up.
        FixupAlpha(dirty);
    }

    // 4) Push to the layered window.
    Present(full ? nullptr : &dirty);
}

void CRegionSelector::ComposeFull()
{
    m_hasSelection = false;
    m_dragging = false;
    m_hasLabelRect = false;
    RECT all{};
    all.left = 0;
    all.top = 0;
    all.right = m_width;
    all.bottom = m_height;
    Repaint(true, all);
}

void CRegionSelector::UpdateSelection(POINT current)
{
    m_current = current;

    RECT previous = m_sel;
    const bool hadPrevious = m_hasSelection;

    RECT selection{};
    selection.left = (m_origin.x < current.x) ? m_origin.x : current.x;
    selection.top = (m_origin.y < current.y) ? m_origin.y : current.y;
    selection.right = (m_origin.x > current.x) ? m_origin.x : current.x;
    selection.bottom = (m_origin.y > current.y) ? m_origin.y : current.y;
    ClampRectTo(selection, m_width, m_height);

    m_sel = selection;
    m_hasSelection = !RectIsEmpty(selection);
    m_dragging = true;

    RECT dirty{};
    RECT expanded = ExpandRect(selection, 240);
    bool haveDirty = true;
    dirty = expanded;
    if (hadPrevious)
    {
        RECT previousExpanded = ExpandRect(previous, 240);
        RECT merged{};
        if (UnionRect(&merged, &dirty, &previousExpanded) != FALSE)
        {
            dirty = merged;
        }
    }
    if (m_hasLabelRect)
    {
        RECT merged{};
        if (UnionRect(&merged, &dirty, &m_lastLabelRect) != FALSE)
        {
            dirty = merged;
        }
    }
    if (!haveDirty)
    {
        return;
    }
    Repaint(false, dirty);
}

void CRegionSelector::UpdateCrosshair(POINT current)
{
    // 计算十字线新旧位置的重叠脏区（整宽/整高条带 + 中心圆区域）。
    RECT dirty{};
    bool haveDirty = false;

    const LONG gap = 12;
    POINT spots[2] = {m_crosshair, current};
    bool valid[2] = {m_crosshairValid, true};
    for (int i = 0; i < 2; ++i)
    {
        if (!valid[i] || spots[i].x < 0)
        {
            continue;
        }
        RECT band{};
        band.left = 0;
        band.top = spots[i].y - 2;
        band.right = m_width;
        band.bottom = spots[i].y + 3;
        RECT vert{};
        vert.left = spots[i].x - 2;
        vert.top = 0;
        vert.right = spots[i].x + 3;
        vert.bottom = m_height;
        RECT circle{};
        circle.left = spots[i].x - gap - 2;
        circle.top = spots[i].y - gap - 2;
        circle.right = spots[i].x + gap + 2;
        circle.bottom = spots[i].y + gap + 2;

        RECT merged{};
        UnionRect(&merged, &band, &vert);
        UnionRect(&merged, &merged, &circle);
        if (!haveDirty)
        {
            dirty = merged;
            haveDirty = true;
        }
        else
        {
            UnionRect(&merged, &dirty, &merged);
            dirty = merged;
        }
    }
    if (!haveDirty)
    {
        return;
    }
    m_crosshair = current;
    m_crosshairValid = true;
    ClampRectTo(dirty, m_width, m_height);
    Repaint(false, dirty);
}

void CRegionSelector::RequestFinish(bool confirmed)
{
    if (m_finished)
    {
        return;
    }
    if (confirmed)
    {
        const int width = static_cast<int>(m_sel.right - m_sel.left);
        const int height = static_cast<int>(m_sel.bottom - m_sel.top);
        if (width < snap::kMinSelectionSide || height < snap::kMinSelectionSide)
        {
            // Treat micro selections as accidental clicks.
            confirmed = false;
        }
    }
    m_confirmed = confirmed;
    m_finished = true;
    m_dragging = false;
}

SelectionResult CRegionSelector::Select(HANDLE hCancelEvent)
{
    SelectionResult result;

    if (!EnsureClass())
    {
        return result;
    }
    if (!CreateOverlay())
    {
        DestroyOverlay();
        return result;
    }

    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);
    SetCapture(m_hwnd);

    bool quitPosted = false;
    while (!m_finished)
    {
        DWORD waitResult;
        if (hCancelEvent != nullptr)
        {
            waitResult = MsgWaitForMultipleObjects(1, &hCancelEvent, FALSE, 100, QS_ALLINPUT);
            if (waitResult == WAIT_OBJECT_0)
            {
                m_confirmed = false;
                m_finished = true;
                break;
            }
        }
        else
        {
            waitResult = MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }
        if (waitResult == WAIT_FAILED)
        {
            // 取消事件句柄失效（宿主正在卸载）：当作取消，绝不在这里空转，
            // 否则作业线程会一直跑在即将被 unmap 的代码里。
            m_confirmed = false;
            m_finished = true;
            break;
        }
        if (waitResult == WAIT_TIMEOUT)
        {
            continue;
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                m_confirmed = false;
                m_finished = true;
                quitPosted = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (m_finished)
            {
                break;
            }
        }
    }

    if (GetCapture() == m_hwnd)
    {
        ReleaseCapture();
    }

    const RECT selection = m_sel;
    const bool confirmed = m_confirmed;
    DestroyOverlay();

    // Make sure the overlay is really gone before the caller captures pixels,
    // otherwise the 40% grey mask would end up inside the screenshot.
    DwmFlush();
    Sleep(confirmed ? 80 : 20);

    if (quitPosted)
    {
        PostThreadMessageW(GetCurrentThreadId(), WM_QUIT, 0, 0);
    }

    if (confirmed)
    {
        result.confirmed = true;
        result.rect.left = m_virtualScreen.left + selection.left;
        result.rect.top = m_virtualScreen.top + selection.top;
        result.rect.right = m_virtualScreen.left + selection.right;
        result.rect.bottom = m_virtualScreen.top + selection.bottom;
        SNAP_LOG(L"region selected: %d,%d %dx%d", static_cast<int>(result.rect.left),
                 static_cast<int>(result.rect.top), static_cast<int>(result.rect.right - result.rect.left),
                 static_cast<int>(result.rect.bottom - result.rect.top));
    }
    else
    {
        result.confirmed = false;
        result.rect = RECT{};
    }
    return result;
}

LRESULT CALLBACK CRegionSelector::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }

    CRegionSelector* self = reinterpret_cast<CRegionSelector*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr)
    {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        if (self->m_layered)
        {
            ValidateRect(hwnd, nullptr);
            return 0;
        }
        {
            // Fallback mode: no per-pixel mask available, plain alpha window.
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            if (self->m_dragging && !RectIsEmpty(self->m_sel))
            {
                HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                HGDIOBJ oldPen = SelectObject(dc, pen);
                HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, self->m_sel.left, self->m_sel.top, self->m_sel.right, self->m_sel.bottom);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(pen);
            }
            EndPaint(hwnd, &ps);
        }
        return 0;

    case WM_SETCURSOR:
        // v1.2.0：隐藏系统十字光标，改用覆盖层自绘的现代十字线。
        SetCursor(nullptr);
        return TRUE;

    case WM_LBUTTONDOWN:
    {
        // 拖拽开始：先擦掉十字线（其所在条带为脏区），再进入拖拽状态。
        if (self->m_crosshairValid)
        {
            RECT dirty{};
            dirty.left = 0;
            dirty.top = self->m_crosshair.y - 2;
            dirty.right = self->m_width;
            dirty.bottom = self->m_crosshair.y + 3;
            RECT vert{};
            vert.left = self->m_crosshair.x - 2;
            vert.top = 0;
            vert.right = self->m_crosshair.x + 3;
            vert.bottom = self->m_height;
            RECT merged{};
            if (UnionRect(&merged, &dirty, &vert) != FALSE)
            {
                dirty = merged;
            }
            self->m_crosshairValid = false;
            self->Repaint(false, dirty);
        }
        POINT pt{};
        pt.x = static_cast<LONG>(static_cast<short>(LOWORD(lp)));
        pt.y = static_cast<LONG>(static_cast<short>(HIWORD(lp)));
        self->m_origin = pt;
        self->m_current = pt;
        self->m_sel = RECT{};
        self->m_hasSelection = false;
        self->m_dragging = true;
        SetCapture(hwnd);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        POINT pt{};
        pt.x = static_cast<LONG>(static_cast<short>(LOWORD(lp)));
        pt.y = static_cast<LONG>(static_cast<short>(HIWORD(lp)));
        if (!self->m_dragging)
        {
            self->UpdateCrosshair(pt);
            return 0;
        }
        self->UpdateSelection(pt);
        return 0;
    }

    case WM_LBUTTONUP:
        if (self->m_dragging)
        {
            POINT pt{};
            pt.x = static_cast<LONG>(static_cast<short>(LOWORD(lp)));
            pt.y = static_cast<LONG>(static_cast<short>(HIWORD(lp)));
            self->UpdateSelection(pt);
            ReleaseCapture();
            self->RequestFinish(true);
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        self->RequestFinish(true);
        return 0;

    case WM_RBUTTONUP:
        ReleaseCapture();
        self->RequestFinish(false);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
        {
            self->RequestFinish(false);
            return 0;
        }
        if (wp == VK_RETURN)
        {
            self->RequestFinish(true);
            return 0;
        }
        return 0;

    case WM_DESTROY:
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
