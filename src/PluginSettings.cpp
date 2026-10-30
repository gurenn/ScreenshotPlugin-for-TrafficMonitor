// PluginSettings.cpp - settings persistence, i18n text table, logging, helpers.
#include "PluginSettings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

HINSTANCE g_hDllInstance = nullptr;

namespace
{

// ---------------------------------------------------------------------------
// Logging state.  A plain wchar_t buffer + SRWLOCK keeps us free of any
// destructor that would run inside DllMain(DETACH)'s loader lock.
// ---------------------------------------------------------------------------
wchar_t g_logDir[MAX_PATH] = {0};
SRWLOCK g_logLock = SRWLOCK_INIT;

const wchar_t kIniSection[] = L"TMSnap";
const wchar_t kIniFileName[] = L"TMPluginScreenshot.ini";
const wchar_t kLogFileName[] = L"TMPluginScreenshot.log";

std::wstring LogPath()
{
    if (g_logDir[0] == L'\0')
    {
        return std::wstring();
    }
    std::wstring path(g_logDir);
    path += L"\\";
    path += kLogFileName;
    return path;
}

void RollLogIfNeeded(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
    {
        return;
    }
    ULARGE_INTEGER size;
    size.LowPart = fad.nFileSizeLow;
    size.HighPart = fad.nFileSizeHigh;
    if (size.QuadPart <= snap::kLogRollBytes)
    {
        return;
    }
    std::wstring backup = path + L".1";
    DeleteFileW(backup.c_str());
    MoveFileW(path.c_str(), backup.c_str());
}

} // namespace

// ---------------------------------------------------------------------------
// snap:: logging
// ---------------------------------------------------------------------------
namespace snap
{

void SetLogDirectory(const std::wstring& dir)
{
    AcquireSRWLockExclusive(&g_logLock);
    if (dir.empty())
    {
        g_logDir[0] = L'\0';
    }
    else
    {
        // Bounded copy; the config dir is always far shorter than MAX_PATH.
        size_t n = dir.size();
        if (n >= MAX_PATH)
        {
            n = MAX_PATH - 1;
        }
        wmemcpy(g_logDir, dir.c_str(), n);
        g_logDir[n] = L'\0';
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

void LogMessage(LogLevel level, const wchar_t* fmt, ...) noexcept
{
    if (fmt == nullptr)
    {
        return;
    }

    wchar_t body[1024];
    body[0] = L'\0';
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, args);
    va_end(args);

    wchar_t line[1200];
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"[%02u:%02u:%02u.%03u][%s] %s\r\n",
                 static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
                 static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds),
                 (level == LogLevel::Error) ? L"E" : L"I", body);

    OutputDebugStringW(line);

    AcquireSRWLockExclusive(&g_logLock);
    std::wstring path = LogPath();
    if (!path.empty())
    {
        RollLogIfNeeded(path);
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER zero;
            zero.QuadPart = 0;
            if (GetFileSizeEx(file, &zero) && zero.QuadPart == 0)
            {
                static const unsigned char kUtf8Bom[3] = {0xEF, 0xBB, 0xBF};
                DWORD written = 0;
                WriteFile(file, kUtf8Bom, 3, &written, nullptr);
            }

            char utf8[4096];
            int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, static_cast<int>(sizeof(utf8)) - 1,
                                            nullptr, nullptr);
            if (bytes > 1)
            {
                DWORD written = 0;
                WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
            }
            CloseHandle(file);
        }
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

std::wstring FormatString(const wchar_t* fmt, ...)
{
    std::wstring result;
    if (fmt == nullptr)
    {
        return result;
    }

    std::vector<wchar_t> buffer(512);
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        va_list args;
        va_start(args, fmt);
        int needed = _vsnwprintf_s(buffer.data(), buffer.size(), _TRUNCATE, fmt, args);
        va_end(args);
        if (needed >= 0)
        {
            result.assign(buffer.data(), static_cast<size_t>(needed));
            return result;
        }
        buffer.resize(buffer.size() * 2);
    }
    return result;
}

