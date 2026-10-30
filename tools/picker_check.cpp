// picker_check: end-to-end test for the v1.2.0 window picker.
//  - ini: hotkey2 = Ctrl+Alt+R, action = 2 (active window)
//  - create a visible dummy target window ("tmshot-target", 400x300 at 60,60)
//  - trigger WM_HOTKEY(id=2) -> the picker dialog must appear
//  - select the entry containing "tmshot-target", confirm
//  - expect exactly one new PNG whose size equals the dummy window's DWM bounds
//  - trigger again and cancel -> no new file
#include "../include/PluginInterface.h"
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
#include <string>

static std::wstring g_dir;
static ITMPlugin* g_plugin = nullptr;
static HWND g_sink = nullptr;

class MockApp : public ITrafficMonitor
{
public:
    int GetAPIVersion() override { return 0; }
    const wchar_t* GetVersion() override { return L"1.8.6"; }
    double GetMonitorValue(MonitorItem) override { return 0; }
    const wchar_t* GetMonitorValueString(MonitorItem, int) override { return L""; }
    void ShowNotifyMessage(const wchar_t* msg) override { wprintf(L"[notify] %s\n", msg); }
    unsigned short GetLanguageId() const override { return 0; }
    const wchar_t* GetPluginConfigDir() const override { return g_dir.c_str(); }
    int GetDPI(DPIType) const override { return 96; }
    unsigned int GetThemeColor() const override { return 0; }
    const wchar_t* GetStringRes(const wchar_t*, const wchar_t*) override { return L""; }
    void* GetMainWindowHwnd() override { return nullptr; }
    void* GetTaskbarWindowHwnd() override { return nullptr; }
};

struct Finder
{
    const wchar_t* titleNeedle;
    HWND result;
};

static BOOL CALLBACK FindPickerProc(HWND hwnd, LPARAM lp)
{
    Finder* f = reinterpret_cast<Finder*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || IsWindowVisible(hwnd) == FALSE)
    {
        return TRUE;
    }
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"#32770") != 0)
    {
        return TRUE;
    }
    if (f->titleNeedle != nullptr)
    {
        wchar_t title[128] = {};
        GetWindowTextW(hwnd, title, 128);
        if (wcsstr(title, f->titleNeedle) == nullptr)
        {
            return TRUE;
        }
    }
    f->result = hwnd;
    return FALSE;
}

static HWND FindPicker(const wchar_t* needle)
{
    Finder f{needle, nullptr};
    EnumWindows(FindPickerProc, reinterpret_cast<LPARAM>(&f));
    return f.result;
}

static int CountPng()
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"\\TMSnap_*.png").c_str(), &fd);
    int n = 0;
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            ++n;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return n;
}

static void PumpFor(DWORD ms)
{
    const ULONGLONG deadline = GetTickCount64() + ms;
    MSG msg;
    while (GetTickCount64() < deadline)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
}

static bool ReadPngSize(const std::wstring& path, int& w, int& h)
{
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || fp == nullptr)
    {
        return false;
    }
    unsigned char head[24] = {};
    const size_t got = fread(head, 1, 24, fp);
    fclose(fp);
    if (got < 24 || head[0] != 0x89 || head[1] != 'P' || head[2] != 'N' || head[3] != 'G')
    {
        return false;
    }
    w = static_cast<int>((head[16] << 24) | (head[17] << 16) | (head[18] << 8) | head[19]);
    h = static_cast<int>((head[20] << 24) | (head[21] << 16) | (head[22] << 8) | head[23]);
    return true;
}

static std::wstring NewestPng()
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"\\TMSnap_*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return L"";
    }
    FILETIME best{};
    std::wstring bestName;
    do
    {
        if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0 || bestName.empty())
        {
            best = fd.ftLastWriteTime;
            bestName = g_dir + L"\\" + fd.cFileName;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return bestName;
}

