// WindowPicker.cpp - list all capturable top-level windows and let the user pick one.
#include "WindowPicker.h"
#include "PluginSettings.h"

#include <dwmapi.h>
#include <string>

namespace
{
constexpr int IDC_PICK_LIST = 2001;

void PushDword(std::vector<BYTE>& out, DWORD v)
{
    const BYTE b[4] = {static_cast<BYTE>(v), static_cast<BYTE>(v >> 8), static_cast<BYTE>(v >> 16),
                       static_cast<BYTE>(v >> 24)};
    out.insert(out.end(), b, b + 4);
}

void PushWord(std::vector<BYTE>& out, WORD v)
{
    out.push_back(static_cast<BYTE>(v));
    out.push_back(static_cast<BYTE>(v >> 8));
}

void PushShort(std::vector<BYTE>& out, SHORT v)
{
    PushWord(out, static_cast<WORD>(v));
}

void AlignDword(std::vector<BYTE>& out)
{
    while (out.size() % 4 != 0)
    {
        out.push_back(0);
    }
}

void PushString(std::vector<BYTE>& out, const wchar_t* text)
{
    const wchar_t* p = text != nullptr ? text : L"";
    const size_t chars = wcslen(p) + 1;
    for (size_t i = 0; i < chars; ++i)
    {
        PushWord(out, static_cast<WORD>(p[i]));
    }
}
} // namespace

BOOL CALLBACK CWindowPicker::CollectProc(HWND hwnd, LPARAM lp)
{
    auto* entries = reinterpret_cast<std::vector<WindowPickEntry>*>(lp);
    if (hwnd == nullptr || IsWindowVisible(hwnd) == FALSE || IsIconic(hwnd) != FALSE)
    {
        return TRUE;
    }

    // Skip our own windows (sink / overlay / picker / worker).
    wchar_t className[128] = {0};
    if (GetClassNameW(hwnd, className, static_cast<int>(_countof(className))) <= 0)
    {
        return TRUE;
    }
    if (wcscmp(className, TMSNAP_OVERLAY_CLASS) == 0 || wcscmp(className, TMSNAP_SINK_CLASS) == 0 ||
        wcscmp(className, TMSNAP_HOTKEY_CLASS) == 0 || wcscmp(className, TMSNAP_WORKER_CLASS) == 0 ||
        wcscmp(className, TMSNAP_PICKER_CLASS) == 0)
    {
        return TRUE;
    }

    // Skip tool windows (tooltips, floating palettes) and cloaked UWP ghosts.
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0)
    {
        return TRUE;
    }
    BOOL cloaked = FALSE;
    if (DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)) == S_OK && cloaked != FALSE)
    {
        return TRUE;
    }

    int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen <= 0)
    {
        return TRUE;
    }
    WindowPickEntry entry;
    entry.hwnd = hwnd;
    entry.className = className;
    entry.title.resize(static_cast<size_t>(titleLen) + 1);
    GetWindowTextW(hwnd, &entry.title[0], titleLen + 1);
    entry.title.resize(entry.title.find(L'\0') == std::wstring::npos ? entry.title.size()
                                                                     : entry.title.find(L'\0'));
    if (entry.title.empty())
    {
        return TRUE;
    }

    HICON icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0));
    if (icon == nullptr)
    {
        icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0));
    }
    if (icon == nullptr)
    {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
    }
    if (icon == nullptr)
    {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
    }
    entry.icon = icon;

    entries->push_back(std::move(entry));
    return TRUE;
}

void CWindowPicker::FillList(HWND hDlg) const
{
    HWND list = GetDlgItem(hDlg, IDC_PICK_LIST);
    if (list == nullptr)
    {
        return;
    }
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        const std::wstring line = m_entries[i].title + L"    [" + m_entries[i].className + L"]";
        const int index = static_cast<int>(SendMessageW(list, LB_ADDSTRING, 0,
                                                        reinterpret_cast<LPARAM>(line.c_str())));
        SendMessageW(list, LB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
    }
    if (!m_entries.empty())
    {
        SendMessageW(list, LB_SETCURSEL, 0, 0);
    }
}

