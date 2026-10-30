// tools/fix_unload.cpp - DEF-02 regression harness (not part of the shipped DLL).
//
// 20 x ( LoadLibrary -> OnInitialize(mock) -> one full screen capture ->
//        FreeLibrary ) in a single process.  After every round it verifies:
//   * the capture really produced one file,
//   * the module is really unloaded again,
//   * the process thread count is back to the baseline (no leaked threads).
// And at the very end:
//   * Ctrl+Alt+A can be registered by this process (our hotkey was released).
//
// Exit code 0 = everything green.
#include <windows.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "PluginInterface.h"

namespace
{

int g_failures = 0;
const int kRounds = 20;
const wchar_t* kDllName = L"TMPluginScreenshot.dll";

void Print(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

bool Check(bool condition, const char* what)
{
    Print("  [%s] %s", condition ? "PASS" : "FAIL", what);
    if (!condition)
    {
        ++g_failures;
    }
    return condition;
}

std::string Narrow(const wchar_t* text)
{
    if (text == nullptr)
    {
        return std::string("<null>");
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
    {
        return std::string("<empty>");
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], needed, nullptr, nullptr);
    return out;
}

std::wstring ExeDirectory()
{
    wchar_t buffer[MAX_PATH * 2] = {0};
    DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(_countof(buffer)));
    std::wstring path(buffer, len);
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring(L".") : path.substr(0, pos);
}

bool WriteUtf16File(const std::wstring& path, const std::wstring& content)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    WriteFile(file, content.c_str(), static_cast<DWORD>(content.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
    return true;
}

int CountFiles(const std::wstring& directory)
{
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = directory + L"\\*.png";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    int count = 0;
    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            ++count;
        }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
    return count;
}

void DeletePngs(const std::wstring& directory)
{
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = directory + L"\\*.png";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return;
    }
    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            const std::wstring full = directory + L"\\" + data.cFileName;
            DeleteFileW(full.c_str());
        }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
}