std::wstring Trim(const std::wstring& s)
{
    size_t begin = 0;
    while (begin < s.size() && (s[begin] == L' ' || s[begin] == L'\t' || s[begin] == L'\r' || s[begin] == L'\n'))
    {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && (s[end - 1] == L' ' || s[end - 1] == L'\t' || s[end - 1] == L'\r' || s[end - 1] == L'\n'))
    {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::wstring ToLowerAscii(const std::wstring& s)
{
    std::wstring result(s);
    for (size_t i = 0; i < result.size(); ++i)
    {
        wchar_t c = result[i];
        if (c >= L'A' && c <= L'Z')
        {
            result[i] = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return result;
}

bool EqualsNoCase(const std::wstring& a, const wchar_t* b)
{
    if (b == nullptr)
    {
        return a.empty();
    }
    return ToLowerAscii(a) == ToLowerAscii(std::wstring(b));
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty())
    {
        return name;
    }
    std::wstring result(dir);
    if (result.back() != L'\\' && result.back() != L'/')
    {
        result += L'\\';
    }
    result += name;
    return result;
}

std::wstring GetDirectoryPart(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
    {
        return std::wstring();
    }
    if (pos == 0)
    {
        return path.substr(0, 1);
    }
    return path.substr(0, pos);
}

std::wstring GetFileNamePart(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
    {
        return path;
    }
    return path.substr(pos + 1);
}

std::wstring GetModuleDirectory()
{
    wchar_t buffer[MAX_PATH * 2] = {0};
    DWORD len = GetModuleFileNameW(g_hDllInstance, buffer, static_cast<DWORD>(_countof(buffer)));
    if (len == 0 || len >= _countof(buffer))
    {
        return std::wstring();
    }
    return GetDirectoryPart(std::wstring(buffer, len));
}

std::wstring GetPicturesDirectory()
{
    wchar_t buffer[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, buffer)))
    {
        if (buffer[0] != L'\0')
        {
            return std::wstring(buffer);
        }
    }
    // Fall back to %USERPROFILE%\Pictures.
    wchar_t profile[MAX_PATH] = {0};
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", profile, static_cast<DWORD>(_countof(profile)));
    if (len > 0 && len < _countof(profile))
    {
        return JoinPath(std::wstring(profile, len), L"Pictures");
    }
    return std::wstring();
}

std::wstring ExpandEnv(const std::wstring& text)
{
    if (text.empty())
    {
        return text;
    }
    DWORD needed = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (needed == 0 || needed > 32768)
    {
        return text;
    }
    std::vector<wchar_t> buffer(needed + 1);
    DWORD written = ExpandEnvironmentStringsW(text.c_str(), buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
    if (written == 0 || written > buffer.size())
    {
        return text;
    }
    return std::wstring(buffer.data());
}

bool DirectoryExists(const std::wstring& dir)
{
    if (dir.empty())
    {
        return false;
    }
    DWORD attr = GetFileAttributesW(dir.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectory(const std::wstring& dir)
{
    if (dir.empty())
    {
        return false;
    }
    int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS)
    {
        return DirectoryExists(dir);
    }
    return DirectoryExists(dir);
}

bool IsDirectoryWritable(const std::wstring& dir)
{
    if (!DirectoryExists(dir))
    {
        return false;
    }
    // 用 CREATE_ALWAYS + 每次都不重名的探测文件，避免上一次异常退出留下的探测文件
    // 让这里误判成"不可写"（曾经用 CREATE_NEW，4 次撞名就直接返回 false）。
    const ULONGLONG stamp = GetTickCount64();
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        std::wstring probe =
            JoinPath(dir, FormatString(L".tmsnap_probe_%u_%llu_%d.tmp", static_cast<unsigned>(GetCurrentProcessId()),
                                       static_cast<unsigned long long>(stamp), attempt));
        HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file); // FILE_FLAG_DELETE_ON_CLOSE：不用再显式删除
            return true;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED || error == ERROR_WRITE_PROTECT || error == ERROR_PATH_NOT_FOUND ||
            error == ERROR_INVALID_NAME || error == ERROR_INVALID_DRIVE || error == ERROR_DISK_FULL)
        {
            return false;
        }
        // 其它错误（极少见）换个名字再试一次。
    }
    return false;
}

std::wstring GetTimeText(const SYSTEMTIME& st)
{
    return FormatString(L"%02u:%02u:%02u", static_cast<unsigned>(st.wHour),
                        static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
}

RECT VirtualScreenRect()
{
    RECT rc{};
    rc.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rc.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rc.right = rc.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rc.bottom = rc.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return rc;
}

bool IsSecureDesktop()
{
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == nullptr)
    {
        return true;
    }
    CloseDesktop(desktop);
    return false;
}