int main()
{
    wchar_t tmpPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpPath);
    g_dir = std::wstring(tmpPath) + L"tmshot_picker";
    CreateDirectoryW(g_dir.c_str(), nullptr);

    std::wstring ini = g_dir + L"\\TMPluginScreenshot.ini";
    HANDLE f = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        printf("cannot create ini\n");
        return 1;
    }
    const unsigned char bom[2] = {0xFF, 0xFE};
    DWORD written = 0;
    WriteFile(f, bom, 2, &written, nullptr);
    CloseHandle(f);
    WritePrivateProfileStringW(L"TMSnap", L"save_dir", g_dir.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"TMSnap", L"format", L"png", ini.c_str());
    WritePrivateProfileStringW(L"TMSnap", L"copy_to_clipboard", L"0", ini.c_str());
    WritePrivateProfileStringW(L"TMSnap", L"hotkey_enabled", L"0", ini.c_str());
    WritePrivateProfileStringW(L"TMSnap", L"hotkey2_enabled", L"1", ini.c_str());
    WritePrivateProfileStringW(L"TMSnap", L"hotkey2_mods", L"ctrl+alt", ini.c_str());
    WritePrivateProfileStringW(L"TMSnap", L"hotkey2_vk", L"82", ini.c_str());
    WritePrivateProfileStringW(L"TMSnap", L"hotkey2_action", L"2", ini.c_str()); // active window

    HMODULE dll = LoadLibraryW(L"TMPluginScreenshot.dll");
    if (!dll)
    {
        printf("LoadLibrary failed\n");
        return 1;
    }
    auto get = reinterpret_cast<ITMPlugin * (*)()>(
        reinterpret_cast<void*>(GetProcAddress(dll, "TMPluginGetInstance")));
    g_plugin = get();

    MockApp app;
    g_plugin->OnInitialize(&app);

    // The dummy capture target.
    HWND target = CreateWindowExW(0, L"STATIC", L"tmshot-target",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 400, 300, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (target == nullptr)
    {
        printf("dummy window failed\n");
        return 1;
    }
    RECT expected{};
    if (DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &expected, sizeof(expected)) != S_OK)
    {
        GetWindowRect(target, &expected);
    }
    const int expW = static_cast<int>(expected.right - expected.left);
    const int expH = static_cast<int>(expected.bottom - expected.top);
    printf("target bounds %dx%d at (%ld,%ld)\n", expW, expH, expected.left, expected.top);

    g_sink = FindWindowExW(reinterpret_cast<HWND>(0xFFFFFFFFFFFFFFFDULL), nullptr,
                           L"TMPluginScreenshotSink", nullptr);
    if (g_sink == nullptr)
    {
        printf("sink not found\n");
        return 2;
    }
    const LPARAM lp = static_cast<LPARAM>(((1 | 4 | 0x4000u) << 16) | 82u);

    // --- pass 1: pick the dummy window ---
    const int before = CountPng();
    PostMessageW(g_sink, WM_HOTKEY, 2, lp);
    PumpFor(2000);

    HWND picker = nullptr;
    for (int i = 0; i < 40 && picker == nullptr; ++i)
    {
        Sleep(100);
        picker = FindPicker(L"tmshot"); // fallback title check later; first by any new #32770
    }
    if (picker == nullptr)
    {
        picker = FindPicker(nullptr);
    }
    if (picker == nullptr)
    {
        printf("picker dialog not found\n");
        return 3;
    }
    printf("picker dialog = 0x%X\n", static_cast<unsigned>(reinterpret_cast<uintptr_t>(picker) & 0xFFFFFFFF));

    // Find the listbox and select the entry containing "tmshot-target".
    HWND list = FindWindowExW(picker, nullptr, L"ListBox", nullptr);
    if (list == nullptr)
    {
        printf("listbox not found in picker\n");
        return 4;
    }
    int targetIndex = -1;
    const int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i)
    {
        wchar_t line[512] = {};
        SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(line));
        if (wcsstr(line, L"tmshot-target") != nullptr)
        {
            targetIndex = i;
            break;
        }
    }
    if (targetIndex < 0)
    {
        printf("target window not listed (count=%d)\n", count);
        return 5;
    }
    printf("list count=%d, target index=%d\n", count, targetIndex);
    SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(targetIndex), 0);
    PostMessageW(picker, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    PumpFor(3000);

    const int afterPick = CountPng();
    printf("captures after pick: %d (before %d)\n", afterPick, before);
    if (afterPick - before != 1)
    {
        printf("RESULT: PICKER CHECK FAILED (no capture)\n");
        return 6;
    }
    const std::wstring newest = NewestPng();
    int w = 0, h = 0;
    if (!ReadPngSize(newest, w, h))
    {
        printf("RESULT: PICKER CHECK FAILED (unreadable png)\n");
        return 7;
    }
    printf("captured %dx%d, expected %dx%d\n", w, h, expW, expH);
    if (w != expW || h != expH)
    {
        printf("RESULT: PICKER CHECK FAILED (size mismatch)\n");
        return 8;
    }

    // --- pass 2: cancel path ---
    PostMessageW(g_sink, WM_HOTKEY, 2, lp);
    PumpFor(2000);
    HWND picker2 = nullptr;
    for (int i = 0; i < 40 && picker2 == nullptr; ++i)
    {
        Sleep(100);
        picker2 = FindPicker(nullptr);
    }
    if (picker2 != nullptr)
    {
        PostMessageW(picker2, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        PumpFor(2000);
    }
    const int afterCancel = CountPng();
    printf("captures after cancel: %d\n", afterCancel);
    if (afterCancel != afterPick)
    {
        printf("RESULT: PICKER CHECK FAILED (cancel produced a file)\n");
        return 9;
    }

    FreeLibrary(dll);
    DestroyWindow(target);
    printf("RESULT: PICKER CHECK PASSED\n");
    return 0;
}
