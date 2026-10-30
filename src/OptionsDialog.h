// OptionsDialog.h - zero-resource modal settings dialog (in-memory DLGTEMPLATE).
#pragma once

#include "PluginSettings.h"
#include "HotkeyManager.h"
#include "PluginInterface.h"

// Declared in CaptureEngine.h; only ever handled as an opaque pointer here so
// that this header keeps its narrow dependency set.
struct SessionResult;

// Control ids.  The OK/Cancel buttons deliberately reuse IDOK / IDCANCEL so we
// inherit the standard Enter / Esc behaviour of the dialog manager.
enum : int
{
    IDC_SAVE_DIR = 1001,
    IDC_BTN_BROWSE = 1002,
    IDC_BTN_OPEN_DIR = 1003,
    IDC_FORMAT = 1004,
    IDC_QUALITY = 1005,
    IDC_QUALITY_MINUS = 1006,
    IDC_QUALITY_PLUS = 1007,
    IDC_TEMPLATE = 1008,
    IDC_EXAMPLE = 1009,
    IDC_CLIPBOARD = 1010,
    IDC_DELAY = 1011,
    IDC_DELAY_MINUS = 1012,
    IDC_DELAY_PLUS = 1013,
    IDC_CURSOR = 1014,
    IDC_HOTKEY_ENABLED = 1015,
    IDC_HOTKEY = 1016,
    IDC_BTN_TEST = 1017,
    IDC_ABOUT = 1018,
    IDC_STATUS = 1019,
    IDC_BTN_RESET = 1020,
    IDC_GRP_SAVE = 1021,
    IDC_GRP_CAPTURE = 1022,
    IDC_GRP_ABOUT = 1023,
    IDC_LBL_SAVE_DIR = 1024,
    IDC_LBL_FORMAT = 1025,
    IDC_LBL_QUALITY = 1026,
    IDC_LBL_TEMPLATE = 1027,
    IDC_LBL_DELAY = 1028,
    IDC_LBL_HOTKEY = 1029,
    IDC_LBL_ACTION1 = 1030,
    IDC_ACTION1 = 1031,
    IDC_HOTKEY2_ENABLED = 1032,
    IDC_LBL_ACTION2 = 1033,
    IDC_ACTION2 = 1034,
    IDC_HOTKEY2 = 1035
};

class COptionsDialog
{
public:
    COptionsDialog(CPluginSettings* settings, CUxText* text, CHotkeyManager* hotkeys);
    ~COptionsDialog();

    COptionsDialog(const COptionsDialog&) = delete;
    COptionsDialog& operator=(const COptionsDialog&) = delete;

    ITMPlugin::OptionReturn Show(HWND hParent);

private:
    // "立即截屏" 的上下文：整条抓屏+编码都在自己的线程上跑，绝不占用宿主 UI 线程
    // （不改宿主线程的 DPI 上下文、不阻塞 0.3–0.7s）。结果经 WM_SNAP_TEST_RESULT
    // 以 SessionResult* 回投，所有权随消息转移。
    struct TestCaptureContext
    {
        PluginSettings settings;
        HWND hDlg{nullptr};
    };

    static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK HotkeyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static DWORD WINAPI TestCaptureProc(LPVOID param);

    void BuildTemplate(std::vector<BYTE>& out) const;
    void LayoutControls(HWND hDlg, int dpi);
    void LoadToControls(HWND hDlg);
    bool ReadFromControls(HWND hDlg, PluginSettings& out, std::wstring& err);
    void UpdateDerivedState(HWND hDlg);
    void UpdateExample(HWND hDlg);
    void SetStatus(HWND hDlg, const std::wstring& text, bool isError);
    void OnTestCapture(HWND hDlg);
    void OnTestResult(HWND hDlg, const SessionResult* result);
    void OnBrowse(HWND hDlg);
    void OnOpenDir(HWND hDlg);
    void OnReset(HWND hDlg);
    void OnOk(HWND hDlg);
    void StartHotkeyRecording(HWND hDlg, int slot);
    void UpdateHotkeyButton(HWND hDlg);

    CPluginSettings* m_pSettings{nullptr};
    CUxText* m_pText{nullptr};
    CHotkeyManager* m_pHotkeys{nullptr};
    PluginSettings m_work;
    HotkeyBinding m_hotkey;
    HotkeyBinding m_hotkey2;
    bool m_changed{false};
    int m_recordingSlot{0}; // 0 = 未在录制；1 / 2 = 正在为热键 1 / 热键 2 录制
    bool m_statusError{false};
    WNDPROC m_oldHotkeyProc{nullptr};
    WNDPROC m_oldHotkeyProc2{nullptr};
    HWND m_hHotkeyButton{nullptr};
    HWND m_hHotkeyButton2{nullptr};
    HFONT m_hFont{nullptr};
    HANDLE m_hTestThread{nullptr};
    bool m_testRunning{false};
    bool m_browseRunning{false}; // 目录浏览在专用 STA 线程上进行（DEF-09）
};