void SetThreadDpiAwareness()
{
    typedef DPI_AWARENESS_CONTEXT(WINAPI * PFN_SetThreadDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
    static PFN_SetThreadDpiAwarenessContext pfn = reinterpret_cast<PFN_SetThreadDpiAwarenessContext>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    if (pfn != nullptr)
    {
        pfn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

UINT GetDpiForHwndLazy(HWND hwnd)
{
    typedef UINT(WINAPI * PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow pfn = reinterpret_cast<PFN_GetDpiForWindow>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (pfn != nullptr && hwnd != nullptr)
    {
        UINT dpi = pfn(hwnd);
        if (dpi >= 48 && dpi <= 960)
        {
            return dpi;
        }
    }
    return GetSystemDpiLazy();
}

int GetSystemDpiLazy()
{
    HDC dc = GetDC(nullptr);
    if (dc == nullptr)
    {
        return 96;
    }
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    if (dpi < 48 || dpi > 960)
    {
        dpi = 96;
    }
    return dpi;
}

} // namespace snap

// ---------------------------------------------------------------------------
// Image format helpers
// ---------------------------------------------------------------------------
const wchar_t* ImageFormatExtension(ImageFormat format)
{
    switch (format)
    {
    case ImageFormat::Jpg:
        return L".jpg";
    case ImageFormat::Bmp:
        return L".bmp";
    case ImageFormat::Png:
    default:
        return L".png";
    }
}

const wchar_t* ImageFormatDisplayName(ImageFormat format)
{
    switch (format)
    {
    case ImageFormat::Jpg:
        return L"JPG";
    case ImageFormat::Bmp:
        return L"BMP";
    case ImageFormat::Png:
    default:
        return L"PNG";
    }
}

// ---------------------------------------------------------------------------
// HotkeyBinding
// ---------------------------------------------------------------------------
bool HotkeyBinding::IsValid() const
{
    const UINT kModMask = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;
    if ((modifiers & kModMask) == 0)
    {
        return false;
    }
    if (vk == 0 || vk > 0xFF)
    {
        return false;
    }
    // Reject "modifier only" virtual keys (the user pressed nothing but Ctrl).
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU ||
        vk == VK_LMENU || vk == VK_RMENU || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS || vk == VK_PAUSE ||
        vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)
    {
        return false;
    }
    return true;
}

std::wstring HotkeyBinding::ModifiersToIniText() const
{
    std::wstring text;
    if ((modifiers & MOD_CONTROL) != 0)
    {
        text += L"ctrl";
    }
    if ((modifiers & MOD_ALT) != 0)
    {
        text += text.empty() ? L"alt" : L"+alt";
    }
    if ((modifiers & MOD_SHIFT) != 0)
    {
        text += text.empty() ? L"shift" : L"+shift";
    }
    if ((modifiers & MOD_WIN) != 0)
    {
        text += text.empty() ? L"win" : L"+win";
    }
    return text;
}

std::wstring HotkeyBinding::ToText() const
{
    std::wstring text;
    if ((modifiers & MOD_CONTROL) != 0)
    {
        text += L"Ctrl";
    }
    if ((modifiers & MOD_ALT) != 0)
    {
        text += text.empty() ? L"Alt" : L"+Alt";
    }
    if ((modifiers & MOD_SHIFT) != 0)
    {
        text += text.empty() ? L"Shift" : L"+Shift";
    }
    if ((modifiers & MOD_WIN) != 0)
    {
        text += text.empty() ? L"Win" : L"+Win";
    }
    if (!text.empty())
    {
        text += L"+";
    }

    // Ask the keyboard layout for a friendly key name.
    wchar_t name[64] = {0};
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
    // Extended keys need bit 24 set so that GetKeyNameText renders "Ins"/"Del" etc.
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
        vk == VK_PRIOR || vk == VK_NEXT || vk == VK_LEFT || vk == VK_RIGHT ||
        vk == VK_UP || vk == VK_DOWN || vk == VK_DIVIDE || vk == VK_NUMLOCK)
    {
        scan |= 0x01000000;
    }
    if (GetKeyNameTextW(static_cast<LONG>(scan), name, static_cast<int>(_countof(name))) > 0)
    {
        text += name;
    }
    else if (vk >= L'A' && vk <= L'Z')
    {
        text += static_cast<wchar_t>(vk);
    }
    else if (vk >= L'0' && vk <= L'9')
    {
        text += static_cast<wchar_t>(vk);
    }
    else
    {
        text += snap::FormatString(L"VK%u", static_cast<unsigned>(vk));
    }
    return text;
}

HotkeyBinding HotkeyBinding::Default()
{
    HotkeyBinding binding;
    binding.modifiers = snap::kDefaultHotkeyMods;
    binding.vk = snap::kDefaultHotkeyVk;
    return binding;
}

bool HotkeyBinding::FromIni(const std::wstring& modsText, const std::wstring& vkText, HotkeyBinding& out)
{
    HotkeyBinding binding;
    binding.modifiers = 0;
    std::wstring mods = snap::ToLowerAscii(modsText);
    if (mods.find(L"ctrl") != std::wstring::npos || mods.find(L"control") != std::wstring::npos)
    {
        binding.modifiers |= MOD_CONTROL;
    }
    if (mods.find(L"alt") != std::wstring::npos)
    {
        binding.modifiers |= MOD_ALT;
    }
    if (mods.find(L"shift") != std::wstring::npos)
    {
        binding.modifiers |= MOD_SHIFT;
    }
    if (mods.find(L"win") != std::wstring::npos)
    {
        binding.modifiers |= MOD_WIN;
    }

    wchar_t* end = nullptr;
    unsigned long value = wcstoul(vkText.c_str(), &end, 10);
    if (end == vkText.c_str() || value == 0 || value > 0xFF)
    {
        binding.vk = snap::kDefaultHotkeyVk;
    }
    else
    {
        binding.vk = static_cast<UINT>(value);
    }

    if (!binding.IsValid())
    {
        out = HotkeyBinding::Default();
        return false;
    }
    out = binding;
    return true;
}

// ---------------------------------------------------------------------------
// CPluginSettings
// ---------------------------------------------------------------------------
CPluginSettings::CPluginSettings()
{
    ResetToDefault();
}

CPluginSettings::~CPluginSettings()
{
}

std::wstring CPluginSettings::DefaultTemplate()
{
    return L"TMSnap_yyyyMMdd_HHmmss";
}

std::wstring CPluginSettings::DefaultSaveDir()
{
    std::wstring pictures = snap::GetPicturesDirectory();
    if (pictures.empty())
    {
        return std::wstring();
    }
    return snap::JoinPath(pictures, L"TMSnap");
}

void CPluginSettings::ResetToDefault()
{
    data.saveDir = DefaultSaveDir();
    data.format = ImageFormat::Png;
    data.jpgQuality = 90;
    data.fileNameTemplate = DefaultTemplate();
    data.copyToClipboard = true;
    data.includeCursor = false;
    data.hotkey1 = HotkeySlot{true, HotkeyBinding::Default(), 0};
    data.hotkey2 = HotkeySlot{false, HotkeyBinding{MOD_CONTROL | MOD_ALT, 'R'}, 1};
    data.delaySeconds = 0;
    data.fallbackToPicturesRoot = true;
    data.lastSavedPath.clear();
}

void CPluginSettings::ApplyDefaults(PluginSettings& out) const
{
    out = PluginSettings();
    out.saveDir = DefaultSaveDir();
    out.fileNameTemplate = DefaultTemplate();
    out.hotkey1 = HotkeySlot{true, HotkeyBinding::Default(), 0};
    out.hotkey2 = HotkeySlot{false, HotkeyBinding{MOD_CONTROL | MOD_ALT, 'R'}, 1};
}

bool CPluginSettings::Initialize(const std::wstring& hostDir, const std::wstring& extendedDir)
{
    bool resolved = ResolveConfigDir(hostDir, extendedDir);
    if (resolved)
    {
        snap::SetLogDirectory(m_configDir);
    }
    Load();
    if (!m_persistent)
    {
        SNAP_LOGE(kErrDirNotWritable, L"configuration directory is not writable, running in memory-only mode");
    }
    return resolved;
}

bool CPluginSettings::ResolveConfigDir(const std::wstring& hostDir, const std::wstring& extendedDir)
{
    m_persistent = false;

    std::vector<std::wstring> candidates;
    if (!hostDir.empty())
    {
        candidates.push_back(snap::ExpandEnv(hostDir));
    }
    if (!extendedDir.empty())
    {
        candidates.push_back(snap::ExpandEnv(extendedDir));
    }
    std::wstring moduleDir = snap::GetModuleDirectory();
    if (!moduleDir.empty())
    {
        candidates.push_back(snap::JoinPath(moduleDir, L"config"));
    }

    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const std::wstring& dir = candidates[i];
        if (dir.empty())
        {
            continue;
        }
        if (!snap::EnsureDirectory(dir))
        {
            continue;
        }
        if (!snap::IsDirectoryWritable(dir))
        {
            continue;
        }
        m_configDir = dir;
        m_iniPath = snap::JoinPath(dir, kIniFileName);
        m_persistent = true;
        return true;
    }

    // Level 3b: %APPDATA%\TrafficMonitor\plugins\TMPluginScreenshot
    wchar_t appData[MAX_PATH] = {0};
    DWORD len = GetEnvironmentVariableW(L"APPDATA", appData, static_cast<DWORD>(_countof(appData)));
    if (len > 0 && len < _countof(appData))
    {
        std::wstring dir = snap::JoinPath(std::wstring(appData, len),
                                          L"TrafficMonitor\\plugins\\TMPluginScreenshot");
        if (snap::EnsureDirectory(dir) && snap::IsDirectoryWritable(dir))
        {
            m_configDir = dir;
            m_iniPath = snap::JoinPath(dir, kIniFileName);
            m_persistent = true;
            return true;
        }
    }

    // Last resort: keep working with in-memory settings.
    m_configDir = snap::GetModuleDirectory();
    m_iniPath = m_configDir.empty() ? std::wstring() : snap::JoinPath(m_configDir, kIniFileName);
    m_persistent = false;
    return false;
}

