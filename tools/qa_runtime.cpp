// tools/qa_runtime.cpp - independent QA runtime/behaviour tests (mock host).
//
// Scenarios (argv[1]) cover QA plan items P0-5 (settings & paths), P0-6
// (three real capture paths, formats, delay timer) and P1-2 (robustness stress).
// Every scenario runs in its own process, so the DLL (and its singleton) starts
// fresh and a specific ini file can be prepared before OnInitialize().
//
// Build: tools/qa_build.bat  ->  bin/qa_runtime.exe
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
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

void* g_dllBase = nullptr;
size_t g_dllSize = 0;


LONG CALLBACK VehLogger(EXCEPTION_POINTERS* info)
{
    static LONG once = 0;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code < 0xC0000000u) // skip debug prints / C++ exceptions
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedExchange(&once, 1) == 0)
    {
        const unsigned char* address = reinterpret_cast<const unsigned char*>(info->ExceptionRecord->ExceptionAddress);
        const unsigned char* base = reinterpret_cast<const unsigned char*>(g_dllBase);
        const bool inside = (base != nullptr) && address >= base && address < base + g_dllSize;
        printf("\n    [VEH] code=0x%08X addr=%p tid=%lu inside_plugin_image=%s (dll base=%p size=0x%zX)\n",
               static_cast<unsigned>(info->ExceptionRecord->ExceptionCode), static_cast<const void*>(address),
               static_cast<unsigned long>(GetCurrentThreadId()), inside ? "YES" : "no", g_dllBase, g_dllSize);
        fflush(stdout);
    }
    return EXCEPTION_CONTINUE_SEARCH;
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

int WaitThreads(int limit, DWORD timeoutMs)
{
    const ULONGLONG start = GetTickCount64();
    int count = ThreadCount();
    while (count > limit && GetTickCount64() - start < timeoutMs)
    {
        Sleep(25);
        count = ThreadCount();
    }
    return count;
}

bool PluginStillLoaded()
{
    HMODULE modules[512];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed) == FALSE)
    {
        return false;
    }
    const size_t count = needed / sizeof(HMODULE);
    for (size_t i = 0; i < count; ++i)
    {
        wchar_t name[MAX_PATH] = {0};
        if (GetModuleFileNameExW(GetCurrentProcess(), modules[i], name, MAX_PATH) > 0)
        {
            if (wcsstr(name, L"TMPluginScreenshot") != nullptr)
            {
                return true;
            }
        }
    }
    return false;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
{
    const unsigned char* address = reinterpret_cast<const unsigned char*>(info->ExceptionRecord->ExceptionAddress);
    const unsigned char* base = reinterpret_cast<const unsigned char*>(g_dllBase);
    const bool inside = (base != nullptr) && address >= base && address < base + g_dllSize;
    printf("\n    [CRASH] code=0x%08X addr=%p tid=%lu inside_plugin_image=%s\n",
           static_cast<unsigned>(info->ExceptionRecord->ExceptionCode), static_cast<const void*>(address),
           static_cast<unsigned long>(GetCurrentThreadId()), inside ? "YES" : "no");
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

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

std::wstring Widen(const std::string& text)
{
    if (text.empty())
    {
        return std::wstring();
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(needed > 0 ? needed - 1 : 0), L'\0');
    if (needed > 1)
    {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], needed);
    }
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

struct FileInfo
{
    std::wstring name;
    unsigned long long size{0};
    unsigned long long writeTime{0};
};

std::vector<FileInfo> ListFiles(const std::wstring& dir)
{
    std::vector<FileInfo> files;
    std::wstring pattern = Join(dir, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return files;
    }
    do
    {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
        {
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            continue;
        }
        FileInfo info;
        info.name = data.cFileName;
        info.size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        info.writeTime = (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                         data.ftLastWriteTime.dwLowDateTime;
        files.push_back(info);
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
    return files;
}

size_t CountFiles(const std::wstring& dir)
{
    return ListFiles(dir).size();
}

bool IsImageName(const std::wstring& name)
{
    std::wstring lower = name;
    for (size_t i = 0; i < lower.size(); ++i)
    {
        if (lower[i] >= L'A' && lower[i] <= L'Z')
        {
            lower[i] = static_cast<wchar_t>(lower[i] - L'A' + L'a');
        }
    }
    if (lower.size() > 5 && lower.substr(lower.size() - 5) == L".jpeg")
    {
        return true;
    }
    if (lower.size() > 4)
    {
        const std::wstring ext = lower.substr(lower.size() - 4);
        return ext == L".png" || ext == L".jpg" || ext == L".bmp";
    }
    return false;
}

// Only real image outputs: IsDirectoryWritable() creates a transient
// ".tmsnap_probe_*.tmp" file, so a raw file count would race with it.
std::vector<FileInfo> ListImages(const std::wstring& dir)
{
    std::vector<FileInfo> images;
    for (const FileInfo& info : ListFiles(dir))
    {
        if (IsImageName(info.name))
        {
            images.push_back(info);
        }
    }
    return images;
}

size_t CountImages(const std::wstring& dir)
{
    return ListImages(dir).size();
}

std::vector<unsigned char> ReadHead(const std::wstring& path, size_t bytes)
{
    std::vector<unsigned char> buffer(bytes, 0);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return std::vector<unsigned char>();
    }
    DWORD read = 0;
    ReadFile(file, buffer.data(), static_cast<DWORD>(bytes), &read, nullptr);
    CloseHandle(file);
    buffer.resize(read);
    return buffer;
}

bool PngSize(const std::wstring& path, int& width, int& height)
{
    std::vector<unsigned char> head = ReadHead(path, 33);
    if (head.size() < 33)
    {
        return false;
    }
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(head.data(), sig, 8) != 0 || memcmp(head.data() + 12, "IHDR", 4) != 0)
    {
        return false;
    }
    width = static_cast<int>((static_cast<unsigned>(head[16]) << 24) | (static_cast<unsigned>(head[17]) << 16) |
                             (static_cast<unsigned>(head[18]) << 8) | static_cast<unsigned>(head[19]));
    height = static_cast<int>((static_cast<unsigned>(head[20]) << 24) | (static_cast<unsigned>(head[21]) << 16) |
                              (static_cast<unsigned>(head[22]) << 8) | static_cast<unsigned>(head[23]));
    return true;
}

