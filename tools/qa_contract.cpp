// tools/qa_contract.cpp - independent QA contract + lifecycle test for TMPluginScreenshot.dll
//
// Written from scratch against include/PluginInterface.h; it does NOT reuse
// tools/smoketest.cpp.  Covers QA plan items P0-3 (interface contract) and
// P0-4 (lifecycle / thread safety) plus an explicit vtable-order check.
//
// Build: tools/qa_build.bat  ->  bin/qa_contract.exe
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "PluginInterface.h"

namespace
{

int g_pass = 0;
int g_fail = 0;

void Print(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

void Check(bool ok, const char* what)
{
    Print("  [%s] %s", ok ? "PASS" : "FAIL", what);
    if (ok)
    {
        ++g_pass;
    }
    else
    {
        ++g_fail;
    }
}

void CheckF(bool ok, const char* fmt, ...)
{
    char buffer[1400] = {0};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
    va_end(args);
    Check(ok, buffer);
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

std::string Narrow(const std::wstring& text)
{
    return Narrow(text.c_str());
}

bool NonEmpty(const wchar_t* text)
{
    return text != nullptr && text[0] != L'\0';
}

std::wstring ExeDirectory()
{
    wchar_t buffer[MAX_PATH * 2] = {0};
    DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(_countof(buffer)));
    std::wstring path(buffer, len);
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring(L".") : path.substr(0, pos);
}

std::wstring Join(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty())
    {
        return name;
    }
    std::wstring out(dir);
    if (out.back() != L'\\' && out.back() != L'/')
    {
        out += L'\\';
    }
    out += name;
    return out;
}

void EnsureDir(const std::wstring& dir)
{
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
}

void DeleteTree(const std::wstring& dir)
{
    std::wstring pattern = Join(dir, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return;
    }
    do
    {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
        {
            continue;
        }
        std::wstring path = Join(dir, data.cFileName);
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            DeleteTree(path);
            RemoveDirectoryW(path.c_str());
        }
        else
        {
            DeleteFileW(path.c_str());
        }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
}

size_t CountFiles(const std::wstring& dir)
{
    std::wstring pattern = Join(dir, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    size_t count = 0;
    do
    {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
        {
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            ++count;
        }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
    return count;
}

void PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool WaitForFiles(const std::wstring& dir, size_t want, DWORD timeoutMs, size_t* got = nullptr)
{
    const ULONGLONG start = GetTickCount64();
    size_t count = 0;
    for (;;)
    {
        PumpMessages();
        count = CountFiles(dir);
        if (count >= want)
        {
            if (got != nullptr)
            {
                *got = count;
            }
            return true;
        }
        if (GetTickCount64() - start >= timeoutMs)
        {
            break;
        }
        Sleep(20);
    }
    if (got != nullptr)
    {
        *got = count;
    }
    return false;
}

std::wstring ReadAllTextUtf16(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return std::wstring();
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::vector<char> raw(static_cast<size_t>(size.QuadPart) + 1, 0);
    DWORD read = 0;
    ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr);
    CloseHandle(file);
    raw.resize(read);
    size_t offset = 0;
    if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE)
    {
        offset = 2;
    }
    std::wstring text;
    text.resize((raw.size() - offset) / sizeof(wchar_t));
    if (!text.empty())
    {
        memcpy(&text[0], raw.data() + offset, text.size() * sizeof(wchar_t));
    }
    return text;
}

size_t RssBytes(){
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == FALSE)
    {
        return 0;
    }
    return static_cast<size_t>(counters.WorkingSetSize);
}

