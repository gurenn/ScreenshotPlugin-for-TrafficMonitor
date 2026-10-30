// browse_check: verify the Browse button no longer hangs the settings dialog on a host-like
// thread that never initialized COM.
//
// Structure mirrors the real host: the settings dialog runs modally on the MAIN thread
// (the same thread that called OnInitialize, exactly like TrafficMonitor's UI thread),
// while a driver thread clicks Browse, probes responsiveness, and closes everything.
//
// PASS criteria:
//   1. settings dialog opens on the main thread;
//   2. after the programmatic "browse" click the dialog stays responsive
//      (SendMessageTimeout WM_NULL within 1.5s) -- pre-fix this hangs forever;
//   3. the folder browse dialog appears and can be closed programmatically;
//   4. the settings dialog then closes normally (cancel) and Show returns.
#include "../include/PluginInterface.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

static std::wstring g_dir;
static ITMPlugin* g_plugin = nullptr;
static HWND g_parent = nullptr;
static DWORD g_mainThreadId = 0;
static volatile LONG g_driverDone = 0;
static volatile LONG g_responsive = 0;
static volatile LONG g_browseAppeared = 0;
static volatile LONG g_browseClosed = 0;
static DWORD g_showResult = 0xDEADBEEF;

struct MockApp : public ITrafficMonitor
{
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

static DWORD g_processId = 0;

struct Finder
{
    HWND owner;
    HWND result;
};

static BOOL CALLBACK FindDialogProc(HWND hwnd, LPARAM lp)
{
    Finder* f = reinterpret_cast<Finder*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != g_processId)
    {
        return TRUE;
    }
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"#32770") != 0 || IsWindowVisible(hwnd) == FALSE)
    {
        return TRUE;
    }
    if (f->owner != nullptr && GetWindow(hwnd, GW_OWNER) != f->owner)
    {
        return TRUE;
    }
    f->result = hwnd;
    return FALSE;
}

static HWND FindDialog(HWND owner)
{
    Finder f{owner, nullptr};
    EnumWindows(FindDialogProc, reinterpret_cast<LPARAM>(&f));
    return f.result;
}

static DWORD WINAPI DriverThread(LPVOID)
{
    // 1. wait for the settings dialog (owned by our hidden parent, on the main thread).
    HWND dlg = nullptr;
    for (int i = 0; i < 50 && dlg == nullptr; ++i)
    {
        Sleep(100);
        dlg = FindDialog(g_parent);
    }
    if (dlg == nullptr)
    {
        printf("driver: settings dialog not found\n");
        InterlockedExchange(&g_driverDone, 1);
        return 1;
    }
    printf("driver: settings dialog = 0x%X\n",
           static_cast<unsigned>(reinterpret_cast<uintptr_t>(dlg) & 0xFFFFFFFF));

    // 2. click "Browse" (IDC_BTN_BROWSE = 1002).
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(1002, 0 /*BN_CLICKED*/), 0);
    Sleep(3000); // pre-fix the dialog thread hangs right here; post-fix the browse dialog comes up

    // 3. THE hang detector: a hung dialog thread will not answer WM_NULL in time.
    DWORD_PTR res = 0;
    const BOOL responsive =
        SendMessageTimeoutW(dlg, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &res);
    printf("driver: settings dialog responsive after browse click: %s\n",
           responsive ? "YES" : "NO (HUNG)");
    InterlockedExchange(&g_responsive, responsive ? 1 : 0);
    if (!responsive)
    {
        InterlockedExchange(&g_driverDone, 1);
        return 2;
    }

    // 4. the browse dialog should appear on the main thread (dedicated STA thread inside the
    //    DLL; the window itself is owner-less, so look for any visible #32770 that is not the
    //    settings dialog).
    HWND browse = nullptr;
    for (int i = 0; i < 60 && browse == nullptr; ++i)
    {
        Sleep(100);
        HWND cand = nullptr;
        for (int j = 0; j < 20 && cand == nullptr; ++j)
        {
            Finder f{nullptr, nullptr};
            EnumWindows(FindDialogProc, reinterpret_cast<LPARAM>(&f));
            cand = f.result;
            if (cand == dlg)
            {
                cand = nullptr;
            }
            if (cand == nullptr)
            {
                Sleep(50);
            }
        }
        browse = cand;
    }
    if (browse != nullptr)
    {
        InterlockedExchange(&g_browseAppeared, 1);
        printf("driver: browse dialog = 0x%X\n",
               static_cast<unsigned>(reinterpret_cast<uintptr_t>(browse) & 0xFFFFFFFF));
        PostMessageW(browse, WM_COMMAND, IDCANCEL, 0);
        for (int i = 0; i < 30 && IsWindow(browse); ++i)
        {
            Sleep(100);
            if (i == 10)
            {
                PostMessageW(browse, WM_CLOSE, 0, 0);
            }
        }
        InterlockedExchange(&g_browseClosed, IsWindow(browse) ? 0 : 1);
        printf("driver: browse dialog closed: %s\n", IsWindow(browse) ? "NO" : "YES");
    }
    else
    {
        printf("driver: browse dialog NOT found\n");
    }

    // 5. cancel the settings dialog.
    PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
    InterlockedExchange(&g_driverDone, 1);
    return 0;
}

int main(int argc, char** argv)
{
    const bool mtaScenario = (argc > 1 && strcmp(argv[1], "--mta") == 0);
    if (mtaScenario)
    {
        // 模拟真实宿主：UI 线程已被某个组件初始化为 MTA。
        // 注意单元模式随线程终身固定，本场景必须在独立进程中跑（--mta）。
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        printf("scenario: MTA thread (CoInitializeEx MTA -> 0x%08X)\n", static_cast<unsigned>(hr));
    }
    else
    {
        printf("scenario: no COM unit at all (default)\n");
    }
    wchar_t tmpPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpPath);
    g_dir = std::wstring(tmpPath) + L"tmshot_browse";
    CreateDirectoryW(g_dir.c_str(), nullptr);

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
    g_plugin = get();

    MockApp app;
    g_plugin->OnInitialize(&app);

    g_parent = CreateWindowExW(0, L"STATIC", L"tmshot-parent", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_parent == nullptr)
    {
        printf("parent window failed\n");
        return 1;
    }
    g_mainThreadId = GetCurrentThreadId();
    g_processId = GetCurrentProcessId();

    HANDLE driver = CreateThread(nullptr, 0, DriverThread, nullptr, 0, nullptr);
    if (driver == nullptr)
    {
        printf("driver thread failed\n");
        return 1;
    }

    // Modal dialog on the main thread -- same shape as the real host UI thread.
    g_showResult = static_cast<DWORD>(g_plugin->ShowOptionsDialog(g_parent));
    printf("main: ShowOptionsDialog returned %X\n", g_showResult);

    WaitForSingleObject(driver, 15000);
    FreeLibrary(dll);
    DestroyWindow(g_parent);

    const bool dialogClosed = (g_showResult == static_cast<DWORD>(ITMPlugin::OR_OPTION_UNCHANGED));
    const bool pass = g_responsive && g_browseAppeared && g_browseClosed && dialogClosed;
    printf("responsive=%d browseAppeared=%d browseClosed=%d dialogClosed=%d\n", g_responsive,
           g_browseAppeared, g_browseClosed, dialogClosed);
    printf("RESULT: %s\n", pass ? "BROWSE CHECK PASSED" : "BROWSE CHECK FAILED");
    return pass ? 0 : 5;
}