bool BmpSize(const std::wstring& path, int& width, int& height)
{
    std::vector<unsigned char> head = ReadHead(path, 26);
    if (head.size() < 26 || head[0] != 'B' || head[1] != 'M')
    {
        return false;
    }
    width = static_cast<int>(head[18] | (head[19] << 8) | (head[20] << 16) | (head[21] << 24));
    height = static_cast<int>(head[22] | (head[23] << 8) | (head[24] << 16) | (head[25] << 24));
    return true;
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
        count = CountImages(dir);
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
        Sleep(10);
    }
    if (got != nullptr)
    {
        *got = count;
    }
    return false;
}

// True when the file carries its format's end marker, i.e. the encoder is done.
bool ImageComplete(const std::wstring& path, const std::wstring& name)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 16)
    {
        CloseHandle(file);
        return false;
    }
    LARGE_INTEGER pos{};
    pos.QuadPart = size.QuadPart - 16;
    SetFilePointerEx(file, pos, nullptr, FILE_BEGIN);
    unsigned char tail[16] = {0};
    DWORD read = 0;
    ReadFile(file, tail, 16, &read, nullptr);
    CloseHandle(file);
    if (read < 16)
    {
        return false;
    }
    std::wstring lower = name;
    for (size_t i = 0; i < lower.size(); ++i)
    {
        if (lower[i] >= L'A' && lower[i] <= L'Z')
        {
            lower[i] = static_cast<wchar_t>(lower[i] - L'A' + L'a');
        }
    }
    if (lower.size() > 4 && lower.substr(lower.size() - 4) == L".png")
    {
        return memcmp(tail + 8, "IEND", 4) == 0 && tail[12] == 0xAE && tail[13] == 0x42 && tail[14] == 0x60 &&
               tail[15] == 0x82;
    }
    if (lower.size() > 4 && (lower.substr(lower.size() - 4) == L".jpg" || lower.substr(lower.size() - 4) == L".bmp"))
    {
        if (lower.substr(lower.size() - 4) == L".jpg")
        {
            return tail[14] == 0xFF && tail[15] == 0xD9;
        }
        return true; // BMP completeness is covered by the size field checks
    }
    return true;
}

// Authoritative completion signal: the plugin reports the result on its sink
// window, which our main thread dispatches inside PumpMessages().  Only after
// that notification does the image count matter - the encoder creates the file
// before it has finished writing it.  (WaitForCapture/Notify live below, after
// CMockHost, because they read its notification counter.)

size_t RssBytes()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == FALSE)
    {
        return 0;
    }
    return static_cast<size_t>(counters.WorkingSetSize);
}

unsigned GdiObjects()
{
    return GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
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
        if (m_lastNotify.find(L"已保存") != std::wstring::npos ||
            m_lastNotify.find(L"截图失败") != std::wstring::npos)
        {
            ++m_captureResults;
        }
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
    int m_captureResults{0}; // only "已保存"/"截图失败" messages
    std::wstring m_lastNotify;

private:
    std::wstring m_configDir;
    HWND m_mainWindow;
};

// Waits for the plugin's own result notification (dispatched to the mock host on
// the main thread) and only then evaluates the image count.  This is the only
// race free way to know that the encoder really finished the file.
bool WaitForCapture(const CMockHost& host, int resultBefore, const std::wstring& dir, size_t want,
                    DWORD timeoutMs, size_t* got = nullptr)
{
    const ULONGLONG start = GetTickCount64();
    for (;;)
    {
        PumpMessages();
        const std::vector<FileInfo> images = ListImages(dir);
        if (host.m_captureResults > resultBefore && images.size() >= want)
        {
            const FileInfo* newest = nullptr;
            for (const FileInfo& info : images)
            {
                if (newest == nullptr || info.writeTime >= newest->writeTime)
                {
                    newest = &info;
                }
            }
            if (newest != nullptr && ImageComplete(Join(dir, newest->name), newest->name))
            {
                if (got != nullptr)
                {
                    *got = images.size();
                }
                return true;
            }
        }
        if (GetTickCount64() - start >= timeoutMs)
        {
            break;
        }
        Sleep(10);
    }
    if (got != nullptr)
    {
        *got = CountImages(dir);
    }
    return false;
}

bool WaitForNotify(const CMockHost& host, int notifyBefore, DWORD timeoutMs)
{
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < timeoutMs)
    {
        PumpMessages();
        if (host.m_notifyCount > notifyBefore)
        {
            return true;
        }
        Sleep(10);
    }
    PumpMessages();
    return host.m_notifyCount > notifyBefore;
}

// Control ids copied from src/OptionsDialog.h (part of the dialog contract).
enum : int
{
    IDC_SAVE_DIR = 1001,
    IDC_FORMAT = 1004,
    IDC_QUALITY = 1005,
    IDC_TEMPLATE = 1008,
    IDC_BTN_TEST = 1017,
    IDC_DELAY = 1011
};

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
            // Park the modal dialog off-screen so the human at the desk is not disturbed.
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

    std::wstring Text(int id) const
    {
        wchar_t buffer[1024] = {0};
        if (dialog != nullptr)
        {
            GetDlgItemTextW(dialog, id, buffer, static_cast<int>(_countof(buffer)));
        }
        return std::wstring(buffer);
    }

    long ComboSelection(int id) const
    {
        HWND combo = (dialog != nullptr) ? GetDlgItem(dialog, id) : nullptr;
        return (combo != nullptr) ? static_cast<long>(SendMessageW(combo, CB_GETCURSEL, 0, 0)) : -1;
    }
};