int ThreadCount()
{
    const DWORD pid = GetCurrentProcessId();
    int count = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return -1;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
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

int WaitForThreadsAtMost(int limit, DWORD timeoutMs)
{
    const ULONGLONG start = GetTickCount64();
    int count = ThreadCount();
    while (count > limit && GetTickCount64() - start < timeoutMs)
    {
        PumpMessages();
        Sleep(25);
        count = ThreadCount();
    }
    return count;
}

bool WriteUtf16Ini(const std::wstring& path, const std::vector<std::pair<std::wstring, std::wstring>>& kv)
{
    std::wstring text = L"[TMSnap]\r\n";
    for (size_t i = 0; i < kv.size(); ++i)
    {
        text += kv[i].first;
        text += L"=";
        text += kv[i].second;
        text += L"\r\n";
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    WriteFile(file, text.c_str(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
    return true;
}

// ---------------------------------------------------------------------------
// Mock host (implements the whole ITrafficMonitor vtable)
// ---------------------------------------------------------------------------
class CMockHost : public ITrafficMonitor
{
public:
    CMockHost(const std::wstring& configDir, HWND mainWindow)
        : m_configDir(configDir), m_mainWindow(mainWindow)
    {
    }

    int GetAPIVersion() override { return 1; }
    const wchar_t* GetVersion() override { return L"1.8.6-qa-mock"; }
    double GetMonitorValue(MonitorItem) override { return 0.0; }
    const wchar_t* GetMonitorValueString(MonitorItem, int = false) override { return L"0"; }
    void ShowNotifyMessage(const wchar_t* message) override
    {
        ++m_notifyCount;
        m_lastNotify = (message != nullptr) ? message : L"";
        Print("    host notification: %s", Narrow(m_lastNotify.c_str()).c_str());
    }
    unsigned short GetLanguageId() const override { return 2052; }
    const wchar_t* GetPluginConfigDir() const override { return m_configDir.c_str(); }
    int GetDPI(DPIType) const override { return 96; }
    unsigned int GetThemeColor() const override { return RGB(255, 255, 255); }
    const wchar_t* GetStringRes(const wchar_t* key, const wchar_t*) override { return (key != nullptr) ? key : L""; }
    void* GetMainWindowHwnd() override { return m_mainWindow; }
    void* GetTaskbarWindowHwnd() override { return nullptr; }

    int m_notifyCount{0};
    std::wstring m_lastNotify;

private:
    std::wstring m_configDir;
    HWND m_mainWindow;
};

// Independent re-declaration of ITMPlugin in the same slot order.  Calling
// through it proves each virtual we hit is the function we expect.
struct ITMPluginMirror
{
    virtual int GetAPIVersion() const = 0;
    virtual IPluginItem* GetItem(int index) = 0;
    virtual void DataRequired() = 0;
    virtual int ShowOptionsDialog(void* hParent) = 0;
    virtual const wchar_t* GetInfo(int index) = 0;
    virtual void OnMonitorInfo(const ITMPlugin::MonitorInfo& info) = 0;
    virtual const wchar_t* GetTooltipInfo() = 0;
    virtual void OnExtenedInfo(int index, const wchar_t* data) = 0;
    virtual void* GetPluginIcon() = 0;
    virtual int GetCommandCount() = 0;
    virtual const wchar_t* GetCommandName(int commandIndex) = 0;
    virtual void* GetCommandIcon(int commandIndex) = 0;
    virtual void OnPluginCommand(int commandIndex, void* hWnd, void* para) = 0;
    virtual int IsCommandChecked(int commandIndex) = 0;
    virtual void OnInitialize(ITrafficMonitor* pApp) = 0;
};

// Minimal IPluginDrawer that records the geometry the plugin asks for.
class CMockDrawer : public IPluginDrawer
{
public:
    int GetAPIVersion() override { return 8; }
    void DrawWindowText(int, int, int, int, const wchar_t*, unsigned long, Alignment, bool, unsigned char) override
    {
        ++textCalls;
    }
    void SetDrawRect(int x, int y, int w, int h) override { Record(x, y, w, h); }
    void FillRect(int x, int y, int w, int h, unsigned long, unsigned char) override { Record(x, y, w, h); }
    void DrawRectOutLine(int x, int y, int w, int h, unsigned long, int, bool, unsigned char, int) override
    {
        Record(x, y, w, h);
    }
    void DrawBitmap(void*, int x, int y, int w, int h, StretchMode, unsigned char) override { Record(x, y, w, h); }
    void DrawIcon(void*, int x, int y, int w, int h) override { Record(x, y, w, h); }
    void GetTextExtent(const wchar_t* text, int& w, int& h) override
    {
        w = (text != nullptr) ? static_cast<int>(wcslen(text)) * 7 : 0;
        h = 14;
    }
    void DrawLine(int x1, int y1, int x2, int y2, unsigned long, int, bool, unsigned char) override
    {
        ++lineCalls;
        UpdateBounds(x1, y1);
        UpdateBounds(x2, y2);
    }
    void* GetHDC() override { return nullptr; }

    int shapeCalls{0};
    int lineCalls{0};
    int textCalls{0};
    int minX{1000};
    int minY{1000};
    int maxX{-1000};
    int maxY{-1000};

private:
    void Record(int x, int y, int w, int h)
    {
        ++shapeCalls;
        UpdateBounds(x, y);
        UpdateBounds(x + w - 1, y + h - 1);
    }
    void UpdateBounds(int x, int y)
    {
        if (x < minX)
        {
            minX = x;
        }
        if (y < minY)
        {
            minY = y;
        }
        if (x > maxX)
        {
            maxX = x;
        }
        if (y > maxY)
        {
            maxY = y;
        }
    }
};

// Waits for a #32770 dialog owned by this process, parks it off-screen (so the
// human in front of the machine is not disturbed) and posts IDCANCEL to it.
struct DialogCloser
{
    HWND dialog{nullptr};

    void Run()
    {
        const DWORD pid = GetCurrentProcessId();
        const ULONGLONG start = GetTickCount64();
        while (GetTickCount64() - start < 10000 && dialog == nullptr)
        {
            struct Ctx
            {
                DWORD pid;
                HWND found;
            } ctx{pid, nullptr};
            EnumWindows(
                [](HWND hwnd, LPARAM param) -> BOOL {
                    Ctx* c = reinterpret_cast<Ctx*>(param);
                    DWORD windowPid = 0;
                    GetWindowThreadProcessId(hwnd, &windowPid);
                    if (windowPid != c->pid || IsWindowVisible(hwnd) == FALSE)
                    {
                        return TRUE;
                    }
                    wchar_t className[64] = {0};
                    GetClassNameW(hwnd, className, static_cast<int>(_countof(className)));
                    if (wcscmp(className, L"#32770") == 0)
                    {
                        c->found = hwnd;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&ctx));
            dialog = ctx.found;
            if (dialog == nullptr)
            {
                Sleep(20);
            }
        }
        if (dialog != nullptr)
        {
            // Keep the modal dialog out of the user's way while we drive it.
            SetWindowPos(dialog, nullptr, -30000, -30000, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    void Cancel() const
    {
        if (dialog != nullptr)
        {
            PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        }
    }
};

// Runs qa_runtime.exe "hotkeyprobe" in a separate process.  A hotkey belongs to
// the registering thread, so only a *different process* proves whether the
// system-wide hotkey is really free.
int RunHotkeyProbe(const std::wstring& runtimeExe)
{
    std::wstring commandLine = L"\"" + runtimeExe + L"\" hotkeyprobe";
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &process) == FALSE)
    {
        return -1;
    }
    WaitForSingleObject(process.hProcess, 20000);
    DWORD code = 99;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}

} // namespace

int main()
{
    wchar_t selfPath[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, selfPath, static_cast<DWORD>(_countof(selfPath)));
    const std::wstring exeDir = ExeDirectory();
    const std::wstring dllPath = Join(exeDir, L"TMPluginScreenshot.dll");
    const std::wstring runtimeExe = Join(exeDir, L"qa_runtime.exe");
    const std::wstring outRoot = Join(Join(exeDir, L".."), L"out-test\\qa\\contract");
    const std::wstring shots = Join(outRoot, L"shots");
    const std::wstring configDir = Join(outRoot, L"config");

    Print("== qa_contract: %s", Narrow(selfPath).c_str());
    Print("   dll   : %s", Narrow(dllPath).c_str());
    Print("   shots : %s", Narrow(shots).c_str());

    DeleteTree(outRoot);
    EnsureDir(shots);
    EnsureDir(configDir);

    // =================================================================
    Print("\n--- PHASE A: contract, never initialised ---");
    HMODULE module = LoadLibraryW(dllPath.c_str());
    CheckF(module != nullptr, "LoadLibrary(%s) (err=%lu)", Narrow(dllPath).c_str(),
           module ? 0UL : static_cast<unsigned long>(GetLastError()));
    if (module == nullptr)
    {
        Print("RESULT: %d passed, %d failed", g_pass, g_fail);
        return 1;
    }

    auto getInstance = reinterpret_cast<ITMPlugin* (*)()>(GetProcAddress(module, "TMPluginGetInstance"));
    Check(getInstance != nullptr, "GetProcAddress(TMPluginGetInstance)");
    if (getInstance == nullptr)
    {
        FreeLibrary(module);
        return 1;
    }
    ITMPlugin* plugin = getInstance();
    Check(plugin != nullptr, "TMPluginGetInstance() non-null");
    if (plugin == nullptr)
    {
        FreeLibrary(module);
        return 1;
    }
    Check(getInstance() == plugin, "TMPluginGetInstance() is a stable singleton");

    // --- vtable introspection -----------------------------------------
    {
        void** vtable = *reinterpret_cast<void***>(plugin);
        Print("   ITMPlugin vtable @%p:", static_cast<void*>(vtable));
        for (int i = 0; i < 15; ++i)
        {
            Print("     slot[%2d] = %p", i, vtable[i]);
        }
        Check(vtable[0] != nullptr && vtable[1] != nullptr && vtable[4] != nullptr,
              "vtable slots 0 (GetAPIVersion) / 1 (GetItem) / 4 (GetInfo) are populated");
    }

    ITMPluginMirror* mirror = reinterpret_cast<ITMPluginMirror*>(plugin);
    const int apiDirect = plugin->GetAPIVersion();
    CheckF(apiDirect == 8, "GetAPIVersion() == 8 (got %d)", apiDirect);
    CheckF(mirror->GetAPIVersion() == apiDirect, "mirror slot 0 -> GetAPIVersion agrees (%d)",
           mirror->GetAPIVersion());

    IPluginItem* item = plugin->GetItem(0);
    Check(item != nullptr, "GetItem(0) != nullptr");
    Check(item == mirror->GetItem(0), "mirror slot 1 -> GetItem(0) returns the same object");

    const wchar_t* nameDirect = plugin->GetInfo(ITMPlugin::TMI_NAME);
    const wchar_t* nameMirror = mirror->GetInfo(ITMPlugin::TMI_NAME);
    Check(nameDirect != nullptr && nameMirror != nullptr && wcscmp(nameDirect, nameMirror) == 0,
          "mirror slot 4 -> GetInfo(TMI_NAME) returns the same string");
    Check(mirror->GetCommandCount() == plugin->GetCommandCount(),
          "mirror slot 9 -> GetCommandCount() agrees");

    // --- GetInfo -------------------------------------------------------
    for (int i = ITMPlugin::TMI_NAME; i < ITMPlugin::TMI_MAX; ++i)
    {
        const wchar_t* value = plugin->GetInfo(static_cast<ITMPlugin::PluginInfoIndex>(i));
        CheckF(NonEmpty(value), "GetInfo(%d) non-empty: \"%s\"", i, Narrow(value).c_str());
    }
    const wchar_t* info99 = plugin->GetInfo(static_cast<ITMPlugin::PluginInfoIndex>(99));
    CheckF(info99 != nullptr, "GetInfo(99) non-null (hard requirement: never nullptr)");
    const wchar_t* infoNeg = plugin->GetInfo(static_cast<ITMPlugin::PluginInfoIndex>(-1));
    CheckF(infoNeg != nullptr, "GetInfo(-1) non-null (hard requirement: never nullptr)");

    // --- GetItem bounds ------------------------------------------------
    Check(plugin->GetItem(-1) == nullptr, "GetItem(-1) == nullptr");
    Check(plugin->GetItem(1) == nullptr, "GetItem(1) == nullptr");
    Check(plugin->GetItem(0x7fffffff) == nullptr, "GetItem(INT_MAX) == nullptr");

    // --- item contract --------------------------------------------------
    if (item != nullptr)
    {
        const wchar_t* itemName = item->GetItemName();
        const wchar_t* itemId = item->GetItemId();
        const wchar_t* label = item->GetItemLableText();
        const wchar_t* value = item->GetItemValueText();
        const wchar_t* sample = item->GetItemValueSampleText();
        Print("   item: name=\"%s\" id=\"%s\" label=\"%s\" value=\"%s\" sample=\"%s\"",
              Narrow(itemName).c_str(), Narrow(itemId).c_str(), Narrow(label).c_str(), Narrow(value).c_str(),
              Narrow(sample).c_str());
        Check(NonEmpty(itemName), "GetItemName() non-empty");
        CheckF(itemId != nullptr && wcscmp(itemId, L"TMSnap_Capture_Item") == 0,
              "GetItemId() == TMSnap_Capture_Item");
        Check(itemName != nullptr && itemId != nullptr && wcscmp(itemName, itemId) != 0,
              "GetItemName() != GetItemId() (slots 0/1 not swapped)");
        Check(label != nullptr, "GetItemLableText() non-null");
        Check(value != nullptr, "GetItemValueText() non-null");
        Check(NonEmpty(sample), "GetItemValueSampleText() non-empty");
        Check(item->IsCustomDraw() == true, "IsCustomDraw() == true");
        CheckF(item->GetItemWidth() == 24, "GetItemWidth() == 24 (got %d)", item->GetItemWidth());
        const int widthEx = item->GetItemWidthEx(nullptr);
        CheckF(widthEx == 24, "GetItemWidthEx(nullptr) == 24 and did not crash (got %d)", widthEx);
        CheckF(item->IsDoubleLineExclusive() == 0, "IsDoubleLineExclusive() == 0");

        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 24, 24);
        HGDIOBJ old = SelectObject(mem, bitmap);
        item->DrawItem(mem, 0, 0, 24, 24, false);
        item->DrawItem(mem, 0, 0, 24, 24, true);
        item->DrawItem(nullptr, 0, 0, 24, 24, false); // guard path
        SelectObject(mem, old);
        DeleteObject(bitmap);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        Check(true, "DrawItem() light/dark/null-HDC did not crash");

        CMockDrawer drawer;
        Check(item->DrawItemEx(&drawer, 0, 0, 24, 24, false) == true, "DrawItemEx(mock drawer) == true");
        CheckF(drawer.shapeCalls + drawer.lineCalls > 0, "DrawItemEx drew %d shapes + %d lines",
               drawer.shapeCalls, drawer.lineCalls);
        CheckF(drawer.minX >= 0 && drawer.minY >= 0 && drawer.maxX <= 23 && drawer.maxY <= 23,
              "DrawItemEx geometry inside 24x24 (x %d..%d, y %d..%d)", drawer.minX, drawer.maxX, drawer.minY,
              drawer.maxY);
        Check(item->DrawItemEx(nullptr, 0, 0, 24, 24, false) == false, "DrawItemEx(nullptr) == false, no crash");
    }

    // --- commands --------------------------------------------------------
    CheckF(plugin->GetCommandCount() == 6, "GetCommandCount() == 6 (got %d)", plugin->GetCommandCount());
    const wchar_t* expected[6] = {L"全屏截图", L"区域截图", L"活动窗口截图", L"延时截图", L"打开保存目录",
                                  L"设置…"};
    for (int i = 0; i < 6; ++i)
    {
        const wchar_t* got = plugin->GetCommandName(i);
        CheckF(got != nullptr && wcscmp(got, expected[i]) == 0, "GetCommandName(%d) == \"%s\" (got \"%s\")", i,
               Narrow(expected[i]).c_str(), Narrow(got).c_str());
    }
    Check(plugin->GetCommandName(6) != nullptr, "GetCommandName(6) did not crash");
    Check(plugin->GetCommandName(-1) != nullptr, "GetCommandName(-1) did not crash");
    Check(plugin->GetCommandName(99) != nullptr, "GetCommandName(99) did not crash");
    Check(plugin->GetCommandIcon(0) != nullptr, "GetCommandIcon(0) returns an HICON");
    Check(plugin->GetCommandIcon(-1) == nullptr, "GetCommandIcon(-1) == nullptr");

    // --- tooltip / mouse / keyboard --------------------------------------
    const wchar_t* tip = plugin->GetTooltipInfo();
    CheckF(NonEmpty(tip), "GetTooltipInfo() non-empty before OnInitialize: \"%s\"", Narrow(tip).c_str());
    Check(item->OnMouseEvent(IPluginItem::MT_RCLICKED, 0, 0, nullptr, 0) == 0,
          "OnMouseEvent(MT_RCLICKED) == 0 (host shows its menu)");
    Check(item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0) != 0,
          "OnMouseEvent(MT_LCLICKED) != 0 (plugin takes the click)");
    Check(item->OnMouseEvent(IPluginItem::MT_WHEEL_UP, 0, 0, nullptr, 0) == 0,
          "OnMouseEvent(MT_WHEEL_UP) == 0");
    Check(item->OnKeboardEvent(0, false, false, false, nullptr, 0) == 0, "OnKeboardEvent() == 0");
    Check(plugin->GetPluginIcon() != nullptr, "GetPluginIcon() returns an HICON");

    // --- DataRequired x1000 + RSS ----------------------------------------
    {
        const size_t before = RssBytes();
        for (int i = 0; i < 1000; ++i)
        {
            plugin->DataRequired();
        }
        const size_t after = RssBytes();
        const long long delta = static_cast<long long>(after) - static_cast<long long>(before);
        Print("   RSS before=%zu after=%zu delta=%lld bytes", before, after, delta);
        Check(true, "DataRequired() x1000 did not crash");
        CheckF(delta < 4 * 1024 * 1024, "DataRequired() x1000 RSS growth < 4MB (%lld bytes)", delta);
    }

    FreeLibrary(module);
    Print("   (FreeLibrary after phase A)");

    // =================================================================
    Print("\n--- PHASE B: initialised with a mock host ---");
    module = LoadLibraryW(dllPath.c_str());
    Check(module != nullptr, "reload the DLL for phase B");
    if (module == nullptr)
    {
        Print("RESULT: %d passed, %d failed", g_pass, g_fail);
        return 1;
    }
    getInstance = reinterpret_cast<ITMPlugin* (*)()>(GetProcAddress(module, "TMPluginGetInstance"));
    plugin = getInstance();
    item = plugin->GetItem(0);

    WriteUtf16Ini(Join(configDir, L"TMPluginScreenshot.ini"),
                  {{L"save_dir", shots},
                   {L"format", L"png"},
                   {L"jpg_quality", L"90"},
                   {L"file_name_template", L"TMSnap_yyyyMMdd_HHmmss"},
                   {L"copy_to_clipboard", L"1"},
                   {L"include_cursor", L"0"},
                   {L"hotkey_enabled", L"1"},
                   {L"hotkey_mods", L"ctrl+alt"},
                   {L"hotkey_vk", L"65"},
                   {L"delay_seconds", L"0"},
                   {L"fallback_pictures_root", L"1"},
                   {L"last_saved", L""}});

    CMockHost host(configDir, nullptr);
    plugin->OnInitialize(&host);
    Check(true, "OnInitialize(mock host) did not crash");
    CheckF(NonEmpty(plugin->GetTooltipInfo()), "GetTooltipInfo() non-empty after OnInitialize: \"%s\"",
           Narrow(plugin->GetTooltipInfo()).c_str());
    CheckF(NonEmpty(plugin->GetInfo(ITMPlugin::TMI_NAME)), "GetInfo(TMI_NAME) non-empty after OnInitialize");

    Sleep(400); // clear the 300ms debounce window

    size_t baseline = CountFiles(shots);
    Print("   baseline files in shots: %zu", baseline);

    // --- DBCLICKED + LCLICKED + RCLICKED => exactly one file -------------
    item->OnMouseEvent(IPluginItem::MT_DBCLICKED, 0, 0, nullptr, 0);
    const int leftResult = item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
    const int rightResult = item->OnMouseEvent(IPluginItem::MT_RCLICKED, 0, 0, nullptr, 0);
    CheckF(leftResult != 0 && rightResult == 0, "LCLICKED != 0 / RCLICKED == 0 when initialised");
    size_t got = 0;
    const bool arrived = WaitForFiles(shots, baseline + 1, 6000, &got);
    CheckF(arrived, "click produced a file (%zu -> %zu)", baseline, got);
    Sleep(1500); // a runaway second session would show up here
    PumpMessages();
    got = CountFiles(shots);
    CheckF(got == baseline + 1, "DBCLICKED+LCLICKED+RCLICKED produced exactly 1 file (got %d)",
           static_cast<int>(got - baseline));

    // --- options dialog x3, each closed with IDCANCEL --------------------
    for (int round = 1; round <= 3; ++round)
    {
        DialogCloser closer;
        std::thread closerThread([&closer]() {
            closer.Run();
            closer.Cancel();
        });
        const ULONGLONG start = GetTickCount64();
        const ITMPlugin::OptionReturn ret = plugin->ShowOptionsDialog(nullptr);
        const ULONGLONG elapsed = GetTickCount64() - start;
        closerThread.join();
        const char* name = (ret == ITMPlugin::OR_OPTION_CHANGED)
                               ? "OR_OPTION_CHANGED"
                               : ((ret == ITMPlugin::OR_OPTION_UNCHANGED) ? "OR_OPTION_UNCHANGED"
                                                                          : "OR_OPTION_NOT_PROVIDED");
        CheckF(ret != ITMPlugin::OR_OPTION_NOT_PROVIDED, "ShowOptionsDialog() round %d -> %s in %llums", round,
               name, static_cast<unsigned long long>(elapsed));
        CheckF(closer.dialog != nullptr, "round %d: the modal dialog really appeared", round);
        CheckF(elapsed < 5000, "round %d: the dialog closed promptly on IDCANCEL (%llums < 5000)",
               round, static_cast<unsigned long long>(elapsed));
    }

    // --- capture must still work after the dialogs -----------------------
    {
        const std::wstring ini = ReadAllTextUtf16(Join(configDir, L"TMPluginScreenshot.ini"));
        CheckF(ini.find(shots) != std::wstring::npos,
              "3x IDCANCEL did not rewrite the ini (save_dir is still the test directory)");
        CheckF(ini.find(L"ctrl+alt") != std::wstring::npos, "3x IDCANCEL kept the original hotkey in the ini");
    }
    Sleep(400);
    baseline = CountFiles(shots);
    item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
    const bool afterDialogFile = WaitForFiles(shots, baseline + 1, 6000, &got);
    CheckF(afterDialogFile, "capture still works after 3 dialogs (m_optionsOpen reset) %zu -> %zu", baseline, got);

    // --- click while the dialog is open ---------------------------------
    {
        Sleep(400);
        const size_t beforeOpen = CountFiles(shots);
        DialogCloser closer;
        std::thread closerThread([&closer, &item]() {
            closer.Run();
            if (closer.dialog != nullptr)
            {
                const int r = item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
                Print("    click while the dialog is open returned %d", r);
                Sleep(800); // plenty of time for a wrong implementation to write a file
                closer.Cancel();
            }
        });
        const ITMPlugin::OptionReturn ret = plugin->ShowOptionsDialog(nullptr);
        closerThread.join();
        CheckF(ret != ITMPlugin::OR_OPTION_NOT_PROVIDED, "dialog 4 (click-inside test) returned a valid code");
        PumpMessages();
        Sleep(400);
        PumpMessages();
        const size_t afterOpen = CountFiles(shots);
        CheckF(afterOpen == beforeOpen, "click while the dialog was open created no file (%zu -> %zu)",
               beforeOpen, afterOpen);
    }

    plugin->OnPluginCommand(4, nullptr, nullptr); // open save folder
    plugin->OnPluginCommand(-1, nullptr, nullptr);
    plugin->OnPluginCommand(99, nullptr, nullptr);
    Check(true, "OnPluginCommand(4/-1/99) did not crash");
    Print("   host notifications seen: %d", host.m_notifyCount);

    FreeLibrary(module);
    Print("   (FreeLibrary after phase B)");

    // =================================================================
    Print("\n--- PHASE C: load/release without OnInitialize ---");
    for (int i = 1; i <= 2; ++i)
    {
        HMODULE m = LoadLibraryW(dllPath.c_str());
        CheckF(m != nullptr, "round %d: LoadLibrary", i);
        if (m != nullptr)
        {
            auto gi = reinterpret_cast<ITMPlugin* (*)()>(GetProcAddress(m, "TMPluginGetInstance"));
            ITMPlugin* p = (gi != nullptr) ? gi() : nullptr;
            if (p != nullptr)
            {
                p->GetAPIVersion();
                p->GetItem(0);
                p->GetTooltipInfo();
            }
            FreeLibrary(m);
            CheckF(true, "round %d: load -> use -> FreeLibrary without OnInitialize survived", i);
        }
    }

    {
        HMODULE m = LoadLibraryW(dllPath.c_str());
        auto gi = reinterpret_cast<ITMPlugin* (*)()>(GetProcAddress(m, "TMPluginGetInstance"));
        ITMPlugin* p = (gi != nullptr) ? gi() : nullptr;
        if (p != nullptr)
        {
            p->OnInitialize(nullptr);
            p->GetTooltipInfo();
            p->GetItem(0)->OnMouseEvent(IPluginItem::MT_RCLICKED, 0, 0, nullptr, 0);
        }
        FreeLibrary(m);
        Check(true, "OnInitialize(nullptr) + FreeLibrary did not crash");
    }

    // =================================================================
    Print("\n--- PHASE D: 5 x load/init/use/release ---");
    const int baseThreads = ThreadCount();
    Print("   baseline thread count (before any load): %d", baseThreads);
    int firstRoundAfter = -1;
    for (int round = 1; round <= 5; ++round)
    {
        HMODULE m = LoadLibraryW(dllPath.c_str());
        CheckF(m != nullptr, "round %d: LoadLibrary", round);
        if (m == nullptr)
        {
            break;
        }
        auto gi = reinterpret_cast<ITMPlugin* (*)()>(GetProcAddress(m, "TMPluginGetInstance"));
        ITMPlugin* p = (gi != nullptr) ? gi() : nullptr;
        CheckF(p != nullptr, "round %d: TMPluginGetInstance", round);

        CMockHost roundHost(configDir, nullptr);
        p->OnInitialize(&roundHost);
        const int loadedThreads = WaitForThreadsAtMost(baseThreads + 2, 2000);
        if (round == 1)
        {
            const int probe = RunHotkeyProbe(runtimeExe);
            Print("   round 1: threads while loaded=%d (%d above baseline); a *separate process* could "
                  "register Ctrl+Alt+A: %s (exit=%d, expected: no)",
                  loadedThreads, loadedThreads - baseThreads, probe == 0 ? "yes" : (probe == 1 ? "no" : "error"),
                  probe);
            CheckF(probe == 1, "while loaded the plugin really owns Ctrl+Alt+A system-wide (foreign process is "
                              "refused, exit=%d)",
                   probe);
        }
        p->DataRequired();
        p->GetItem(0)->OnMouseEvent(IPluginItem::MT_RCLICKED, 0, 0, nullptr, 0);
        p->GetTooltipInfo();

        const int beforeFree = ThreadCount();
        FreeLibrary(m);
        const int settled = WaitForThreadsAtMost(baseThreads, 3000);
        Print("   round %d: threads before FreeLibrary=%d, after=%d (baseline %d)", round, beforeFree, settled,
              baseThreads);
        if (round == 1)
        {
            firstRoundAfter = settled;
            CheckF(settled <= baseThreads,
                  "round 1: capture+hotkey threads exit during FreeLibrary (%d -> %d, baseline %d)", beforeFree,
                  settled, baseThreads);
        }
        else
        {
            CheckF(settled <= baseThreads, "round %d: no thread leak after FreeLibrary (%d <= baseline %d)", round,
                  settled, baseThreads);
        }
    }

    {
        const int probe = RunHotkeyProbe(runtimeExe);
        Print("   after 5x FreeLibrary a separate process registering Ctrl+Alt+A: %s (exit=%d)",
              probe == 0 ? "succeeded" : (probe == 1 ? "refused" : "error"), probe);
        CheckF(probe == 0,
              "after 5x FreeLibrary another process can register Ctrl+Alt+A => hotkey released (exit=%d)", probe);
    }

    Print("\n================ qa_contract summary ================");
    Print("PASS: %d   FAIL: %d", g_pass, g_fail);
    Print("threads: baseline=%d round1_after_free=%d", baseThreads, firstRoundAfter);
    Print("====================================================");
    return (g_fail == 0) ? 0 : 1;
}
