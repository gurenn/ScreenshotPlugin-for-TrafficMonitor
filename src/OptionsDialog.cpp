// OptionsDialog.cpp - in-memory DLGTEMPLATE, pixel-exact layout, hotkey recorder.
#include "OptionsDialog.h"

#include "CaptureEngine.h"
#include "ImageWriter.h"

#include <new>

namespace
{

const int kClientWidth96 = 460;
const int kClientHeight96 = 642;

// --- template building helpers ---------------------------------------------
void AlignDword(std::vector<BYTE>& out)
{
    while ((out.size() % 4) != 0)
    {
        out.push_back(0);
    }
}

void PushWord(std::vector<BYTE>& out, WORD value)
{
    out.push_back(static_cast<BYTE>(value & 0xFF));
    out.push_back(static_cast<BYTE>((value >> 8) & 0xFF));
}

void PushDword(std::vector<BYTE>& out, DWORD value)
{
    out.push_back(static_cast<BYTE>(value & 0xFF));
    out.push_back(static_cast<BYTE>((value >> 8) & 0xFF));
    out.push_back(static_cast<BYTE>((value >> 16) & 0xFF));
    out.push_back(static_cast<BYTE>((value >> 24) & 0xFF));
}

void PushShort(std::vector<BYTE>& out, int value)
{
    PushWord(out, static_cast<WORD>(static_cast<short>(value)));
}

void PushString(std::vector<BYTE>& out, const wchar_t* text)
{
    if (text != nullptr)
    {
        for (const wchar_t* p = text; *p != L'\0'; ++p)
        {
            PushWord(out, static_cast<WORD>(*p));
        }
    }
    PushWord(out, 0);
}

enum class ItemClass
{
    Button = 0x0080,
    Edit = 0x0081,
    Static = 0x0082,
    ComboBox = 0x0085
};

struct ItemDef
{
    int id;
    ItemClass itemClass;
    const wchar_t* text;
    DWORD style;
};

// --- small dialog helpers --------------------------------------------------
std::wstring GetDlgText(HWND hDlg, int id)
{
    wchar_t buffer[1024] = {0};
    GetDlgItemTextW(hDlg, id, buffer, static_cast<int>(_countof(buffer)));
    return std::wstring(buffer);
}

int GetDlgInt(HWND hDlg, int id, int fallback, int minValue, int maxValue)
{
    wchar_t buffer[64] = {0};
    GetDlgItemTextW(hDlg, id, buffer, static_cast<int>(_countof(buffer)));
    wchar_t* end = nullptr;
    long value = wcstol(buffer, &end, 10);
    if (end == buffer)
    {
        return fallback;
    }
    if (value < minValue)
    {
        return minValue;
    }
    if (value > maxValue)
    {
        return maxValue;
    }
    return static_cast<int>(value);
}

bool IsChecked(HWND hDlg, int id)
{
    return IsDlgButtonChecked(hDlg, id) == BST_CHECKED;
}

bool IsModifierKey(UINT vk)
{
    switch (vk)
    {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

BOOL CALLBACK SetChildFontProc(HWND child, LPARAM param)
{
    HFONT font = reinterpret_cast<HFONT>(param);
    if (font != nullptr)
    {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return TRUE;
}

} // namespace

COptionsDialog::COptionsDialog(CPluginSettings* settings, CUxText* text, CHotkeyManager* hotkeys)
    : m_pSettings(settings), m_pText(text), m_pHotkeys(hotkeys)
{
    if (m_pSettings != nullptr)
    {
        m_work = m_pSettings->data;
        m_hotkey = m_pSettings->data.hotkey1.binding;
        m_hotkey2 = m_pSettings->data.hotkey2.binding;
    }
    else
    {
        m_hotkey = HotkeyBinding::Default();
        m_hotkey2 = HotkeyBinding{MOD_CONTROL | MOD_ALT, 'R'};
    }
}

COptionsDialog::~COptionsDialog()
{
}

ITMPlugin::OptionReturn COptionsDialog::Show(HWND hParent)
{
    if (m_pSettings == nullptr || m_pText == nullptr)
    {
        return ITMPlugin::OR_OPTION_NOT_PROVIDED;
    }

    m_work = m_pSettings->data;
    m_hotkey = m_pSettings->data.hotkey1.binding;
    m_hotkey2 = m_pSettings->data.hotkey2.binding;
    m_changed = false;
    m_recordingSlot = 0;
    m_statusError = false;
    m_testRunning = false;
    m_hTestThread = nullptr;

    std::vector<BYTE> buffer;
    BuildTemplate(buffer);

    INT_PTR result = DialogBoxIndirectParamW(g_hDllInstance,
                                             reinterpret_cast<LPCDLGTEMPLATEW>(buffer.data()), hParent,
                                             &COptionsDialog::DlgProc, reinterpret_cast<LPARAM>(this));
    if (result == IDOK && m_changed)
    {
        return ITMPlugin::OR_OPTION_CHANGED;
    }
    return ITMPlugin::OR_OPTION_UNCHANGED;
}

void COptionsDialog::BuildTemplate(std::vector<BYTE>& out) const
{
    const std::wstring title = m_pText ? m_pText->Str(StringId::kDlgTitle) : L"TMSnap";

    const ItemDef items[] = {
        {IDC_GRP_SAVE, ItemClass::Button, m_pText->Str(StringId::kGrpSave),
         WS_CHILD | WS_VISIBLE | BS_GROUPBOX},
        {IDC_LBL_SAVE_DIR, ItemClass::Static, m_pText->Str(StringId::kLblSaveDir),
         WS_CHILD | WS_VISIBLE | SS_LEFT},
        {IDC_SAVE_DIR, ItemClass::Edit, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL},
        {IDC_BTN_BROWSE, ItemClass::Button, m_pText->Str(StringId::kBtnBrowse),
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON},
        {IDC_BTN_OPEN_DIR, ItemClass::Button, m_pText->Str(StringId::kBtnOpenDir),
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON},
        {IDC_LBL_FORMAT, ItemClass::Static, m_pText->Str(StringId::kLblFormat),
         WS_CHILD | WS_VISIBLE | SS_LEFT},
        {IDC_FORMAT, ItemClass::ComboBox, L"",
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST},
        {IDC_LBL_QUALITY, ItemClass::Static, m_pText->Str(StringId::kLblQuality),
         WS_CHILD | WS_VISIBLE | SS_LEFT},
        {IDC_QUALITY, ItemClass::Edit, L"",
         WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL},
        {IDC_QUALITY_MINUS, ItemClass::Button, L"\u2212", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON},
        {IDC_QUALITY_PLUS, ItemClass::Button, L"+", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON},
        {IDC_LBL_TEMPLATE, ItemClass::Static, m_pText->Str(StringId::kLblTemplate),
         WS_CHILD | WS_VISIBLE | SS_LEFT},
        {IDC_TEMPLATE, ItemClass::Edit, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL},
        {IDC_EXAMPLE, ItemClass::Static, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS},
        {IDC_CLIPBOARD, ItemClass::Button, m_pText->Str(StringId::kChkClipboard),
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX},

        {IDC_GRP_CAPTURE, ItemClass::Button, m_pText->Str(StringId::kGrpCapture),
         WS_CHILD | WS_VISIBLE | BS_GROUPBOX},
        {IDC_LBL_DELAY, ItemClass::Static, m_pText->Str(StringId::kLblDelay),
         WS_CHILD | WS_VISIBLE | SS_LEFT},
        {IDC_DELAY, ItemClass::Edit, L"",
         WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL},
        {IDC_DELAY_MINUS, ItemClass::Button, L"\u2212", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON},
        {IDC_DELAY_PLUS, ItemClass::Button, L"+", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON},
        {IDC_CURSOR, ItemClass::Button, m_pText->Str(StringId::kChkCursor),
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX},
        {IDC_HOTKEY_ENABLED, ItemClass::Button, m_pText->Str(StringId::kChkHotkeyEnabled),
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX},
        {IDC_LBL_ACTION1, ItemClass::Static, m_pText->Str(StringId::kLblAction),
         WS_CHILD | WS_VISIBLE | SS_LEFT},
        {IDC_ACTION1, ItemClass::ComboBox, L"",
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST},
        {IDC_HOTKEY, ItemClass::Button, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON},
        {IDC_HOTKEY2_ENABLED, ItemClass::Button, m_pText->Str(StringId::kChkHotkey2Enabled),
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX},
        {IDC_LBL_ACTION2, ItemClass::Static, m_pText->Str(StringId::kLblAction),
         WS_CHILD | WS_VISIBLE | SS_LEFT},
        {IDC_ACTION2, ItemClass::ComboBox, L"",
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST},
        {IDC_HOTKEY2, ItemClass::Button, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON},
        {IDC_BTN_TEST, ItemClass::Button, m_pText->Str(StringId::kBtnTest),
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON},
        {IDC_STATUS, ItemClass::Static, m_pText->Str(StringId::kMsgStatusReady),
         WS_CHILD | WS_VISIBLE | SS_LEFT},

        {IDC_GRP_ABOUT, ItemClass::Button, m_pText->Str(StringId::kGrpAbout),
         WS_CHILD | WS_VISIBLE | BS_GROUPBOX},
        {IDC_ABOUT, ItemClass::Static, m_pText->Str(StringId::kAboutText),
         WS_CHILD | WS_VISIBLE | SS_LEFT},

        {IDC_BTN_RESET, ItemClass::Button, m_pText->Str(StringId::kBtnReset),
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON},
        {IDOK, ItemClass::Button, m_pText->Str(StringId::kBtnOk),
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON},
        {IDCANCEL, ItemClass::Button, m_pText->Str(StringId::kBtnCancel),
         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON},
    };
    const WORD itemCount = static_cast<WORD>(sizeof(items) / sizeof(items[0]));

    out.clear();
    out.reserve(4096);

    // ---- DLGTEMPLATE ------------------------------------------------------
    PushDword(out, DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    PushDword(out, 0);          // dwExtendedStyle
    PushWord(out, itemCount);   // cdit
    PushShort(out, 0);          // x
    PushShort(out, 0);          // y
    PushShort(out, 300);        // cx (dialog units; exact pixels are set later)
    PushShort(out, 340);        // cy
    PushWord(out, 0);           // no menu
    PushWord(out, 0);           // default window class
    PushString(out, title.c_str());
    PushWord(out, 9);           // point size
    PushString(out, L"MS Shell Dlg");

    // ---- items ------------------------------------------------------------
    for (WORD i = 0; i < itemCount; ++i)
    {
        const ItemDef& item = items[i];
        AlignDword(out);
        PushDword(out, item.style);
        PushDword(out, 0); // dwExtendedStyle
        PushShort(out, 0);
        PushShort(out, 0);
        PushShort(out, 10);
        PushShort(out, 10);
        PushWord(out, static_cast<WORD>(item.id));
        PushWord(out, 0xFFFF);
        PushWord(out, static_cast<WORD>(item.itemClass));
        PushString(out, item.text);
        PushWord(out, 0); // creation data size
    }
}

void COptionsDialog::LayoutControls(HWND hDlg, int dpi)
{
    struct LayoutItem
    {
        int id;
        int x;
        int y;
        int w;
        int h;
    };

    const LayoutItem layout[] = {
        {IDC_GRP_SAVE, 10, 6, 440, 236},
        {IDC_LBL_SAVE_DIR, 20, 30, 92, 18},
        {IDC_SAVE_DIR, 116, 26, 266, 22},
        {IDC_BTN_BROWSE, 388, 26, 52, 24},
        {IDC_BTN_OPEN_DIR, 116, 56, 110, 24},
        {IDC_LBL_FORMAT, 20, 92, 92, 18},
        {IDC_FORMAT, 116, 88, 140, 200},
        {IDC_LBL_QUALITY, 20, 124, 92, 18},
        {IDC_QUALITY, 116, 120, 56, 22},
        {IDC_QUALITY_MINUS, 176, 120, 24, 24},
        {IDC_QUALITY_PLUS, 204, 120, 24, 24},
        {IDC_LBL_TEMPLATE, 20, 156, 92, 18},
        {IDC_TEMPLATE, 116, 152, 324, 22},
        {IDC_EXAMPLE, 116, 178, 324, 16},
        {IDC_CLIPBOARD, 116, 200, 324, 20},

        {IDC_GRP_CAPTURE, 10, 250, 440, 262},
        {IDC_LBL_DELAY, 20, 274, 92, 18},
        {IDC_DELAY, 116, 270, 56, 22},
        {IDC_DELAY_MINUS, 176, 270, 24, 24},
        {IDC_DELAY_PLUS, 204, 270, 24, 24},
        {IDC_CURSOR, 116, 302, 324, 20},
        {IDC_HOTKEY_ENABLED, 116, 326, 324, 20},
        {IDC_LBL_ACTION1, 20, 354, 92, 18},
        {IDC_ACTION1, 116, 350, 110, 200},
        {IDC_HOTKEY, 232, 350, 208, 26},
        {IDC_HOTKEY2_ENABLED, 116, 382, 324, 20},
        {IDC_LBL_ACTION2, 20, 410, 92, 18},
        {IDC_ACTION2, 116, 406, 110, 200},
        {IDC_HOTKEY2, 232, 406, 208, 26},
        {IDC_BTN_TEST, 116, 440, 110, 26},
        {IDC_STATUS, 116, 470, 324, 36},

        {IDC_GRP_ABOUT, 10, 520, 440, 70},
        {IDC_ABOUT, 20, 536, 420, 50},

        {IDC_BTN_RESET, 140, 598, 84, 26},
        {IDOK, 232, 598, 84, 26},
        {IDCANCEL, 324, 598, 84, 26},
    };

    for (size_t i = 0; i < sizeof(layout) / sizeof(layout[0]); ++i)
    {
        HWND control = GetDlgItem(hDlg, layout[i].id);
        if (control == nullptr)
        {
            continue;
        }
        SetWindowPos(control, nullptr, MulDiv(layout[i].x, dpi, 96), MulDiv(layout[i].y, dpi, 96),
                     MulDiv(layout[i].w, dpi, 96), MulDiv(layout[i].h, dpi, 96),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    RECT client{};
    client.left = 0;
    client.top = 0;
    client.right = MulDiv(kClientWidth96, dpi, 96);
    client.bottom = MulDiv(kClientHeight96, dpi, 96);

    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hDlg, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hDlg, GWL_EXSTYLE));
    RECT window = client;
    AdjustWindowRectEx(&window, style, FALSE, exStyle);

    const int width = window.right - window.left;
    const int height = window.bottom - window.top;

    HWND parent = GetParent(hDlg);
    RECT parentRect{};
    if (parent == nullptr || GetWindowRect(parent, &parentRect) == FALSE)
    {
        GetWindowRect(GetDesktopWindow(), &parentRect);
    }
    int x = parentRect.left + ((parentRect.right - parentRect.left) - width) / 2;
    int y = parentRect.top + ((parentRect.bottom - parentRect.top) - height) / 2;
    if (x < 0)
    {
        x = 0;
    }
    if (y < 0)
    {
        y = 0;
    }
    SetWindowPos(hDlg, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void COptionsDialog::LoadToControls(HWND hDlg)
{
    SetWindowTextW(hDlg, m_pText->Str(StringId::kDlgTitle));
    SetDlgItemTextW(hDlg, IDC_SAVE_DIR, m_work.saveDir.c_str());
    SetDlgItemTextW(hDlg, IDC_TEMPLATE, m_work.fileNameTemplate.c_str());

    HWND combo = GetDlgItem(hDlg, IDC_FORMAT);
    if (combo != nullptr)
    {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PNG"));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"JPG"));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"BMP"));
        int selection = 0;
        if (m_work.format == ImageFormat::Jpg)
        {
            selection = 1;
        }
        else if (m_work.format == ImageFormat::Bmp)
        {
            selection = 2;
        }
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
    }

    SetDlgItemInt(hDlg, IDC_QUALITY, static_cast<UINT>(m_work.jpgQuality), FALSE);
    SetDlgItemInt(hDlg, IDC_DELAY, static_cast<UINT>(m_work.delaySeconds), FALSE);

    CheckDlgButton(hDlg, IDC_CLIPBOARD, m_work.copyToClipboard ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_CURSOR, m_work.includeCursor ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_HOTKEY_ENABLED, m_work.hotkey1.enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_HOTKEY2_ENABLED, m_work.hotkey2.enabled ? BST_CHECKED : BST_UNCHECKED);

    // 两个"动作"下拉框共用命令名文案：全屏截图 / 区域截图 / 活动窗口截图 / 延时截图。
    const wchar_t* actionNames[] = {
        m_pText->Str(StringId::kCmdFullScreen),  m_pText->Str(StringId::kCmdRegion),
        m_pText->Str(StringId::kCmdActiveWindow), m_pText->Str(StringId::kCmdDelay),
    };
    HWND action1 = GetDlgItem(hDlg, IDC_ACTION1);
    HWND action2 = GetDlgItem(hDlg, IDC_ACTION2);
    for (HWND combo : {action1, action2})
    {
        if (combo != nullptr)
        {
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            for (const wchar_t* name : actionNames)
            {
                SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
            }
        }
    }
    if (action1 != nullptr)
    {
        SendMessageW(action1, CB_SETCURSEL, static_cast<WPARAM>(m_work.hotkey1.action), 0);
    }
    if (action2 != nullptr)
    {
        SendMessageW(action2, CB_SETCURSEL, static_cast<WPARAM>(m_work.hotkey2.action), 0);
    }

    m_hotkey = m_work.hotkey1.binding;
    m_hotkey2 = m_work.hotkey2.binding;
    m_recordingSlot = 0;
    UpdateHotkeyButton(hDlg);
    UpdateDerivedState(hDlg);
    SetStatus(hDlg, m_pText->Str(StringId::kMsgStatusReady), false);
}

void COptionsDialog::UpdateHotkeyButton(HWND hDlg)
{
    HWND button1 = GetDlgItem(hDlg, IDC_HOTKEY);
    if (button1 != nullptr)
    {
        m_hHotkeyButton = button1;
        if (m_recordingSlot == 1)
        {
            SetWindowTextW(button1, m_pText->Str(StringId::kHotkeyPress));
        }
        else
        {
            std::wstring text = m_hotkey.ToText();
            SetWindowTextW(button1, text.c_str());
        }
    }
    HWND button2 = GetDlgItem(hDlg, IDC_HOTKEY2);
    if (button2 != nullptr)
    {
        m_hHotkeyButton2 = button2;
        if (m_recordingSlot == 2)
        {
            SetWindowTextW(button2, m_pText->Str(StringId::kHotkeyPress));
        }
        else
        {
            std::wstring text = m_hotkey2.ToText();
            SetWindowTextW(button2, text.c_str());
        }
    }
}

void COptionsDialog::UpdateDerivedState(HWND hDlg)
{
    HWND combo = GetDlgItem(hDlg, IDC_FORMAT);
    int selection = 0;
    if (combo != nullptr)
    {
        selection = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    }
    const bool jpgSelected = (selection == 1);

    EnableWindow(GetDlgItem(hDlg, IDC_QUALITY), jpgSelected ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_QUALITY_MINUS), jpgSelected ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_QUALITY_PLUS), jpgSelected ? TRUE : FALSE);

    const bool hotkey1Enabled = IsChecked(hDlg, IDC_HOTKEY_ENABLED);
    EnableWindow(GetDlgItem(hDlg, IDC_ACTION1), hotkey1Enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_HOTKEY), hotkey1Enabled ? TRUE : FALSE);

    const bool hotkey2Enabled = IsChecked(hDlg, IDC_HOTKEY2_ENABLED);
    EnableWindow(GetDlgItem(hDlg, IDC_ACTION2), hotkey2Enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_HOTKEY2), hotkey2Enabled ? TRUE : FALSE);

    UpdateExample(hDlg);
}

void COptionsDialog::UpdateExample(HWND hDlg)
{
    std::wstring tmpl = snap::Trim(GetDlgText(hDlg, IDC_TEMPLATE));
    if (tmpl.empty())
    {
        tmpl = CPluginSettings::DefaultTemplate();
    }

    SYSTEMTIME now;
    GetLocalTime(&now);

    HWND combo = GetDlgItem(hDlg, IDC_FORMAT);
    int selection = 0;
    if (combo != nullptr)
    {
        selection = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    }
    ImageFormat format = ImageFormat::Png;
    if (selection == 1)
    {
        format = ImageFormat::Jpg;
    }
    else if (selection == 2)
    {
        format = ImageFormat::Bmp;
    }

    CPluginSettings* settings = m_pSettings;
    std::wstring expanded;
    if (settings != nullptr)
    {
        expanded = settings->ExpandTemplateFor(tmpl, now);
    }
    std::wstring stem = CPluginSettings::SanitizeFileName(expanded);
    if (stem.empty())
    {
        stem = L"TMSnap";
    }
    std::wstring fileName = stem + ImageFormatExtension(format);
    std::wstring text = snap::FormatString(m_pText->Str(StringId::kLblExample), fileName.c_str());
    SetDlgItemTextW(hDlg, IDC_EXAMPLE, text.c_str());
}

void COptionsDialog::SetStatus(HWND hDlg, const std::wstring& text, bool isError)
{
    m_statusError = isError;
    SetDlgItemTextW(hDlg, IDC_STATUS, text.c_str());
    HWND status = GetDlgItem(hDlg, IDC_STATUS);
    if (status != nullptr)
    {
        InvalidateRect(status, nullptr, TRUE);
    }
}

bool COptionsDialog::ReadFromControls(HWND hDlg, PluginSettings& out, std::wstring& err)
{
    out = m_work;
    err.clear();

    out.saveDir = snap::Trim(snap::ExpandEnv(GetDlgText(hDlg, IDC_SAVE_DIR)));
    if (out.saveDir.empty())
    {
        err = m_pText->Str(StringId::kMsgDirMissing);
        return false;
    }

    HWND combo = GetDlgItem(hDlg, IDC_FORMAT);
    int selection = 0;
    if (combo != nullptr)
    {
        selection = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    }
    out.format = ImageFormat::Png;
    if (selection == 1)
    {
        out.format = ImageFormat::Jpg;
    }
    else if (selection == 2)
    {
        out.format = ImageFormat::Bmp;
    }

    out.jpgQuality = GetDlgInt(hDlg, IDC_QUALITY, 90, 1, 100);
    out.delaySeconds = GetDlgInt(hDlg, IDC_DELAY, 0, 0, 30);

    out.fileNameTemplate = snap::Trim(GetDlgText(hDlg, IDC_TEMPLATE));
    if (out.fileNameTemplate.empty())
    {
        out.fileNameTemplate = CPluginSettings::DefaultTemplate();
    }

    out.copyToClipboard = IsChecked(hDlg, IDC_CLIPBOARD);
    out.includeCursor = IsChecked(hDlg, IDC_CURSOR);
    out.hotkey1.enabled = IsChecked(hDlg, IDC_HOTKEY_ENABLED);
    out.hotkey1.binding = m_hotkey;
    out.hotkey1.action = static_cast<int>(SendMessageW(GetDlgItem(hDlg, IDC_ACTION1), CB_GETCURSEL, 0, 0));
    if (out.hotkey1.action < 0 || out.hotkey1.action > 3)
    {
        out.hotkey1.action = 0;
    }
    out.hotkey2.enabled = IsChecked(hDlg, IDC_HOTKEY2_ENABLED);
    out.hotkey2.binding = m_hotkey2;
    out.hotkey2.action = static_cast<int>(SendMessageW(GetDlgItem(hDlg, IDC_ACTION2), CB_GETCURSEL, 0, 0));
    if (out.hotkey2.action < 0 || out.hotkey2.action > 3)
    {
        out.hotkey2.action = 1;
    }

    if (out.hotkey1.enabled && !out.hotkey1.binding.IsValid())
    {
        err = m_pText->Str(StringId::kMsgNeedModifier);
        return false;
    }
    if (out.hotkey2.enabled && !out.hotkey2.binding.IsValid())
    {
        err = m_pText->Str(StringId::kMsgNeedModifier);
        return false;
    }
    if (out.hotkey1.enabled && out.hotkey2.enabled &&
        out.hotkey1.binding.vk == out.hotkey2.binding.vk &&
        out.hotkey1.binding.modifiers == out.hotkey2.binding.modifiers)
    {
        err = m_pText->Str(StringId::kMsgHotkeyDuplicate);
        return false;
    }
    return true;
}

void COptionsDialog::OnTestCapture(HWND hDlg)
{
    if (m_testRunning)
    {
        return;
    }

    PluginSettings temporary;
    std::wstring err;
    if (!ReadFromControls(hDlg, temporary, err))
    {
        SetStatus(hDlg, err, true);
        return;
    }
    if (!snap::EnsureDirectory(temporary.saveDir) || !snap::IsDirectoryWritable(temporary.saveDir))
    {
        SetStatus(hDlg,
                  snap::FormatString(m_pText->Str(StringId::kMsgDirNotWritable), temporary.saveDir.c_str()),
                  true);
        return;
    }

    TestCaptureContext* context = new (std::nothrow) TestCaptureContext();
    if (context == nullptr)
    {
        SetStatus(hDlg, m_pText->Str(StringId::kMsgStatusReady), true);
        return;
    }
    context->settings = temporary;
    context->hDlg = hDlg;

    m_hTestThread = CreateThread(nullptr, 0, &COptionsDialog::TestCaptureProc, context, 0, nullptr);
    if (m_hTestThread == nullptr)
    {
        delete context;
        SetStatus(hDlg, m_pText->Str(StringId::kMsgStatusReady), true);
        return;
    }

    m_testRunning = true;
    EnableWindow(GetDlgItem(hDlg, IDC_BTN_TEST), FALSE);
    SetStatus(hDlg, m_pText->Str(StringId::kMsgTesting), false);
}

DWORD WINAPI COptionsDialog::TestCaptureProc(LPVOID param)
{
    TestCaptureContext* context = static_cast<TestCaptureContext*>(param);
    if (context == nullptr)
    {
        return 0;
    }

    // 全部在自己的线程上完成：DPI 上下文与 COM 都只影响这一个线程。
    CImageWriter writer;
    writer.InitThread();

    SnapCode code = kOk;
    CapturedFrame frame = CCaptureEngine().CaptureFullScreen(context->settings.includeCursor, code);
    std::wstring path;
    std::wstring detail;
    if (code == kOk && frame.IsValid())
    {
        CPluginSettings settings;
        settings.data = context->settings;
        code = writer.Save(frame, settings, path, detail);
    }
    frame.Reset();
    writer.ShutdownThread();

    SessionResult* result = new (std::nothrow) SessionResult();
    if (result != nullptr)
    {
        result->code = code;
        result->path = path;
        result->detail = detail;
        if (PostMessageW(context->hDlg, WM_SNAP_TEST_RESULT, 0, reinterpret_cast<LPARAM>(result)) == FALSE)
        {
            delete result;
        }
    }
    delete context;
    return 0;
}

void COptionsDialog::OnTestResult(HWND hDlg, const SessionResult* result)
{
    if (result == nullptr)
    {
        return;
    }
    if (result->code == kOk)
    {
        std::wstring message = snap::FormatString(m_pText->Str(StringId::kNotifyTestOk),
                                                  snap::GetFileNamePart(result->path).c_str());
        SetStatus(hDlg, message, false);
    }
    else
    {
        std::wstring message = snap::FormatString(m_pText->Str(StringId::kNotifyTestFailed),
                                                  m_pText->SnapCodeText(result->code), result->detail.c_str());
        SetStatus(hDlg, message, true);
    }
}

// 目录浏览在专用 STA 线程上进行（DEF-09 最终方案）。
// 根因链：SHBrowseForFolderW（无论新旧样式）要求调用线程是 COM STA 单元；
// TrafficMonitor 1.8.6 的 UI 线程从未调用 OleInitialize（宿主源码核实），
// 且 COM 单元模式一旦被其它组件设定便随线程终身固定、不可逆转 —— 若宿主 UI 线程
// 已是 MTA，任何同步调用浏览对话框的写法都会永久挂死（真实事故：点"浏览"后
// 整个对话框"未响应"）。把浏览挪到自建线程上（新线程必然可以 OleInitialize 成
// STA），宿主 UI 线程只负责收结果消息，从架构上杜绝阻塞。
namespace
{
const UINT WM_SNAP_BROWSE_DONE = WM_APP + 0x22;

struct BrowseArg
{
    HWND hDlg;
    std::wstring title;
    PIDLIST_ABSOLUTE pidl{nullptr};
};

DWORD WINAPI BrowseThreadProc(LPVOID param)
{
    BrowseArg* arg = static_cast<BrowseArg*>(param);
    if (arg == nullptr)
    {
        return 0;
    }

    HRESULT oleHr = OleInitialize(nullptr);
    const bool oleByUs = (oleHr == S_OK);

    BROWSEINFOW info{};
    info.hwndOwner = nullptr; // owner 在另一线程上，不设 owner；由 UI 线程自行禁用按钮防连点
    info.lpszTitle = arg->title.c_str();
    info.ulFlags = BIF_RETURNONLYFSDIRS;
    if (SUCCEEDED(oleHr))
    {
        info.ulFlags |= BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    }

    wchar_t displayName[MAX_PATH] = {0};
    info.pszDisplayName = displayName;
    arg->pidl = SHBrowseForFolderW(&info);

    // 结果所有权经消息移交 UI 线程；对话框已销毁时自行清理，避免泄漏。
    if (PostMessageW(arg->hDlg, WM_SNAP_BROWSE_DONE, 0, reinterpret_cast<LPARAM>(arg)) == FALSE)
    {
        if (arg->pidl != nullptr)
        {
            CoTaskMemFree(arg->pidl);
        }
        delete arg;
    }

    if (oleByUs)
    {
        OleUninitialize();
    }
    return 0;
}
} // namespace

void COptionsDialog::OnBrowse(HWND hDlg)
{
    if (m_browseRunning)
    {
        return;
    }
    BrowseArg* arg = new (std::nothrow) BrowseArg{hDlg, m_pText->Str(StringId::kLblSaveDir), nullptr};
    if (arg == nullptr)
    {
        return;
    }
    EnableWindow(GetDlgItem(hDlg, IDC_BTN_BROWSE), FALSE); // 防连点；结果回来时恢复
    m_browseRunning = true;
    HANDLE thread = CreateThread(nullptr, 0, BrowseThreadProc, arg, 0, nullptr);
    if (thread == nullptr)
    {
        delete arg;
        m_browseRunning = false;
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_BROWSE), TRUE);
        return;
    }
    CloseHandle(thread);
}