// ---------------------------------------------------------------------------
// Shared test environment
// ---------------------------------------------------------------------------
struct Env
{
    std::wstring exeDir;
    std::wstring dllPath;
    std::wstring root;
    std::wstring configDir;
    std::wstring shots;
    HMODULE module{nullptr};
    ITMPlugin* plugin{nullptr};
    IPluginItem* item{nullptr};

    bool Load()
    {
        module = LoadLibraryW(dllPath.c_str());
        if (module == nullptr)
        {
            Print("  [FAIL] LoadLibrary err=%lu", static_cast<unsigned long>(GetLastError()));
            ++g_fail;
            return false;
        }
        g_dllBase = reinterpret_cast<void*>(module);
        MODULEINFO info{};
        if (GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) != FALSE)
        {
            g_dllBase = info.lpBaseOfDll;
            g_dllSize = info.SizeOfImage;
        }
        Print("    [dbg] dll base=%p size=0x%zX", g_dllBase, g_dllSize);
        auto getInstance = reinterpret_cast<ITMPlugin* (*)()>(GetProcAddress(module, "TMPluginGetInstance"));
        plugin = (getInstance != nullptr) ? getInstance() : nullptr;
        item = (plugin != nullptr) ? plugin->GetItem(0) : nullptr;
        return plugin != nullptr && item != nullptr;
    }

    void Unload()
    {
        if (module != nullptr)
        {
            FreeLibrary(module);
            module = nullptr;
            plugin = nullptr;
            item = nullptr;
        }
    }

    std::wstring IniPath() const { return Join(configDir, L"TMPluginScreenshot.ini"); }

    void WriteIni(const std::wstring& saveDir, const std::wstring& format, const std::wstring& quality,
                  const std::wstring& tmpl, const std::wstring& delay, const std::wstring& fallback,
                  bool hotkey = true)
    {
        WriteUtf16Ini(IniPath(), {{L"save_dir", saveDir},
                                  {L"format", format},
                                  {L"jpg_quality", quality},
                                  {L"file_name_template", tmpl},
                                  {L"copy_to_clipboard", L"1"},
                                  {L"include_cursor", L"0"},
                                  {L"hotkey_enabled", hotkey ? L"1" : L"0"},
                                  {L"hotkey_mods", L"ctrl+alt"},
                                  {L"hotkey_vk", L"65"},
                                  {L"delay_seconds", delay},
                                  {L"fallback_pictures_root", fallback},
                                  {L"last_saved", L""}});
    }
};


std::wstring NewestImage(const std::wstring& dir)
{
    std::wstring newest;
    unsigned long long best = 0;
    for (const FileInfo& info : ListImages(dir))
    {
        if (newest.empty() || info.writeTime >= best)
        {
            best = info.writeTime;
            newest = info.name;
        }
    }
    return newest;
}

// Returns the concatenated payload of all DQT segments of a JPEG.  Two files
// with different JPEG quality must have different quantization tables.
std::string JpegQuantizationTables(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return std::string();
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::vector<unsigned char> data(static_cast<size_t>(size.QuadPart), 0);
    DWORD read = 0;
    ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);
    data.resize(read);

    std::string tables;
    size_t i = 2;
    while (i + 3 < data.size())
    {
        if (data[i] != 0xFF)
        {
            ++i;
            continue;
        }
        const unsigned char marker = data[i + 1];
        if (marker == 0xD9)
        {
            break;
        }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01)
        {
            i += 2;
            continue;
        }
        const size_t length = (static_cast<size_t>(data[i + 2]) << 8) | data[i + 3];
        if (length < 2 || i + 2 + length > data.size())
        {
            break;
        }
        if (marker == 0xDB)
        {
            tables.append(reinterpret_cast<const char*>(&data[i + 4]), length - 2);
        }
        i += 2 + length;
    }
    return tables;
}

size_t FileSizeOf(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) == FALSE)
    {
        return 0;
    }
    return (static_cast<size_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

} // namespace