namespace
{

std::wstring ReadIniString(const std::wstring& iniPath, const wchar_t* key, const wchar_t* defaultValue)
{
    wchar_t buffer[2048] = {0};
    GetPrivateProfileStringW(kIniSection, key, defaultValue, buffer, static_cast<DWORD>(_countof(buffer)),
                             iniPath.c_str());
    return std::wstring(buffer);
}

int ReadIniInt(const std::wstring& iniPath, const wchar_t* key, int defaultValue)
{
    std::wstring text = ReadIniString(iniPath, key, nullptr);
    if (text.empty())
    {
        return defaultValue;
    }
    wchar_t* end = nullptr;
    long value = wcstol(text.c_str(), &end, 10);
    if (end == text.c_str())
    {
        return defaultValue;
    }
    return static_cast<int>(value);
}

bool ReadIniBool(const std::wstring& iniPath, const wchar_t* key, bool defaultValue)
{
    std::wstring text = ReadIniString(iniPath, key, nullptr);
    if (text.empty())
    {
        return defaultValue;
    }
    text = snap::Trim(text);
    if (text == L"1")
    {
        return true;
    }
    if (text == L"0")
    {
        return false;
    }
    if (snap::EqualsNoCase(text, L"true"))
    {
        return true;
    }
    if (snap::EqualsNoCase(text, L"false"))
    {
        return false;
    }
    return defaultValue;
}

int ClampInt(int value, int minValue, int maxValue)
{
    if (value < minValue)
    {
        return minValue;
    }
    if (value > maxValue)
    {
        return maxValue;
    }
    return value;
}

bool IsReservedName(const std::wstring& stem)
{
    static const wchar_t* kReserved[] = {L"CON", L"PRN",  L"AUX",  L"NUL",  L"COM1", L"COM2", L"COM3",
                                         L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
                                         L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6",
                                         L"LPT7", L"LPT8", L"LPT9"};
    std::wstring upper = stem;
    for (size_t i = 0; i < upper.size(); ++i)
    {
        if (upper[i] >= L'a' && upper[i] <= L'z')
        {
            upper[i] = static_cast<wchar_t>(upper[i] - L'a' + L'A');
        }
    }
    for (size_t i = 0; i < _countof(kReserved); ++i)
    {
        if (upper == kReserved[i])
        {
            return true;
        }
    }
    return false;
}

} // namespace

