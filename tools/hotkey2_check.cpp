// hotkey2_check: verify the second hotkey slot end-to-end against the real DLL.
//  - mock host reports GetLanguageId()=0 / GetAPIVersion()=0 (like TrafficMonitor 1.8.6)
//  - ini preseeded: hotkey2 enabled, vk='R' (82), action=0 (full screen), save dir = temp
//  - PostMessage(WM_HOTKEY, id=2) must produce exactly one PNG in the save dir
#include "../include/PluginInterface.h"
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstdint>
#include <string>

static std::wstring g_dir;

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

static int CountPng(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\TMSnap_*.png").c_str(), &fd);
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

int main()
{
    wchar_t tmpPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpPath);
    g_dir = std::wstring(tmpPath) + L"tmshot_hotkey2";
    CreateDirectoryW(g_dir.c_str(), nullptr);

    // Clean old captures so the count delta is exact.
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"\\TMSnap_*.png").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            DeleteFileW((g_dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Prepare the ini (UTF-16LE BOM first, as required by the plugin's loader).
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
    WritePrivateProfileStringW(L"TMSnap", L"hotkey_enabled", L"0", ini.c_str());       // slot 1 off
    WritePrivateProfileStringW(L"TMSnap", L"hotkey2_enabled", L"1", ini.c_str());      // slot 2 on
    WritePrivateProfileStringW(L"TMSnap", L"hotkey2_mods", L"ctrl+alt", ini.c_str());
    WritePrivateProfileStringW(L"TMSnap", L"hotkey2_vk", L"82", ini.c_str());          // 'R'
    WritePrivateProfileStringW(L"TMSnap", L"hotkey2_action", L"0", ini.c_str());       // full screen

    HMODULE dll = LoadLibraryW(L"TMPluginScreenshot.dll");
    if (!dll)
    {
        printf("LoadLibrary failed\n");
        return 1;
    }
    auto get = reinterpret_cast<ITMPlugin * (*)()>(
        reinterpret_cast<void*>(GetProcAddress(dll, "TMPluginGetInstance")));
    if (!get)
    {
        printf("GetProcAddress failed\n");
        return 1;
    }
    ITMPlugin* plugin = get();
    MockApp app;
    plugin->OnInitialize(&app);

    // Locate the sink window (message-only).
    HWND sink = FindWindowExW(reinterpret_cast<HWND>(0xFFFFFFFFFFFFFFFDULL), nullptr,
                              L"TMPluginScreenshotSink", nullptr);
    if (sink == nullptr)
    {
        printf("sink window not found\n");
        return 2;
    }
    printf("sink hwnd = 0x%X\n", static_cast<unsigned>(reinterpret_cast<uintptr_t>(sink) & 0xFFFFFFFF));

    const int before = CountPng(g_dir);
    // WM_HOTKEY for slot 2: wParam = kHotkeyId2 = 2, lParam = mods<<16 | vk
    const LPARAM lp = static_cast<LPARAM>(((1 | 4 | 0x4000u) << 16) | 82u);
    for (int i = 0; i < 2; ++i)
    {
        PostMessageW(sink, WM_HOTKEY, 2, lp);
        PumpFor(1500); // 控制台进程必须自己泵消息，WM_HOTKEY 才会被分发到 sink 窗口
    }
    const int after = CountPng(g_dir);
    printf("captures before=%d after=%d\n", before, after);

    // Slot 1 is disabled: id=1 must NOT capture.
    PostMessageW(sink, WM_HOTKEY, 1, lp);
    PumpFor(1500);
    const int finalCount = CountPng(g_dir);

    plugin->DataRequired();
    FreeLibrary(dll);

    bool pass = (after - before == 2) && (finalCount == after);
    printf("%s\n", pass ? "RESULT: HOTKEY2 CHECK PASSED" : "RESULT: HOTKEY2 CHECK FAILED");
    return pass ? 0 : 3;
}