void COptionsDialog::OnOpenDir(HWND hDlg)
{
    std::wstring dir = snap::Trim(GetDlgText(hDlg, IDC_SAVE_DIR));
    if (dir.empty())
    {
        SetStatus(hDlg, m_pText->Str(StringId::kMsgDirMissing), true);
        return;
    }
    if (!snap::EnsureDirectory(dir))
    {
        SetStatus(hDlg, m_pText->Str(StringId::kMsgDirMissing), true);
        return;
    }
    ShellExecuteW(hDlg, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void COptionsDialog::OnReset(HWND hDlg)
{
    PluginSettings defaults;
    if (m_pSettings != nullptr)
    {
        m_pSettings->ApplyDefaults(defaults);
    }
    m_work = defaults;
    m_hotkey = defaults.hotkey1.binding;
    m_hotkey2 = defaults.hotkey2.binding;
    m_recordingSlot = 0;
    LoadToControls(hDlg);
    UpdateHotkeyButton(hDlg);
    SetStatus(hDlg, m_pText->Str(StringId::kMsgStatusReady), false);
}

void COptionsDialog::StartHotkeyRecording(HWND hDlg, int slot)
{
    if (slot == 1 && !IsChecked(hDlg, IDC_HOTKEY_ENABLED))
    {
        return;
    }
    if (slot == 2 && !IsChecked(hDlg, IDC_HOTKEY2_ENABLED))
    {
        return;
    }
    m_recordingSlot = slot;
    UpdateHotkeyButton(hDlg);
    HWND button = (slot == 2) ? GetDlgItem(hDlg, IDC_HOTKEY2) : GetDlgItem(hDlg, IDC_HOTKEY);
    if (button != nullptr)
    {
        SetFocus(button);
    }
}

void COptionsDialog::OnOk(HWND hDlg)
{
    if (m_pSettings == nullptr)
    {
        EndDialog(hDlg, IDCANCEL);
        return;
    }

    PluginSettings next;
    std::wstring err;
    if (!ReadFromControls(hDlg, next, err))
    {
        SetStatus(hDlg, err, true);
        return;
    }

    if (!snap::EnsureDirectory(next.saveDir) || !snap::IsDirectoryWritable(next.saveDir))
    {
        SetStatus(hDlg, snap::FormatString(m_pText->Str(StringId::kMsgDirNotWritable), next.saveDir.c_str()),
                  true);
        return;
    }

    const bool hotkey1Changed = next.hotkey1.enabled != m_pSettings->data.hotkey1.enabled ||
                                next.hotkey1.action != m_pSettings->data.hotkey1.action ||
                                next.hotkey1.binding.vk != m_pSettings->data.hotkey1.binding.vk ||
                                next.hotkey1.binding.modifiers != m_pSettings->data.hotkey1.binding.modifiers;
    const bool hotkey2Changed = next.hotkey2.enabled != m_pSettings->data.hotkey2.enabled ||
                                next.hotkey2.action != m_pSettings->data.hotkey2.action ||
                                next.hotkey2.binding.vk != m_pSettings->data.hotkey2.binding.vk ||
                                next.hotkey2.binding.modifiers != m_pSettings->data.hotkey2.binding.modifiers;
    const bool hotkeyChanged = hotkey1Changed || hotkey2Changed;

    if (hotkeyChanged && m_pHotkeys != nullptr)
    {
        if (!m_pHotkeys->Apply(next.hotkey1, next.hotkey2))
        {
            SetStatus(hDlg, m_pText->Str(StringId::kMsgHotkeyConflict), true);
            return;
        }
    }

    const PluginSettings backup = m_pSettings->data;
    m_pSettings->data = next;
    if (m_pSettings->IsPersistent() && !m_pSettings->Save())
    {
        m_pSettings->data = backup;
        if (hotkeyChanged && m_pHotkeys != nullptr)
        {
            m_pHotkeys->Apply(backup.hotkey1, backup.hotkey2);
        }
        SetStatus(hDlg, snap::FormatString(m_pText->Str(StringId::kMsgSaveFailed),
                                           m_pSettings->IniPath().c_str()),
                  true);
        return;
    }

    m_changed = true;
    EndDialog(hDlg, IDOK);
}

INT_PTR CALLBACK COptionsDialog::DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INITDIALOG)
    {
        COptionsDialog* self = reinterpret_cast<COptionsDialog*>(lp);
        SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        if (self == nullptr)
        {
            return FALSE;
        }

        const int dpi = static_cast<int>(snap::GetDpiForHwndLazy(hDlg));
        self->m_hFont = reinterpret_cast<HFONT>(SendMessageW(hDlg, WM_GETFONT, 0, 0));

        // Re-layout in exact pixels.  This is the single source of DPI truth.
        self->LayoutControls(hDlg, dpi);

        EnumChildWindows(hDlg, &SetChildFontProc, reinterpret_cast<LPARAM>(self->m_hFont));

        self->LoadToControls(hDlg);

        HWND hotkeyButton = GetDlgItem(hDlg, IDC_HOTKEY);
        if (hotkeyButton != nullptr)
        {
            self->m_oldHotkeyProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                hotkeyButton, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&COptionsDialog::HotkeyProc)));
        }
        HWND hotkeyButton2 = GetDlgItem(hDlg, IDC_HOTKEY2);
        if (hotkeyButton2 != nullptr)
        {
            self->m_oldHotkeyProc2 = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                hotkeyButton2, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&COptionsDialog::HotkeyProc)));
        }

        self->UpdateDerivedState(hDlg);
        return TRUE;
    }

    COptionsDialog* self = reinterpret_cast<COptionsDialog*>(GetWindowLongPtrW(hDlg, DWLP_USER));
    if (self == nullptr)
    {
        return FALSE;
    }

    switch (msg)
    {
    case WM_COMMAND:
    {
        const int id = LOWORD(wp);
        const int notification = HIWORD(wp);
        switch (id)
        {
        case IDC_BTN_BROWSE:
            self->OnBrowse(hDlg);
            return TRUE;
        case IDC_BTN_OPEN_DIR:
            self->OnOpenDir(hDlg);
            return TRUE;
        case IDC_FORMAT:
            if (notification == CBN_SELCHANGE)
            {
                self->UpdateDerivedState(hDlg);
            }
            return TRUE;
        case IDC_TEMPLATE:
            if (notification == EN_CHANGE)
            {
                self->UpdateDerivedState(hDlg);
            }
            return TRUE;
        case IDC_HOTKEY_ENABLED:
            if (notification == BN_CLICKED)
            {
                if (self->m_recordingSlot == 1)
                {
                    self->m_recordingSlot = 0;
                }
                self->UpdateHotkeyButton(hDlg);
                self->UpdateDerivedState(hDlg);
            }
            return TRUE;
        case IDC_HOTKEY2_ENABLED:
            if (notification == BN_CLICKED)
            {
                if (self->m_recordingSlot == 2)
                {
                    self->m_recordingSlot = 0;
                }
                self->UpdateHotkeyButton(hDlg);
                self->UpdateDerivedState(hDlg);
            }
            return TRUE;
        case IDC_HOTKEY:
            if (notification == BN_CLICKED)
            {
                self->StartHotkeyRecording(hDlg, 1);
            }
            return TRUE;
        case IDC_HOTKEY2:
            if (notification == BN_CLICKED)
            {
                self->StartHotkeyRecording(hDlg, 2);
            }
            return TRUE;
        case IDC_QUALITY_MINUS:
        {
            int value = GetDlgInt(hDlg, IDC_QUALITY, 90, 1, 100);
            value = (value > 1) ? value - 1 : 1;
            SetDlgItemInt(hDlg, IDC_QUALITY, static_cast<UINT>(value), FALSE);
            return TRUE;
        }
        case IDC_QUALITY_PLUS:
        {
            int value = GetDlgInt(hDlg, IDC_QUALITY, 90, 1, 100);
            value = (value < 100) ? value + 1 : 100;
            SetDlgItemInt(hDlg, IDC_QUALITY, static_cast<UINT>(value), FALSE);
            return TRUE;
        }
        case IDC_DELAY_MINUS:
        {
            int value = GetDlgInt(hDlg, IDC_DELAY, 0, 0, 30);
            value = (value > 0) ? value - 1 : 0;
            SetDlgItemInt(hDlg, IDC_DELAY, static_cast<UINT>(value), FALSE);
            return TRUE;
        }
        case IDC_DELAY_PLUS:
        {
            int value = GetDlgInt(hDlg, IDC_DELAY, 0, 0, 30);
            value = (value < 30) ? value + 1 : 30;
            SetDlgItemInt(hDlg, IDC_DELAY, static_cast<UINT>(value), FALSE);
            return TRUE;
        }
        case IDC_BTN_TEST:
            self->OnTestCapture(hDlg);
            return TRUE;
        case IDC_BTN_RESET:
            self->OnReset(hDlg);
            return TRUE;
        case IDOK:
            self->OnOk(hDlg);
            return TRUE;
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        return FALSE;
    }

    case WM_SNAP_BROWSE_DONE:
    {
        // 目录浏览线程的结果：pidl 所有权随消息移交（BrowseThreadProc 注释）。
        BrowseArg* arg = reinterpret_cast<BrowseArg*>(lp);
        self->m_browseRunning = false;
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_BROWSE), TRUE);
        if (arg != nullptr)
        {
            if (arg->pidl != nullptr)
            {
                wchar_t selected[MAX_PATH] = {0};
                if (SHGetPathFromIDListW(arg->pidl, selected) != FALSE && selected[0] != L'\0')
                {
                    SetDlgItemTextW(hDlg, IDC_SAVE_DIR, selected);
                }
                CoTaskMemFree(arg->pidl);
            }
            delete arg;
        }
        return TRUE;
    }

    case WM_SNAP_TEST_RESULT:
    {
        SessionResult* result = reinterpret_cast<SessionResult*>(lp);
        self->m_testRunning = false;
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_TEST), TRUE);
        if (self->m_hTestThread != nullptr)
        {
            WaitForSingleObject(self->m_hTestThread, 3000);
            CloseHandle(self->m_hTestThread);
            self->m_hTestThread = nullptr;
        }
        if (result != nullptr)
        {
            self->OnTestResult(hDlg, result);
            delete result;
        }
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC dc = reinterpret_cast<HDC>(wp);
        HWND control = reinterpret_cast<HWND>(lp);
        const int id = GetDlgCtrlID(control);
        if (id == IDC_STATUS)
        {
            SetTextColor(dc, self->m_statusError ? RGB(190, 30, 30) : RGB(20, 120, 20));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        }
        if (id == IDC_EXAMPLE)
        {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    }

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;

    case WM_DESTROY:
        if (self->m_hTestThread != nullptr)
        {
            WaitForSingleObject(self->m_hTestThread, 3000);
            CloseHandle(self->m_hTestThread);
            self->m_hTestThread = nullptr;
            self->m_testRunning = false;
        }
        if (self->m_hHotkeyButton != nullptr && self->m_oldHotkeyProc != nullptr)
        {
            SetWindowLongPtrW(self->m_hHotkeyButton, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(self->m_oldHotkeyProc));
            self->m_oldHotkeyProc = nullptr;
            self->m_hHotkeyButton = nullptr;
        }
        return TRUE;

    default:
        break;
    }
    return FALSE;
}