bool CPluginSettings::Load()
{
    ApplyDefaults(data);

    if (m_iniPath.empty())
    {
        return false;
    }

    DWORD attr = GetFileAttributesW(m_iniPath.c_str());
    bool exists = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
    if (!exists)
    {
        // First run: persist the defaults so that the ini (and its BOM) exists.
        if (m_persistent)
        {
            Save();
        }
        return true;
    }

    // ---- field by field fallback (R-22) -----------------------------------
    std::wstring saveDir = snap::Trim(ReadIniString(m_iniPath, L"save_dir", nullptr));
    saveDir = snap::ExpandEnv(saveDir);
    if (!saveDir.empty())
    {
        data.saveDir = saveDir;
    }
    if (data.saveDir.empty())
    {
        data.saveDir = DefaultSaveDir();
    }

    std::wstring format = snap::ToLowerAscii(snap::Trim(ReadIniString(m_iniPath, L"format", nullptr)));
    if (format == L"jpg" || format == L"jpeg")
    {
        data.format = ImageFormat::Jpg;
    }
    else if (format == L"bmp")
    {
        data.format = ImageFormat::Bmp;
    }
    else
    {
        data.format = ImageFormat::Png;
    }

    data.jpgQuality = ClampInt(ReadIniInt(m_iniPath, L"jpg_quality", 90), 1, 100);

    std::wstring tmpl = snap::Trim(ReadIniString(m_iniPath, L"file_name_template", nullptr));
    data.fileNameTemplate = tmpl.empty() ? DefaultTemplate() : tmpl;

    data.copyToClipboard = ReadIniBool(m_iniPath, L"copy_to_clipboard", true);
    data.includeCursor = ReadIniBool(m_iniPath, L"include_cursor", false);
    data.fallbackToPicturesRoot = ReadIniBool(m_iniPath, L"fallback_pictures_root", true);

    // 热键 1：沿用旧键名 hotkey_enabled/hotkey_mods/hotkey_vk（向后兼容）。
    data.hotkey1.enabled = ReadIniBool(m_iniPath, L"hotkey_enabled", true);
    std::wstring mods = ReadIniString(m_iniPath, L"hotkey_mods", L"ctrl+alt");
    std::wstring vk = ReadIniString(m_iniPath, L"hotkey_vk", L"65");
    HotkeyBinding binding;
    HotkeyBinding::FromIni(mods, vk, binding);
    data.hotkey1.binding = binding;
    data.hotkey1.action = ClampInt(ReadIniInt(m_iniPath, L"hotkey1_action", 0), 0, 3);

    // 热键 2：新键名，默认关闭。
    data.hotkey2.enabled = ReadIniBool(m_iniPath, L"hotkey2_enabled", false);
    std::wstring mods2 = ReadIniString(m_iniPath, L"hotkey2_mods", L"ctrl+alt");
    std::wstring vk2 = ReadIniString(m_iniPath, L"hotkey2_vk", L"82"); // 'R'
    HotkeyBinding binding2;
    HotkeyBinding::FromIni(mods2, vk2, binding2);
    data.hotkey2.binding = binding2;
    data.hotkey2.action = ClampInt(ReadIniInt(m_iniPath, L"hotkey2_action", 1), 0, 3);

    data.delaySeconds = ClampInt(ReadIniInt(m_iniPath, L"delay_seconds", 0), 0, 30);

    data.lastSavedPath = snap::Trim(ReadIniString(m_iniPath, L"last_saved", nullptr));

    return true;
}