INT_PTR CALLBACK CWindowPicker::DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CWindowPicker* self = nullptr;
    if (msg == WM_INITDIALOG)
    {
        self = reinterpret_cast<CWindowPicker*>(lp);
        SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        if (self == nullptr)
        {
            return FALSE;
        }

        const int dpi = static_cast<int>(snap::GetDpiForHwndLazy(hDlg));

        // Center on the primary work area at exact pixels.
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int width = MulDiv(430, dpi, 96);
        const int height = MulDiv(470, dpi, 96);
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + ((work.bottom - work.top) - height) / 2;
        SetWindowPos(hDlg, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);

        HWND list = GetDlgItem(hDlg, IDC_PICK_LIST);
        SetWindowPos(list, nullptr, MulDiv(10, dpi, 96), MulDiv(10, dpi, 96), MulDiv(410, dpi, 96),
                     MulDiv(380, dpi, 96), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hDlg, IDOK), nullptr, MulDiv(220, dpi, 96), MulDiv(400, dpi, 96),
                     MulDiv(96, dpi, 96), MulDiv(28, dpi, 96), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hDlg, IDCANCEL), nullptr, MulDiv(324, dpi, 96), MulDiv(400, dpi, 96),
                     MulDiv(96, dpi, 96), MulDiv(28, dpi, 96), SWP_NOZORDER | SWP_NOACTIVATE);

        HFONT font = reinterpret_cast<HFONT>(SendMessageW(hDlg, WM_GETFONT, 0, 0));
        EnumChildWindows(hDlg, [](HWND child, LPARAM lp) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(font));

        // Scale list rows to the DPI.
        self->FillList(hDlg);

        self->m_oldListProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            list, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CWindowPicker::ListProc)));
        return TRUE;
    }

    self = reinterpret_cast<CWindowPicker*>(GetWindowLongPtrW(hDlg, DWLP_USER));
    if (self == nullptr)
    {
        return FALSE;
    }

    switch (msg)
    {
    case WM_MEASUREITEM:
    {
        MEASUREITEMSTRUCT* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (mi != nullptr && mi->CtlType == ODT_LISTBOX)
        {
            const int dpi = static_cast<int>(snap::GetDpiForHwndLazy(hDlg));
            mi->itemHeight = static_cast<UINT>(MulDiv(24, dpi, 96));
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (di == nullptr || di->CtlType != ODT_LISTBOX || di->itemID == static_cast<UINT>(-1))
        {
            break;
        }
        const size_t dataIndex = static_cast<size_t>(SendMessageW(di->hwndItem, LB_GETITEMDATA,
                                                                  static_cast<WPARAM>(di->itemID), 0));
        if (dataIndex >= self->m_entries.size())
        {
            break;
        }
        const WindowPickEntry& entry = self->m_entries[dataIndex];
        const bool selected = (di->itemState & ODS_SELECTED) != 0;

        if (selected)
        {
            RECT bg = di->rcItem;
            HBRUSH brush = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT));
            FillRect(di->hDC, &bg, brush);
            DeleteObject(brush);
            SetTextColor(di->hDC, GetSysColor(COLOR_HIGHLIGHTTEXT));
        }
        else
        {
            SetTextColor(di->hDC, GetSysColor(COLOR_WINDOWTEXT));
        }
        SetBkMode(di->hDC, TRANSPARENT);

        const int dpi = static_cast<int>(snap::GetDpiForHwndLazy(di->hwndItem));
        const int iconSize = MulDiv(16, dpi, 96);
        if (entry.icon != nullptr)
        {
            DrawIconEx(di->hDC, di->rcItem.left + MulDiv(4, dpi, 96),
                       di->rcItem.top + (di->rcItem.bottom - di->rcItem.top - iconSize) / 2, entry.icon,
                       iconSize, iconSize, 0, nullptr, DI_NORMAL);
        }
        RECT textRect = di->rcItem;
        textRect.left += MulDiv(26, dpi, 96);
        DrawTextW(di->hDC, entry.title.c_str(), -1, &textRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_PICK_LIST:
            if (HIWORD(wp) == LBN_DBLCLK)
            {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
            }
            return TRUE;
        case IDOK:
        {
            HWND list = GetDlgItem(hDlg, IDC_PICK_LIST);
            const int index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
            if (index < 0 || static_cast<size_t>(index) >= self->m_entries.size())
            {
                return TRUE;
            }
            const size_t dataIndex = static_cast<size_t>(
                SendMessageW(list, LB_GETITEMDATA, static_cast<WPARAM>(index), 0));
            if (dataIndex < self->m_entries.size())
            {
                HWND picked = self->m_entries[dataIndex].hwnd;
                EndDialog(hDlg, reinterpret_cast<INT_PTR>(picked));
            }
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

LRESULT CALLBACK CWindowPicker::ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND hDlg = GetParent(hwnd);
    CWindowPicker* self =
        (hDlg != nullptr) ? reinterpret_cast<CWindowPicker*>(GetWindowLongPtrW(hDlg, DWLP_USER)) : nullptr;
    if (self == nullptr)
    {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_KEYDOWN)
    {
        if (wp == VK_RETURN)
        {
            SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
            return 0;
        }
        if (wp == VK_ESCAPE)
        {
            SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
            return 0;
        }
    }
    return CallWindowProcW(self->m_oldListProc, hwnd, msg, wp, lp);
}

bool CWindowPicker::Pick(CUxText* text, HWND& out)
{
    out = nullptr;
    m_entries.clear();

    const wchar_t* titleText = text != nullptr ? text->Str(StringId::kPickTitle) : L"选择要截取的窗口";
    const wchar_t* okText = text != nullptr ? text->Str(StringId::kBtnOk) : L"确定";
    const wchar_t* cancelText = text != nullptr ? text->Str(StringId::kBtnCancel) : L"取消";

    // Current foreground window first, then z-order (EnumWindows enumerates top-down).
    HWND foreground = GetForegroundWindow();
    EnumWindows(CollectProc, reinterpret_cast<LPARAM>(&m_entries));
    for (size_t i = 1; i < m_entries.size(); ++i)
    {
        if (m_entries[i].hwnd == foreground)
        {
            WindowPickEntry entry = m_entries[i];
            m_entries.erase(m_entries.begin() + static_cast<long>(i));
            m_entries.insert(m_entries.begin(), std::move(entry));
            break;
        }
    }
    if (m_entries.empty())
    {
        return false;
    }

    // Build the dialog template in memory (same technique as COptionsDialog).
    struct ItemDef
    {
        int id;
        WORD itemClass;
        const wchar_t* text;
        DWORD style;
    };
    const ItemDef items[] = {
        {IDC_PICK_LIST, 0x0083 /*listbox*/, L"",
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
             LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_BORDER},
        {IDOK, 0x0080 /*button*/, okText, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON},
        {IDCANCEL, 0x0080 /*button*/, cancelText, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON},
    };

    std::vector<BYTE> buffer;
    buffer.reserve(2048);
    PushDword(buffer, DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    PushDword(buffer, 0);
    PushWord(buffer, static_cast<WORD>(sizeof(items) / sizeof(items[0])));
    PushShort(buffer, 0);
    PushShort(buffer, 0);
    PushShort(buffer, 300);
    PushShort(buffer, 320);
    PushWord(buffer, 0);
    PushWord(buffer, 0);
    PushString(buffer, titleText);
    PushWord(buffer, 9);
    PushString(buffer, L"MS Shell Dlg");

    for (const ItemDef& item : items)
    {
        AlignDword(buffer);
        PushDword(buffer, item.style);
        PushDword(buffer, 0);
        PushShort(buffer, 0);
        PushShort(buffer, 0);
        PushShort(buffer, 10);
        PushShort(buffer, 10);
        PushWord(buffer, static_cast<WORD>(item.id));
        PushWord(buffer, 0xFFFF);
        PushWord(buffer, item.itemClass);
        PushString(buffer, item.text);
        PushWord(buffer, 0);
    }

    // Same-thread modal loop: the picker runs on the capture job thread, the result is the
    // dialog return value (chosen HWND or 0).
    const INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
                                                   reinterpret_cast<LPCDLGTEMPLATEW>(buffer.data()),
                                                   nullptr, &CWindowPicker::DlgProc, reinterpret_cast<LPARAM>(this));
    if (result <= 0 || !IsWindow(reinterpret_cast<HWND>(result)))
    {
        return false;
    }
    out = reinterpret_cast<HWND>(result);
    return true;
}