LRESULT CALLBACK COptionsDialog::HotkeyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND hDlg = GetParent(hwnd);
    COptionsDialog* self =
        (hDlg != nullptr) ? reinterpret_cast<COptionsDialog*>(GetWindowLongPtrW(hDlg, DWLP_USER)) : nullptr;
    if (self == nullptr)
    {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg)
    {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;

    case WM_LBUTTONDOWN:
    {
        const int slot = (hwnd == self->m_hHotkeyButton2) ? 2 : 1;
        self->StartHotkeyRecording(hDlg, slot);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        if (self->m_recordingSlot == 0)
        {
            break;
        }
        const UINT vk = static_cast<UINT>(wp);
        UINT modifiers = 0;
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        {
            modifiers |= MOD_CONTROL;
        }
        if ((GetKeyState(VK_MENU) & 0x8000) != 0)
        {
            modifiers |= MOD_ALT;
        }
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
        {
            modifiers |= MOD_SHIFT;
        }
        if ((GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0)
        {
            modifiers |= MOD_WIN;
        }

        HotkeyBinding candidate;
        candidate.modifiers = modifiers;
        candidate.vk = vk;

        if (!candidate.IsValid())
        {
            if (IsModifierKey(vk))
            {
                // Keep waiting: the user may still add a real key.
                return 0;
            }
            self->SetStatus(hDlg, self->m_pText->Str(StringId::kMsgNeedModifier), true);
            return 0;
        }

        if (self->m_recordingSlot == 1)
        {
            self->m_hotkey = candidate;
        }
        else
        {
            self->m_hotkey2 = candidate;
        }
        const HotkeyBinding recorded = (self->m_recordingSlot == 1) ? self->m_hotkey : self->m_hotkey2;
        self->m_recordingSlot = 0;
        self->UpdateHotkeyButton(hDlg);
        self->SetStatus(hDlg, recorded.ToText(), false);
        return 0;
    }

    case WM_CHAR:
    case WM_SYSCHAR:
        if (self->m_recordingSlot != 0)
        {
            return 0;
        }
        break;

    default:
        break;
    }

    WNDPROC oldProc = (hwnd == self->m_hHotkeyButton2) ? self->m_oldHotkeyProc2 : self->m_oldHotkeyProc;
    if (oldProc != nullptr)
    {
        return CallWindowProcW(oldProc, hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