bool CPluginSettings::EnsureIniUnicode()
{
    if (m_iniPath.empty())
    {
        return false;
    }

    bool needCreate = false;
    HANDLE existing = CreateFileW(m_iniPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (existing == INVALID_HANDLE_VALUE)
    {
        needCreate = true;
    }
    else
    {
        unsigned char bom[2] = {0, 0};
        DWORD read = 0;
        ReadFile(existing, bom, 2, &read, nullptr);
        CloseHandle(existing);
        if (!(read == 2 && bom[0] == 0xFF && bom[1] == 0xFE))
        {
            // Existing ANSI ini: recreate as UTF-16LE.  Load() already read the
            // old values into memory, and Save() rewrites every key.
            DeleteFileW(m_iniPath.c_str());
            needCreate = true;
        }
    }

    if (needCreate)
    {
        HANDLE file = CreateFileW(m_iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        const wchar_t bom = 0xFEFF;
        DWORD written = 0;
        BOOL ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr);
        CloseHandle(file);
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

bool CPluginSettings::Save()
{
    if (!m_persistent || m_iniPath.empty())
    {
        return false;
    }
    if (!EnsureIniUnicode())
    {
        SNAP_LOGE(kErrWrite, L"cannot prepare ini file: %s", m_iniPath.c_str());
        return false;
    }

    auto writeString = [this](const wchar_t* key, const std::wstring& value) -> bool {
        return WritePrivateProfileStringW(kIniSection, key, value.c_str(), m_iniPath.c_str()) != FALSE;
    };
    auto writeInt = [this](const wchar_t* key, int value) -> bool {
        std::wstring text = snap::FormatString(L"%d", value);
        return WritePrivateProfileStringW(kIniSection, key, text.c_str(), m_iniPath.c_str()) != FALSE;
    };

    bool ok = true;
    ok = writeString(L"save_dir", data.saveDir) && ok;
    const wchar_t* formatText = L"png";
    if (data.format == ImageFormat::Jpg)
    {
        formatText = L"jpg";
    }
    else if (data.format == ImageFormat::Bmp)
    {
        formatText = L"bmp";
    }
    ok = writeString(L"format", formatText) && ok;
    ok = writeInt(L"jpg_quality", data.jpgQuality) && ok;
    ok = writeString(L"file_name_template", data.fileNameTemplate) && ok;
    ok = writeInt(L"copy_to_clipboard", data.copyToClipboard ? 1 : 0) && ok;
    ok = writeInt(L"include_cursor", data.includeCursor ? 1 : 0) && ok;
    ok = writeInt(L"hotkey_enabled", data.hotkey1.enabled ? 1 : 0) && ok;
    ok = writeString(L"hotkey_mods", data.hotkey1.binding.ModifiersToIniText()) && ok;
    ok = writeInt(L"hotkey_vk", static_cast<int>(data.hotkey1.binding.vk)) && ok;
    ok = writeInt(L"hotkey1_action", data.hotkey1.action) && ok;
    ok = writeInt(L"hotkey2_enabled", data.hotkey2.enabled ? 1 : 0) && ok;
    ok = writeString(L"hotkey2_mods", data.hotkey2.binding.ModifiersToIniText()) && ok;
    ok = writeInt(L"hotkey2_vk", static_cast<int>(data.hotkey2.binding.vk)) && ok;
    ok = writeInt(L"hotkey2_action", data.hotkey2.action) && ok;
    ok = writeInt(L"delay_seconds", data.delaySeconds) && ok;
    ok = writeInt(L"fallback_pictures_root", data.fallbackToPicturesRoot ? 1 : 0) && ok;
    ok = writeString(L"last_saved", data.lastSavedPath) && ok;

    if (!ok)
    {
        SNAP_LOGE(kErrWrite, L"failed to write ini file: %s", m_iniPath.c_str());
    }
    return ok;
}

bool CPluginSettings::SaveLastSavedPath(const std::wstring& path)
{
    data.lastSavedPath = path;
    if (!m_persistent || m_iniPath.empty())
    {
        return false;
    }
    return WritePrivateProfileStringW(kIniSection, L"last_saved", path.c_str(), m_iniPath.c_str()) != FALSE;
}

std::wstring CPluginSettings::EffectiveSaveDir() const
{
    std::wstring dir = snap::ExpandEnv(data.saveDir);
    if (dir.empty())
    {
        dir = DefaultSaveDir();
    }
    // Strip a trailing separator so that JoinPath() stays canonical.
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/'))
    {
        dir.pop_back();
    }
    return dir;
}

std::wstring CPluginSettings::ExpandTemplateFor(const std::wstring& tmpl, const SYSTEMTIME& st) const
{
    std::wstring out;
    out.reserve(tmpl.size() + 16);
    const wchar_t* p = tmpl.c_str();
    size_t i = 0;
    while (i < tmpl.size())
    {
        const wchar_t* cur = p + i;
        if (wcsncmp(cur, L"yyyy", 4) == 0)
        {
            out += snap::FormatString(L"%04u", static_cast<unsigned>(st.wYear));
            i += 4;
        }
        else if (wcsncmp(cur, L"MM", 2) == 0)
        {
            out += snap::FormatString(L"%02u", static_cast<unsigned>(st.wMonth));
            i += 2;
        }
        else if (wcsncmp(cur, L"dd", 2) == 0)
        {
            out += snap::FormatString(L"%02u", static_cast<unsigned>(st.wDay));
            i += 2;
        }
        else if (wcsncmp(cur, L"HH", 2) == 0)
        {
            out += snap::FormatString(L"%02u", static_cast<unsigned>(st.wHour));
            i += 2;
        }
        else if (wcsncmp(cur, L"mm", 2) == 0)
        {
            out += snap::FormatString(L"%02u", static_cast<unsigned>(st.wMinute));
            i += 2;
        }
        else if (wcsncmp(cur, L"ss", 2) == 0)
        {
            out += snap::FormatString(L"%02u", static_cast<unsigned>(st.wSecond));
            i += 2;
        }
        else
        {
            out += *cur;
            i += 1;
        }
    }
    return out;
}

std::wstring CPluginSettings::ExpandTemplate(const SYSTEMTIME& st) const
{
    std::wstring tmpl = data.fileNameTemplate.empty() ? DefaultTemplate() : data.fileNameTemplate;
    return ExpandTemplateFor(tmpl, st);
}

std::wstring CPluginSettings::SanitizeFileName(const std::wstring& name)
{
    std::wstring out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i)
    {
        wchar_t c = name[i];
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' ||
            c == L'>' || c == L'|' || c < 0x20 || c == 0x7F)
        {
            out += L'_';
        }
        else
        {
            out += c;
        }
    }
    // Trailing dots and spaces are illegal on Windows and confuse some tools.
    while (!out.empty() && (out.back() == L'.' || out.back() == L' '))
    {
        out.pop_back();
    }
    if (IsReservedName(out))
    {
        out.insert(out.begin(), L'_');
    }
    if (out.size() > 100)
    {
        out.resize(100);
    }
    return out;
}

std::wstring CPluginSettings::BuildFileName(const SYSTEMTIME& st) const
{
    std::wstring stem = SanitizeFileName(ExpandTemplate(st));
    if (stem.empty())
    {
        stem = SanitizeFileName(ExpandTemplateFor(DefaultTemplate(), st));
    }
    if (stem.empty())
    {
        stem = L"TMSnap";
    }
    std::wstring name = stem;
    name += ImageFormatExtension(data.format);
    return name;
}

bool CPluginSettings::EnsureSaveDir(std::wstring& outErr) const
{
    outErr.clear();
    std::wstring dir = EffectiveSaveDir();
    if (dir.empty())
    {
        outErr = L"<empty>";
        return false;
    }
    if (!snap::EnsureDirectory(dir))
    {
        outErr = dir;
        return false;
    }
    if (!snap::IsDirectoryWritable(dir))
    {
        outErr = dir;
        return false;
    }
    return true;
}

bool CPluginSettings::ValidateSettings(PluginSettings& io, std::wstring& outErr) const
{
    outErr.clear();

    io.saveDir = snap::Trim(snap::ExpandEnv(io.saveDir));
    if (io.saveDir.empty())
    {
        io.saveDir = DefaultSaveDir();
    }
    if (io.saveDir.empty())
    {
        outErr = L"save_dir";
        return false;
    }

    io.fileNameTemplate = snap::Trim(io.fileNameTemplate);
    if (io.fileNameTemplate.empty())
    {
        io.fileNameTemplate = DefaultTemplate();
    }

    io.jpgQuality = ClampInt(io.jpgQuality, 1, 100);
    io.delaySeconds = ClampInt(io.delaySeconds, 0, 30);

    if (io.hotkey1.enabled && !io.hotkey1.binding.IsValid())
    {
        io.hotkey1.binding = HotkeyBinding::Default();
        outErr = L"hotkey1";
        return false;
    }
    if (io.hotkey2.enabled && !io.hotkey2.binding.IsValid())
    {
        io.hotkey2.binding = HotkeyBinding{MOD_CONTROL | MOD_ALT, 'R'};
        outErr = L"hotkey2";
        return false;
    }
    if (io.hotkey1.enabled && io.hotkey2.enabled &&
        io.hotkey1.binding.vk == io.hotkey2.binding.vk &&
        io.hotkey1.binding.modifiers == io.hotkey2.binding.modifiers)
    {
        outErr = L"duplicate hotkey";
        return false;
    }
    io.hotkey1.action = ClampInt(io.hotkey1.action, 0, 3);
    io.hotkey2.action = ClampInt(io.hotkey2.action, 0, 3);

    if (!snap::EnsureDirectory(io.saveDir) || !snap::IsDirectoryWritable(io.saveDir))
    {
        outErr = io.saveDir;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// CUxText
// ---------------------------------------------------------------------------
namespace
{

struct TextEntry
{
    const wchar_t* zh;
    const wchar_t* en;
};

// Index order MUST match enum class StringId.
const TextEntry kTextTable[] = {
    // plugin info
    {L"截图插件",
     L"Screenshot Plugin"},
    {L"为 TrafficMonitor 提供全屏/区域/活动窗口截图，支持全局热键与 PNG/JPG/BMP 保存",
     L"Capture the full screen, a region or the active window from TrafficMonitor. Global hotkey, PNG/JPG/BMP output."},
    {L"gurenn", L"gurenn"},
    {L"Copyright (C) 2026 gurenn", L"Copyright (C) 2026 gurenn"},
    {L"1.2.0", L"1.2.0"},
    {L"https://github.com/gurenn/ScreenshotPlugin-for-TrafficMonitor", L"https://github.com/gurenn/ScreenshotPlugin-for-TrafficMonitor"},
    // item name
    {L"截图", L"Capture"},
    // commands
    {L"全屏截图", L"Capture Full Screen"},
    {L"区域截图", L"Capture Region"},
    {L"活动窗口截图", L"Capture Active Window"},
    {L"延时截图", L"Capture with Delay"},
    {L"打开保存目录", L"Open Save Folder"},
    {L"设置…", L"Settings..."},
    // tooltip
    {L"点击截图（默认全屏）｜%s", L"Click to capture (full screen) | %s"},
    {L"上次：%s（%s）", L"Last: %s (%s)"},
    {L"热键已关闭", L"Hotkey disabled"},
    // notifications
    {L"已保存：%s", L"Saved: %s"},
    {L"已保存：%s（%s）", L"Saved: %s (%s)"},
    {L"截图失败：%s（%s）", L"Capture failed: %s (%s)"},
    {L"复制到剪贴板失败", L"Failed to copy to the clipboard"},
    {L"热键 %s 已被占用，请在设置中更换", L"Hotkey %s is already in use, please change it in Settings"},
    {L"热键已启用：%s", L"Hotkey enabled: %s"},
    {L"热键已关闭", L"Hotkey disabled"},
    {L"保存目录不存在，已打开上级目录", L"Save folder does not exist, opened its parent folder instead"},
    {L"试截成功：%s", L"Test capture saved: %s"},
    {L"试截失败：%s（%s）", L"Test capture failed: %s (%s)"},
    // SnapCode texts
    {L"成功", L"OK"},
    {L"已取消", L"Cancelled"},
    {L"正在截图，请稍候", L"A capture is already running"},
    {L"操作过快，已忽略", L"Triggered too fast, ignored"},
    {L"设置对话框打开中，已忽略", L"Settings dialog is open, ignored"},
    {L"无法获取屏幕画面", L"Cannot access the screen device context"},
    {L"屏幕抓取失败", L"Screen capture failed"},
    {L"未找到可截取的窗口", L"No capturable window found"},
    {L"图像过大，已拒绝", L"Image too large, rejected"},
    {L"当前桌面不可截取（可能是锁屏或 UAC 提示）",
     L"The current desktop cannot be captured (lock screen or UAC prompt)"},
    {L"图像编码组件不可用（WIC）", L"Image encoder unavailable (WIC)"},
    {L"编码器不可用或写入失败", L"Encoder unavailable or the write failed"},
    {L"写入文件失败", L"Failed to write the file"},
    {L"目录不可写", L"The folder is not writable"},
    {L"目录创建失败", L"Failed to create the folder"},
    {L"复制到剪贴板失败", L"Failed to copy to the clipboard"},
    {L"热键已被占用", L"The hotkey is already in use"},
    {L"内存不足", L"Out of memory"},
    {L"内部错误", L"Internal error"},
    // dialog
    {L"截图插件设置", L"Screenshot Plugin Settings"},
    {L"保存", L"Save"},
    {L"截图", L"Capture"},
    {L"关于", L"About"},
    {L"保存目录", L"Save folder"},
    {L"浏览…", L"Browse..."},
    {L"打开目录", L"Open"},
    {L"图片格式", L"Format"},
    {L"JPG 质量", L"JPG quality"},
    {L"文件名模板", L"File name template"},
    {L"示例：%s", L"Example: %s"},
    {L"同时复制到剪贴板", L"Also copy to the clipboard"},
    {L"延时（秒）", L"Delay (seconds)"},
    {L"包含鼠标光标", L"Include the mouse cursor"},
    {L"启用热键 1", L"Enable hotkey 1"},
    {L"热键", L"Hotkey"},
    {L"立即截屏", L"Capture now"},
    {L"确定", L"OK"},
    {L"取消", L"Cancel"},
    {L"恢复默认", L"Defaults"},
    {L"截图插件 1.2.0\n作者：gurenn\n插件接口版本：8（宿主 1.8.6 实际为 7）",
     L"Screenshot Plugin 1.2.0\nAuthor: gurenn\nPlugin API version: 8 (host 1.8.6 implements API 7)"},
    {L"请按下新的热键…", L"Press the new hotkey..."},
    {L"热键必须包含至少一个修饰键（Ctrl/Alt/Shift/Win）",
     L"The hotkey must include at least one modifier (Ctrl/Alt/Shift/Win)"},
    {L"该热键已被其它程序占用，请换一个", L"This hotkey is used by another program, please pick another one"},
    {L"保存目录不可写：%s", L"The save folder is not writable: %s"},
    {L"模板为空，已使用默认模板", L"The template was empty, the default one is used"},
    {L"保存配置失败：%s", L"Failed to save the settings: %s"},
    {L"就绪", L"Ready"},
    {L"目录不存在", L"Folder does not exist"},
    {L"正在试截…", L"Capturing..."},
    {L"启用热键 2", L"Enable hotkey 2"},
    {L"动作", L"Action"},
    {L"两个热键不能相同", L"The two hotkeys must be different"},
    {L"选择要截取的窗口", L"Select a window to capture"},
};

const size_t kTextCount = sizeof(kTextTable) / sizeof(kTextTable[0]);

} // namespace

CUxText::CUxText()
{
    Init(2052);
}

void CUxText::Init(unsigned short hostLangId)
{
    m_langId = hostLangId;
    // 宿主"语言=跟随系统"时 GetLanguageId() 返回 0（1.8.6 实测），此时改用系统 UI 语言判断，
    // 避免误落到英文文案（真实事故：2026-10-30 中文系统下界面全变英文）。
    unsigned short effective = hostLangId;
    if (effective == 0)
    {
        effective = static_cast<unsigned short>(GetUserDefaultUILanguage());
    }
    switch (effective)
    {
    case 2052: // zh-CN
    case 1028: // zh-TW
    case 3076: // zh-HK
    case 5124: // zh-MO
        m_chinese = true;
        break;
    default:
        m_chinese = false;
        break;
    }
}

const wchar_t* CUxText::Str(StringId id) const
{
    int index = static_cast<int>(id);
    if (index < 0 || static_cast<size_t>(index) >= kTextCount)
    {
        return L"";
    }
    return m_chinese ? kTextTable[index].zh : kTextTable[index].en;
}

const wchar_t* CUxText::SnapCodeText(SnapCode code) const
{
    switch (code)
    {
    case kOk:
        return Str(StringId::kCodeOk);
    case kCancelled:
        return Str(StringId::kCodeCancelled);
    case kIgnoredBusy:
        return Str(StringId::kCodeBusy);
    case kIgnoredDebounce:
        return Str(StringId::kCodeDebounce);
    case kIgnoredOptionsOpen:
        return Str(StringId::kCodeOptionsOpen);
    case kErrCaptureDC:
        return Str(StringId::kCodeErrCaptureDC);
    case kErrBitBlt:
        return Str(StringId::kCodeErrBitBlt);
    case kErrWindowNotFound:
        return Str(StringId::kCodeErrWindowNotFound);
    case kErrTooLarge:
        return Str(StringId::kCodeErrTooLarge);
    case kErrBlackFrame:
        return Str(StringId::kCodeErrBlackFrame);
    case kErrWicFactory:
        return Str(StringId::kCodeErrWicFactory);
    case kErrWicEncoder:
        return Str(StringId::kCodeErrWicEncoder);
    case kErrWrite:
        return Str(StringId::kCodeErrWrite);
    case kErrDirNotWritable:
        return Str(StringId::kCodeErrDirNotWritable);
    case kErrDirCreate:
        return Str(StringId::kCodeErrDirCreate);
    case kErrClipboard:
        return Str(StringId::kCodeErrClipboard);
    case kErrHotkeyConflict:
        return Str(StringId::kCodeErrHotkeyConflict);
    case kErrNoMemory:
        return Str(StringId::kCodeErrNoMemory);
    case kErrInternal:
    default:
        return Str(StringId::kCodeErrInternal);
    }
}