int ThreadCount()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return -1;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    int count = 0;
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snapshot, &entry) != FALSE)
    {
        do
        {
            if (entry.th32OwnerProcessID == pid)
            {
                ++count;
            }
        } while (Thread32Next(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    return count;
}

void PumpMessages(int milliseconds)
{
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
    while (GetTickCount64() < deadline)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

class CMockHost : public ITrafficMonitor
{
public:
    explicit CMockHost(const std::wstring& configDir) : m_configDir(configDir) {}
    volatile LONG notifications{0};

    int GetAPIVersion() override { return 1; }
    const wchar_t* GetVersion() override { return L"fix_unload"; }
    double GetMonitorValue(MonitorItem item) override
    {
        (void)item;
        return 0.0;
    }
    const wchar_t* GetMonitorValueString(MonitorItem item, int is_main_window = false) override
    {
        (void)item;
        (void)is_main_window;
        return L"0";
    }
    void ShowNotifyMessage(const wchar_t* strMsg) override
    {
        InterlockedIncrement(&notifications);
        Print("      [notify] %s", Narrow(strMsg).c_str());
    }
    unsigned short GetLanguageId() const override { return 2052; }
    const wchar_t* GetPluginConfigDir() const override { return m_configDir.c_str(); }
    int GetDPI(DPIType type) const override
    {
        (void)type;
        return 96;
    }
    unsigned int GetThemeColor() const override { return 0xFFFFFF; }
    const wchar_t* GetStringRes(const wchar_t* key, const wchar_t* section) override
    {
        (void)key;
        (void)section;
        return L"";
    }
    void* GetMainWindowHwnd() override { return nullptr; }
    void* GetTaskbarWindowHwnd() override { return nullptr; }

private:
    std::wstring m_configDir;
};

} // namespace

int main()
{
    Print("=== DEF-02 unload harness: %d x (load -> init -> capture -> FreeLibrary) ===", kRounds);

    const std::wstring exeDir = ExeDirectory();
    const std::wstring rootDir = exeDir + L"\\..";
    const std::wstring workDir = rootDir + L"\\out-test\\fix-unload";
    const std::wstring configDir = workDir + L"\\config";
    const std::wstring shotsDir = workDir + L"\\shots";
    const std::wstring dllPath = exeDir + L"\\" + kDllName;

    CreateDirectoryW(workDir.c_str(), nullptr);
    CreateDirectoryW(configDir.c_str(), nullptr);
    CreateDirectoryW(shotsDir.c_str(), nullptr);

    std::wstring ini;
    ini += L"[TMSnap]\r\n";
    ini += L"save_dir=" + shotsDir + L"\r\n";
    ini += L"format=png\r\n";
    ini += L"jpg_quality=90\r\n";
    ini += L"file_name_template=unload_yyyyMMdd_HHmmss\r\n";
    ini += L"copy_to_clipboard=0\r\n";
    ini += L"include_cursor=0\r\n";
    ini += L"hotkey_enabled=1\r\n";
    ini += L"hotkey_mods=ctrl+alt\r\n";
    ini += L"hotkey_vk=65\r\n";
    ini += L"delay_seconds=0\r\n";
    ini += L"fallback_pictures_root=0\r\n";
    if (!Check(WriteUtf16File(configDir + L"\\TMPluginScreenshot.ini", ini), "wrote the test ini"))
    {
        return 1;
    }

    CMockHost host(configDir);
    const int baselineThreads = ThreadCount();
    Print("  baseline thread count = %d", baselineThreads);

    int totalFiles = 0;
    for (int round = 1; round <= kRounds; ++round)
    {
        HMODULE module = LoadLibraryW(dllPath.c_str());
        if (module == nullptr)
        {
            Print("  round %d: LoadLibraryW failed (error %lu)", round,
                  static_cast<unsigned long>(GetLastError()));
            return 1;
        }
        typedef ITMPlugin* (*GetInstanceFn)();
        GetInstanceFn getInstance =
            reinterpret_cast<GetInstanceFn>(GetProcAddress(module, "TMPluginGetInstance"));
        if (getInstance == nullptr)
        {
            Print("  round %d: GetProcAddress failed", round);
            return 1;
        }
        ITMPlugin* plugin = getInstance();
        if (plugin == nullptr)
        {
            Print("  round %d: TMPluginGetInstance returned nullptr", round);
            return 1;
        }

        plugin->OnInitialize(&host);
        PumpMessages(120);

        IPluginItem* item = plugin->GetItem(0);
        const int before = CountFiles(shotsDir);
        const LONG notificationsBefore = host.notifications;
        if (item != nullptr)
        {
            item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
        }
        else
        {
            plugin->OnPluginCommand(0, nullptr, nullptr);
        }

        // Wait until the job has really finished: the sink window receives the
        // result (that is what the notification counts), NOT merely "a .png file
        // exists" - WIC creates the file when the stream is opened, long before
        // the encode is done.  Unloading while the job thread is still encoding
        // is the documented residual race and would of course crash.
        const ULONGLONG deadline = GetTickCount64() + 8000;
        while (GetTickCount64() < deadline && host.notifications == notificationsBefore)
        {
            PumpMessages(20);
        }
        // Let the job thread return all the way out before we pull the DLL down.
        PumpMessages(80);
        Sleep(60);
        const int after = CountFiles(shotsDir);
        totalFiles += (after - before);

        const ULONGLONG unloadStart = GetTickCount64();
        const BOOL freed = FreeLibrary(module);
        const ULONGLONG unloadMs = GetTickCount64() - unloadStart;

        const bool unloaded = (GetModuleHandleW(kDllName) == nullptr);

        // Give the (already finished) job thread a moment to be reaped.
        int threads = ThreadCount();
        const ULONGLONG threadDeadline = GetTickCount64() + 1500;
        while (threads > baselineThreads && GetTickCount64() < threadDeadline)
        {
            Sleep(20);
            threads = ThreadCount();
        }

        const bool captured = (after == before + 1);
        Print("  round %2d: files %d->%d, FreeLibrary %llums, unloaded=%s, threads=%d", round, before, after,
              static_cast<unsigned long long>(unloadMs), unloaded ? "yes" : "no", threads);
        if (!freed || !captured || !unloaded || threads > baselineThreads)
        {
            Print("  round %d FAILED (freed=%d captured=%d unloaded=%d threads=%d/%d)", round,
                  freed ? 1 : 0, captured ? 1 : 0, unloaded ? 1 : 0, threads, baselineThreads);
            ++g_failures;
            break;
        }
    }

    Print("");
    Check(totalFiles == kRounds, "every round produced exactly one screenshot");
    Check(GetModuleHandleW(kDllName) == nullptr, "the module is not loaded any more");
    {
        const int threads = ThreadCount();
        Print("  final thread count = %d (baseline %d)", threads, baselineThreads);
        Check(threads <= baselineThreads, "no thread left behind");
    }

    // The plugin registered Ctrl+Alt+A; after unload this process must be able to take it.
    const BOOL hotkeyOk = RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'A');
    Print("  RegisterHotKey(Ctrl+Alt+A) after unload = %s (error %lu)", hotkeyOk ? "OK" : "REFUSED",
          static_cast<unsigned long>(GetLastError()));
    Check(hotkeyOk != FALSE, "Ctrl+Alt+A is free again after unload");
    if (hotkeyOk != FALSE)
    {
        UnregisterHotKey(nullptr, 1);
    }

    Print("");
    if (g_failures == 0)
    {
        Print("RESULT: DEF-02 REGRESSION CLEAN (%d/%d rounds)", kRounds, kRounds);
        return 0;
    }
    Print("RESULT: %d CHECK(S) FAILED", g_failures);
    return 2;
}