int main(int argc, char** argv)
{
    SetUnhandledExceptionFilter(&CrashFilter);
    AddVectoredExceptionHandler(1, &VehLogger);
    const std::string scenario = (argc > 1) ? argv[1] : "";
    Env env;
    env.exeDir = ExeDirectory();
    env.dllPath = Join(env.exeDir, L"TMPluginScreenshot.dll");
    env.root = Join(Join(env.exeDir, L".."), L"out-test\\qa\\runtime");
    env.configDir = Join(env.root, L"config");
    env.shots = Join(env.root, L"shots");

    Print("== qa_runtime scenario '%s'", scenario.c_str());
    Print("   dll   : %s", Narrow(env.dllPath).c_str());

    // Cross-process hotkey probe: exit 0 = the hotkey was free, 1 = it is taken.
    if (scenario == "hotkeyprobe")
    {
        const BOOL ok = RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'A');
        if (ok != FALSE)
        {
            Print("hotkeyprobe: Ctrl+Alt+A registered => FREE");
            UnregisterHotKey(nullptr, 1);
            return 0;
        }
        Print("hotkeyprobe: Ctrl+Alt+A refused (err=%lu) => IN USE",
              static_cast<unsigned long>(GetLastError()));
        return 1;
    }

    // Holds Ctrl+Alt+A for ~10s so another run can observe the conflict path.
    if (scenario == "hold")
    {
        const BOOL ok = RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'A');
        Print("hold: registered=%s", ok ? "yes" : "no");
        fflush(stdout);
        Sleep(10000);
        if (ok != FALSE)
        {
            UnregisterHotKey(nullptr, 1);
        }
        return 0;
    }

    DeleteTree(env.root);
    EnsureDir(env.configDir);
    EnsureDir(env.shots);

    CMockHost host(env.configDir, nullptr);

    // ------------------------------------------------------------------
    if (scenario == "inifresh")
    {
        // A brand new config dir: the plugin must create the ini itself.
        env.Load();
        env.plugin->OnInitialize(&host);
        Check(true, "OnInitialize on a fresh config dir did not crash");
        Sleep(150);
        const std::vector<unsigned char> head = ReadHead(env.IniPath(), 2);
        CheckF(head.size() == 2 && head[0] == 0xFF && head[1] == 0xFE,
              "fresh ini starts with FF FE (UTF-16LE BOM), got %02X %02X",
              head.size() > 0 ? head[0] : 0, head.size() > 1 ? head[1] : 0);
        env.Unload();
    }
    // ------------------------------------------------------------------
    else if (scenario == "chinesepath")
    {
        const std::wstring chineseDir = L"D:\\截图测试\\中文目录";
        env.WriteIni(env.shots, L"png", L"90", L"docs_yyyyMMdd_HHmmss", L"0", L"1");
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(300);

        // Drive the real settings dialog: type the Chinese path, press OK.
        DialogCloser closer;
        std::thread closerThread([&closer, &chineseDir]() {
            closer.Run();
            if (closer.dialog != nullptr)
            {
                SetDlgItemTextW(closer.dialog, IDC_SAVE_DIR, chineseDir.c_str());
                Sleep(120);
                PostMessageW(closer.dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
            }
        });
        const ITMPlugin::OptionReturn ret = env.plugin->ShowOptionsDialog(nullptr);
        closerThread.join();
        CheckF(ret == ITMPlugin::OR_OPTION_CHANGED, "dialog OK with the Chinese dir returned OR_OPTION_CHANGED (%d)",
               static_cast<int>(ret));

        // The plugin rewrote the ini itself: inspect the raw bytes.
        const std::wstring iniPath = env.IniPath();
        HANDLE file = CreateFileW(iniPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        std::string utf16;
        if (file != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER size{};
            GetFileSizeEx(file, &size);
            std::vector<char> raw(static_cast<size_t>(size.QuadPart), 0);
            DWORD read = 0;
            ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr);
            CloseHandle(file);
            raw.resize(read);
            if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF &&
                static_cast<unsigned char>(raw[1]) == 0xFE)
            {
                utf16.assign(raw.begin() + 2, raw.end());
            }
            std::wstring text;
            if (!utf16.empty())
            {
                text.resize(utf16.size() / sizeof(wchar_t));
                memcpy(&text[0], utf16.data(), text.size() * sizeof(wchar_t));
            }
            CheckF(text.find(chineseDir) != std::wstring::npos,
                  "the ini written by the plugin still contains the Chinese path verbatim");
            CheckF(text.find(L"???") == std::wstring::npos, "the ini contains no '???' replacement");
        }
        else
        {
            Check(false, "the ini file could not be read back");
        }

        // And a capture must land in that directory.
        Sleep(400);
        const size_t before = CountImages(chineseDir);
        PumpMessages();
        const int notifyBefore = host.m_captureResults;
        env.item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
        WaitForCapture(host, notifyBefore, chineseDir, before + 1, 8000);
        const size_t after = CountImages(chineseDir);
        CheckF(after == before + 1, "capture landed in D:\\截图测试\\中文目录 (%zu -> %zu)", before, after);
        if (after == before + 1)
        {
            for (const FileInfo& info : ListImages(chineseDir))
            {
                Print("    file: %s (%llu bytes)", Narrow(info.name.c_str()).c_str(), info.size);
            }
        }
        DeleteTree(chineseDir);
        env.Unload();
    }
    // ------------------------------------------------------------------
    else if (scenario == "fallback")
    {
        // Field-by-field fallback: no 'format' key, jpg_quality=abc, delay=999,
        // empty template.  The dialog shows the loaded values, so we can assert
        // them without waiting 30 seconds for the delay.
        const std::wstring iniPath = env.IniPath();
        const std::wstring body = std::wstring(L"[TMSnap]\r\n") + L"save_dir=" + env.shots +
                                  L"\r\njpg_quality=abc\r\nfile_name_template=\r\ndelay_seconds=999\r\n"
                                  L"hotkey_enabled=0\r\ncopy_to_clipboard=0\r\n";
        HANDLE file = CreateFileW(iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        const wchar_t bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(file, &bom, sizeof(bom), &written, nullptr);
        WriteFile(file, body.c_str(), static_cast<DWORD>(body.size() * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(file);

        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(300);

        DialogCloser closer;
        std::wstring quality;
        std::wstring delay;
        std::wstring tmpl;
        long format = -1;
        std::thread closerThread([&closer, &quality, &delay, &tmpl, &format]() {
            closer.Run();
            if (closer.dialog != nullptr)
            {
                Sleep(300); // let LoadToControls() populate the controls
                quality = closer.Text(IDC_QUALITY);
                delay = closer.Text(IDC_DELAY);
                tmpl = closer.Text(IDC_TEMPLATE);
                format = closer.ComboSelection(IDC_FORMAT);
                closer.Cancel();
            }
        });
        env.plugin->ShowOptionsDialog(nullptr);
        closerThread.join();
        if (closer.dialog == nullptr)
        {
            Check(false, "the settings dialog did not appear");
        }
        else
        {
            Print("    loaded values -> format combo=%ld quality=\"%s\" delay=\"%s\" template=\"%s\"", format,
                  Narrow(quality.c_str()).c_str(), Narrow(delay.c_str()).c_str(), Narrow(tmpl.c_str()).c_str());
            CheckF(format == 0, "missing 'format' key falls back to PNG (combo=%ld)", format);
            CheckF(quality == L"90", "jpg_quality=abc falls back to 90 (got \"%s\")",
                  Narrow(quality.c_str()).c_str());
            CheckF(delay == L"30", "delay_seconds=999 is clamped to 30 (got \"%s\")", Narrow(delay.c_str()).c_str());
            CheckF(tmpl == L"TMSnap_yyyyMMdd_HHmmss", "empty template falls back to the default (got \"%s\")",
                  Narrow(tmpl.c_str()).c_str());
        }

        Sleep(400);
        PumpMessages();
        const int notifyBefore = host.m_captureResults;
        env.item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
        size_t got = 0;
        const bool captured = WaitForCapture(host, notifyBefore, env.shots, 1, 8000, &got);
        CheckF(captured, "the plugin still captures a file after the fallback");
        for (const FileInfo& info : ListImages(env.shots))
        {
            Print("    file: %s (%llu bytes)", Narrow(info.name.c_str()).c_str(), info.size);
        }
        std::vector<FileInfo> files = ListImages(env.shots);
        if (!files.empty())
        {
            std::vector<unsigned char> head = ReadHead(Join(env.shots, files[0].name), 4);
            CheckF(head.size() >= 4 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G',
                  "output is a real PNG (89 50 4E 47), i.e. the format fallback really applies");
            CheckF(files[0].name.rfind(L"TMSnap_", 0) == 0,
                  "file name follows the default template (got \"%s\")", Narrow(files[0].name.c_str()).c_str());
        }
        env.Unload();
    }
    // ------------------------------------------------------------------
    else if (scenario == "one")
    {
        // argv[2]=format argv[3]=quality argv[4]=template(optional)
        const std::string format = (argc > 2) ? argv[2] : "png";
        const std::string quality = (argc > 3) ? argv[3] : "90";
        const std::wstring tmpl = (argc > 4) ? Widen(argv[4]) : std::wstring(L"snap_yyyyMMdd_HHmmss");
        env.WriteIni(env.shots, Widen(format), Widen(quality), tmpl, L"0", L"1");
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(350);
        const ULONGLONG start = GetTickCount64();
        PumpMessages();
        const int notifyBefore = host.m_captureResults;
        env.item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
        size_t got = 0;
        const bool ok = WaitForCapture(host, notifyBefore, env.shots, 1, 8000, &got);
        const ULONGLONG elapsed = GetTickCount64() - start;
        CheckF(ok, "capture produced a file in %llums", static_cast<unsigned long long>(elapsed));
        const std::vector<FileInfo> files = ListImages(env.shots);
        for (const FileInfo& info : files)
        {
            std::vector<unsigned char> head = ReadHead(Join(env.shots, info.name), 4);
            Print("    file: %s (%llu bytes) head=%02X %02X %02X %02X", Narrow(info.name.c_str()).c_str(),
                  info.size, head.size() > 0 ? head[0] : 0, head.size() > 1 ? head[1] : 0,
                  head.size() > 2 ? head[2] : 0, head.size() > 3 ? head[3] : 0);
        }
        env.Unload();
    }
    // ------------------------------------------------------------------
    else if (scenario == "paths")
    {
        // argv[2] = template.  Only checks that the produced name is legal.
        const std::string tmplArg = (argc > 2) ? argv[2] : "shot_yyyy-MM-dd";
        env.WriteIni(env.shots, L"png", L"90", Widen(tmplArg), L"0", L"1");
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(350);
        PumpMessages();
        const int notifyBefore = host.m_captureResults;
        env.item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
        size_t got = 0;
        const bool ok = WaitForCapture(host, notifyBefore, env.shots, 1, 8000, &got);
        CheckF(ok, "template \"%s\": a file was produced", tmplArg.c_str());
        const std::vector<FileInfo> files = ListImages(env.shots);
        for (const FileInfo& info : files)
        {
            const std::wstring& name = info.name;
            const bool illegal = name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos;
            CheckF(!illegal, "template \"%s\": name \"%s\" has no illegal characters", tmplArg.c_str(),
                   Narrow(name.c_str()).c_str());
            CheckF(name.size() <= 255, "template \"%s\": name length %zu <= 255", tmplArg.c_str(), name.size());
            CheckF(info.size > 0, "template \"%s\": file is not zero bytes (%llu)", tmplArg.c_str(), info.size);
            Print("    name: %s", Narrow(name.c_str()).c_str());
        }
        env.Unload();
    }
    // ------------------------------------------------------------------
    else if (scenario == "unwritable")
    {
        // A *file* is placed where the save dir should be, so EnsureDirectory()
        // fails no matter whether the test runs elevated.  Fallback disabled.
        const std::wstring blocked = Join(env.root, L"blocked");
        HANDLE file = CreateFileW(blocked.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        CloseHandle(file);
        env.WriteIni(blocked, L"png", L"90", L"snap_yyyyMMdd_HHmmss", L"0", L"0");
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(350);
        env.item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
        Sleep(2500);
        PumpMessages();
        CheckF(host.m_notifyCount > 0, "an unwritable save directory is reported to the user");
        CheckF(host.m_lastNotify.find(L"目录不可写") != std::wstring::npos,
              "the notification says the folder is not writable (got \"%s\")",
              Narrow(host.m_lastNotify.c_str()).c_str());
        CheckF(CountImages(env.shots) == 0, "no file was written to the (fallback) shots directory");
        // Look for zero byte leftovers anywhere under out-test/qa/runtime.
        size_t zeroByte = 0;
        for (const FileInfo& info : ListFiles(env.root))
        {
            if (info.size == 0 && info.name.rfind(L"blocked", 0) != 0)
            {
                ++zeroByte;
                Print("    zero-byte leftover: %s", Narrow(info.name.c_str()).c_str());
            }
        }
        CheckF(zeroByte == 0, "no zero-byte leftover files (%zu)", zeroByte);
        env.Unload();
    }
    // ------------------------------------------------------------------
    else if (scenario == "unique")
    {
        env.WriteIni(env.shots, L"png", L"90", L"fixed", L"0", L"1");
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(350);
        for (int i = 0; i < 3; ++i)
        {
            const size_t before = CountImages(env.shots);
            PumpMessages();
            const int notifyBefore = host.m_captureResults;
            env.item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
            size_t got = before;
            const bool arrived = WaitForCapture(host, notifyBefore, env.shots, before + 1, 8000, &got);
            CheckF(arrived, "capture %d produced a file (%zu -> %zu)", i + 1, before, got);
            Sleep(360);
        }
        const std::vector<FileInfo> files = ListImages(env.shots);
        std::vector<std::wstring> names;
        for (const FileInfo& info : files)
        {
            names.push_back(info.name);
            Print("    name: %s", Narrow(info.name.c_str()).c_str());
        }
        CheckF(files.size() == 3, "three captures produced three files (got %zu)", files.size());
        if (files.size() == 3)
        {
            CheckF(names[0] == L"fixed.png" && names[1] == L"fixed_1.png" && names[2] == L"fixed_2.png",
                  "collision suffixes are fixed.png / fixed_1.png / fixed_2.png");
        }
        env.Unload();
    }
    // ------------------------------------------------------------------
    else if (scenario == "delay")
    {
        env.WriteIni(env.shots, L"png", L"90", L"snap_yyyyMMdd_HHmmss", L"3", L"1");
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(400);
        const ULONGLONG start = GetTickCount64();
        PumpMessages();
        const int notifyBefore = host.m_captureResults;
        env.plugin->OnPluginCommand(3, nullptr, nullptr); // delay command
        size_t got = 0;
        const bool ok = WaitForCapture(host, notifyBefore, env.shots, 1, 8000, &got);
        const ULONGLONG elapsed = GetTickCount64() - start;
        CheckF(ok, "the delayed command produced a file");
        CheckF(elapsed >= 3000 && elapsed <= 4500, "the 3s delay really waited %llums (window 3000..4500)",
               static_cast<unsigned long long>(elapsed));
        env.Unload();
    }
    // ------------------------------------------------------------------
    else if (scenario == "region" || scenario == "regioncancel")
    {
        env.WriteIni(env.shots, L"png", L"90", L"snap_yyyyMMdd_HHmmss", L"0", L"1");
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(350);
        env.plugin->OnPluginCommand(1, nullptr, nullptr); // region command

        HWND overlay = nullptr;
        const ULONGLONG start = GetTickCount64();
        while (GetTickCount64() - start < 5000 && overlay == nullptr)
        {
            overlay = FindWindowW(L"TMPluginScreenshotOverlay", nullptr);
            if (overlay == nullptr)
            {
                PumpMessages();
                Sleep(20);
            }
        }
        CheckF(overlay != nullptr, "the region overlay window appeared");
        if (overlay != nullptr)
        {
            if (scenario == "region")
            {
                PumpMessages();
                const int notifyBefore = host.m_captureResults;
                PostMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
                Sleep(60);
                PostMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(180, 180));
                Sleep(60);
                PostMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(300, 250));
                Sleep(60);
                PostMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(300, 250));
                size_t got = 0;
                CheckF(WaitForCapture(host, notifyBefore, env.shots, 1, 8000, &got),
                       "the region selection produced a file");
                const std::vector<FileInfo> files = ListImages(env.shots);
                if (!files.empty())
                {
                    int w = 0;
                    int h = 0;
                    const bool png = PngSize(Join(env.shots, files[0].name), w, h);
                    CheckF(png && w == 200 && h == 150, "the region image is exactly 200x150 (got %dx%d)", w, h);
                }
            }
            else
            {
                PostMessageW(overlay, WM_KEYDOWN, VK_ESCAPE, 0);
                Sleep(2500);
                PumpMessages();
                CheckF(CountImages(env.shots) == 0, "ESC on the overlay produced no file");
            }
        }
        // The overlay must be gone afterwards.
        Sleep(400);
        CheckF(FindWindowW(L"TMPluginScreenshotOverlay", nullptr) == nullptr,
              "no overlay window is left behind after the region flow");
        env.Unload();
    }
    // ------------------------------------------------------------------
    else if (scenario == "activewindow")
    {
        env.WriteIni(env.shots, L"png", L"90", L"snap_yyyyMMdd_HHmmss", L"0", L"1");
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(350);
        HWND foreground = GetForegroundWindow();
        wchar_t cls[128] = {0};
        if (foreground != nullptr)
        {
            GetClassNameW(foreground, cls, static_cast<int>(_countof(cls)));
        }
        Print("    foreground window: %p class=\"%s\"", static_cast<void*>(foreground), Narrow(cls).c_str());
        PumpMessages();
        const int notifyBefore = host.m_captureResults;
        env.plugin->OnPluginCommand(2, nullptr, nullptr); // active window command
        size_t got = 0;
        const bool ok = WaitForCapture(host, notifyBefore, env.shots, 1, 8000, &got);
        CheckF(ok, "the active-window command produced a file");
        const std::vector<FileInfo> files = ListImages(env.shots);
        if (!files.empty())
        {
            int w = 0;
            int h = 0;
            const bool sized = PngSize(Join(env.shots, files[0].name), w, h);
            CheckF(sized && w > 0 && h > 0, "the active-window image has real dimensions (%dx%d)", w, h);
        }
        env.Unload();
    }
    // ------------------------------------------------------------------
    else if (scenario == "unloadcap")
    {
        // Does FreeLibrary immediately after a real capture crash the process?
        const bool clipboard = (argc > 2) ? (std::string(argv[2]) != "noclip") : true;
        const int preDelayMs = (argc > 3) ? atoi(argv[3]) : 0;
        if (preDelayMs > 0)
        {
            Sleep(static_cast<DWORD>(preDelayMs));
            PumpMessages();
            Print("    [dbg] waited %d ms before unload", preDelayMs);
            fflush(stdout);
        }
        WriteUtf16Ini(env.IniPath(), {{L"save_dir", env.shots},
                                      {L"format", L"png"},
                                      {L"jpg_quality", L"90"},
                                      {L"file_name_template", L"u_yyyyMMdd_HHmmss"},
                                      {L"copy_to_clipboard", clipboard ? L"1" : L"0"},
                                      {L"include_cursor", L"0"},
                                      {L"hotkey_enabled", L"1"},
                                      {L"hotkey_mods", L"ctrl+alt"},
                                      {L"hotkey_vk", L"65"},
                                      {L"delay_seconds", L"0"},
                                      {L"fallback_pictures_root", L"1"},
                                      {L"last_saved", L""}});
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(350);
        PumpMessages();
        const int before = host.m_captureResults;
        env.item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
        size_t got = 0;
        CheckF(WaitForCapture(host, before, env.shots, 1, 8000, &got), "capture finished before unload");
        Print("    [dbg] unloading (clipboard=%s), threads now=%d", clipboard ? "on" : "off", ThreadCount());
        fflush(stdout);
        const ULONGLONG unloadStart = GetTickCount64();
        env.Unload();
        Print("    [dbg] FreeLibrary took %llums, threads now=%d",
              static_cast<unsigned long long>(GetTickCount64() - unloadStart), ThreadCount());
        Print("    [dbg] FreeLibrary returned; plugin still loaded=%s", PluginStillLoaded() ? "yes" : "no");
        fflush(stdout);
        for (int t = 0; t < 5; ++t)
        {
            Sleep(300);
            Print("    [dbg] +%d00ms alive", t + 1);
            fflush(stdout);
        }
        Print("    [dbg] survived 2.5s after unload");
        fflush(stdout);
        Check(true, "FreeLibrary right after a capture did not crash the process");
    }
    // ------------------------------------------------------------------
    else if (scenario == "dpiaware")
    {
        // Does the dialog's "Capture now" button change the *host UI thread*'s
        // DPI awareness?  That would be a process-wide side effect on the host.
        env.WriteIni(env.shots, L"png", L"90", L"snap_yyyyMMdd_HHmmss", L"0", L"1");
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(300);

        const DPI_AWARENESS_CONTEXT before = GetThreadDpiAwarenessContext();
        Print("    UI thread DPI context before: %p", reinterpret_cast<void*>(before));

        DialogCloser closer;
        std::thread closerThread([&closer]() {
            closer.Run();
            if (closer.dialog != nullptr)
            {
                Sleep(200);
                PostMessageW(closer.dialog, WM_COMMAND, MAKEWPARAM(IDC_BTN_TEST, BN_CLICKED), 0);
                Sleep(2500); // the test capture runs synchronously on this thread
                closer.Cancel();
            }
        });
        env.plugin->ShowOptionsDialog(nullptr);
        closerThread.join();

        const DPI_AWARENESS_CONTEXT after = GetThreadDpiAwarenessContext();
        Print("    UI thread DPI context after : %p", reinterpret_cast<void*>(after));
        const bool nowPerMonitorV2 = AreDpiAwarenessContextsEqual(after, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
        const bool unchanged = AreDpiAwarenessContextsEqual(before, after) != FALSE;
        CheckF(unchanged, "the dialog's test capture left the host UI thread's DPI awareness untouched "
                          "(now PER_MONITOR_AWARE_V2 = %s)",
               nowPerMonitorV2 ? "yes" : "no");
    }
    // ------------------------------------------------------------------
    else if (scenario == "jpegquality")
    {
        // Capture at quality 95, then change the quality to 10 through the real
        // settings dialog and capture again.  Same process, no reload.
        env.WriteIni(env.shots, L"jpg", L"95", L"q_yyyyMMdd_HHmmss", L"0", L"1");
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(350);

        size_t got = 0;
        PumpMessages();
        int resultBefore = host.m_captureResults;
        env.item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
        CheckF(WaitForCapture(host, resultBefore, env.shots, 1, 8000, &got), "quality 95: capture produced a file");
        const std::wstring fileA = NewestImage(env.shots);
        const size_t sizeA = FileSizeOf(Join(env.shots, fileA));
        const std::string dqtA = JpegQuantizationTables(Join(env.shots, fileA));
        Print("    quality 95 -> %s (%zu bytes, %zu DQT bytes)", Narrow(fileA.c_str()).c_str(), sizeA,
              dqtA.size());

        // Change the quality to 10 through the dialog (the real user path).
        DialogCloser closer;
        std::thread closerThread([&closer]() {
            closer.Run();
            if (closer.dialog != nullptr)
            {
                Sleep(250);
                SetDlgItemTextW(closer.dialog, IDC_QUALITY, L"10");
                Sleep(120);
                PostMessageW(closer.dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
            }
        });
        const ITMPlugin::OptionReturn ret = env.plugin->ShowOptionsDialog(nullptr);
        closerThread.join();
        CheckF(ret == ITMPlugin::OR_OPTION_CHANGED, "the dialog accepted quality=10 (returned CHANGED)");

        Sleep(500);
        PumpMessages();
        resultBefore = host.m_captureResults;
        env.item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
        CheckF(WaitForCapture(host, resultBefore, env.shots, 2, 8000, &got), "quality 10: capture produced a file");
        const std::wstring fileB = NewestImage(env.shots);
        const size_t sizeB = FileSizeOf(Join(env.shots, fileB));
        const std::string dqtB = JpegQuantizationTables(Join(env.shots, fileB));
        Print("    quality 10 -> %s (%zu bytes, %zu DQT bytes)", Narrow(fileB.c_str()).c_str(), sizeB,
              dqtB.size());

        CheckF(!dqtA.empty() && !dqtB.empty(), "both files carry JPEG quantization tables");
        CheckF(dqtA != dqtB, "quality 95 and quality 10 use DIFFERENT quantization tables (setting takes effect)");
        if (!dqtA.empty() && dqtA == dqtB)
        {
            Print("    !! identical quantization tables => the JPG quality setting is IGNORED");
        }
        const double ratio = (sizeB > 0) ? static_cast<double>(sizeA) / static_cast<double>(sizeB) : 0.0;
        Print("    size ratio (q95/q10) = %.2f", ratio);
        CheckF(ratio > 2.0, "quality 95 is >2x larger than quality 10 (ratio %.2f)", ratio);
    }
    // ------------------------------------------------------------------
    else if (scenario == "unloadbusy")
    {
        // Known limitation the engineer documented: FreeLibrary while a job thread
        // is still inside plugin code.  We measure what actually happens.
        env.WriteIni(env.shots, L"png", L"90", L"busy_yyyyMMdd_HHmmss", L"5", L"1");
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(300);
        PumpMessages();
        const int before = host.m_captureResults;
        env.plugin->OnPluginCommand(3, nullptr, nullptr); // 5s delay -> job thread is sleeping
        Sleep(400);
        Print("    [dbg] job running (delay 5s), threads=%d, unloading now", ThreadCount());
        fflush(stdout);
        const ULONGLONG unloadStart = GetTickCount64();
        env.Unload();
        Print("    [dbg] FreeLibrary took %llums while a job was running; still loaded=%s; threads=%d",
              static_cast<unsigned long long>(GetTickCount64() - unloadStart),
              PluginStillLoaded() ? "yes" : "no", ThreadCount());
        fflush(stdout);
        Sleep(7000); // longer than the 5s delay, so a surviving job thread must wake up
        Print("    [dbg] survived 7s after unload-while-busy (jobs=%d)", host.m_captureResults - before);
        fflush(stdout);
        Check(true, "unload while a job was running: process survived (see [dbg] lines)");
    }
    // ------------------------------------------------------------------
    else if (scenario == "stress")
    {
        env.WriteIni(env.shots, L"png", L"90", L"snap_yyyyMMdd_HHmmss", L"0", L"1");
        env.Load();
        env.plugin->OnInitialize(&host);
        Sleep(400);

        const unsigned gdiBefore = GdiObjects();
        const size_t rssBefore = RssBytes();
        const int threadsBefore = ThreadCount();
        const int captureCount = (argc > 2) ? atoi(argv[2]) : 20;
        int success = 0;
        for (int i = 1; i <= captureCount; ++i)
        {
            const size_t before = CountImages(env.shots);
            PumpMessages();
            const int notifyBefore = host.m_captureResults;
            env.item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
            size_t got = before;
            if (WaitForCapture(host, notifyBefore, env.shots, before + 1, 8000, &got))
            {
                ++success;
            }
            Sleep(340); // stay above the 300ms debounce
        }
        const int threadsAfter = WaitThreads(threadsBefore + 1, 5000);
        const unsigned gdiAfter = GdiObjects();
        const size_t rssAfter = RssBytes();
        Print("    20 captures: threads %d -> %d", threadsBefore, threadsAfter);
        Print("    20 captures: success=%d gdi %u->%u rss %zu->%zu", success, gdiBefore, gdiAfter, rssBefore,
              rssAfter);
        CheckF(success == captureCount, "%d sequential captures all succeeded (%d/%d)", captureCount, success, captureCount);
        CheckF(static_cast<long long>(gdiAfter) - static_cast<long long>(gdiBefore) <= 4,
              "GDI object delta after 20 captures <= 4 (%u -> %u)", gdiBefore, gdiAfter);
        const long long rssDelta = static_cast<long long>(rssAfter) - static_cast<long long>(rssBefore);
        CheckF(rssDelta < 20LL * 1024 * 1024, "RSS growth after 20 captures < 20MB (%lld bytes)", rssDelta);
        CheckF(threadsAfter <= threadsBefore + 1,
              "one-thread-per-job model does not accumulate threads (%d -> %d, <= +1)", threadsBefore,
              threadsAfter);

        // 10 rapid requests during one session -> exactly one file.
        const size_t beforeBurst = CountImages(env.shots);
        PumpMessages();
        const int notifyBefore = host.m_captureResults;
        for (int i = 0; i < 10; ++i)
        {
            env.item->OnMouseEvent(IPluginItem::MT_LCLICKED, 0, 0, nullptr, 0);
            Sleep(5);
        }
        size_t got = beforeBurst;
        WaitForCapture(host, notifyBefore, env.shots, beforeBurst + 1, 8000, &got);
        Sleep(2500);
        PumpMessages();
        const size_t afterBurst = CountImages(env.shots);
        CheckF(afterBurst == beforeBurst + 1, "10 rapid clicks produced exactly 1 file (%zu -> %zu)", beforeBurst,
              afterBurst);

        // No temporary / zero byte leftovers.
        size_t tmp = 0;
        size_t zero = 0;
        size_t probes = 0;
        for (const FileInfo& info : ListFiles(env.shots))
        {
            if (info.name.size() > 4 && info.name.substr(info.name.size() - 4) == L".tmp")
            {
                ++tmp;
            }
            if (info.name.rfind(L".tmsnap_probe", 0) == 0)
            {
                ++probes;
            }
            if (info.size == 0)
            {
                ++zero;
                Print("    zero-byte file: %s", Narrow(info.name.c_str()).c_str());
            }
        }
        CheckF(probes == 0, "no .tmsnap_probe_* leftovers (%zu)", probes);
        CheckF(tmp == 0, "no *.tmp leftovers (%zu)", tmp);
        CheckF(zero == 0, "no zero-byte files (%zu)", zero);
        env.Unload();
    }
    // ------------------------------------------------------------------
    else
    {
        Print("unknown scenario '%s'", scenario.c_str());
        return 2;
    }

    Print("SCENARIO %s: PASS %d FAIL %d", scenario.c_str(), g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
