// PluginSettings.h - public foundation header for TMPluginScreenshot.
//
// This is the ONLY header that pulls in the Windows / COM / WIC system headers.
// Every other header/source in this project must include this file first and
// must NOT re-define WIN32_LEAN_AND_MEAN / NOMINMAX (it would raise C4005).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <shlobj.h>
// wincodec.h depends on these COM/WIC prerequisite headers; do not rely on
// windows.h to bring them in.
#include <objbase.h>
#include <oleauto.h>
#include <ocidl.h>
#include <wincodec.h>

#include <cstdarg>
#include <cwchar>
#include <string>
#include <vector>
#include <atomic>

// Static import libraries.  Keeping them here means both build.bat and
// CMakeLists.txt stay short and cannot drift apart.
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// Error / result codes.
//   0        success
//   1..9     silent results (cancel / busy / debounce / options-open)
//   >=0x8000 hard errors, must always be reported to the user
// ---------------------------------------------------------------------------
enum SnapCode : int
{
    kOk = 0,
    kCancelled = 1,
    kIgnoredBusy = 2,
    kIgnoredDebounce = 3,
    kIgnoredOptionsOpen = 4,

    kErrCaptureDC = 0x8001,
    kErrBitBlt = 0x8002,
    kErrWindowNotFound = 0x8003,
    kErrTooLarge = 0x8004,
    kErrBlackFrame = 0x8005,
    kErrWicFactory = 0x8006,
    kErrWicEncoder = 0x8007,
    kErrWrite = 0x8008,
    kErrDirNotWritable = 0x8009,
    kErrDirCreate = 0x800A,
    kErrClipboard = 0x800B,
    kErrHotkeyConflict = 0x800C,
    kErrNoMemory = 0x800D,
    kErrInternal = 0x800E
};

// Custom window message ids (shared by TMPlugin.cpp and HotkeyManager.cpp,
// therefore declared in this foundation header instead of TMPlugin.h so that
// HotkeyManager never has to include TMPlugin.h).
const UINT WM_SNAP_JOB = WM_APP + 0x510;
const UINT WM_SNAP_RESULT = WM_APP + 0x511;
const UINT WM_SNAP_APPLY_HOTKEY = WM_APP + 0x512;
const UINT WM_SNAP_HOTKEY_RESULT = WM_APP + 0x513;
const UINT WM_SNAP_TEST_RESULT = WM_APP + 0x514;
const UINT WM_SNAP_HOTKEY_TRIGGER = WM_APP + 0x515;

// Window class names (see ARCHITECTURE 3.7).
#define TMSNAP_SINK_CLASS L"TMPluginScreenshotSink"
#define TMSNAP_HOTKEY_CLASS L"TMPluginScreenshotHotkey"
#define TMSNAP_OVERLAY_CLASS L"TMPluginScreenshotOverlay"
#define TMSNAP_WORKER_CLASS L"TMPluginScreenshotWorker"
#define TMSNAP_PICKER_CLASS L"TMPluginScreenshotPicker"

// HINSTANCE of this DLL, captured in DllMain(DLL_PROCESS_ATTACH).
extern HINSTANCE g_hDllInstance;

namespace snap
{

// ---------------------------------------------------------------------------
// Guard thresholds (see TASKS 3.7).
// ---------------------------------------------------------------------------
const int kMaxImageSide = 32767;
const SIZE_T kMaxImageBytes = static_cast<SIZE_T>(256) * 1024 * 1024;
const SIZE_T kMaxOverlayBytes = static_cast<SIZE_T>(160) * 1024 * 1024;
const int kDebounceMs = 300;
const int kHotkeyId = 1;
const int kHotkeyId2 = 2;
const UINT kDefaultHotkeyMods = MOD_CONTROL | MOD_ALT;
const UINT kDefaultHotkeyVk = static_cast<UINT>('A');
const int kMinSelectionSide = 4;
const UINT kLogRollBytes = 256 * 1024;

enum class LogLevel
{
    Info = 0,
    Error = 1
};

// ---------------------------------------------------------------------------
// Logging.  All log entry points are noexcept and swallow every failure.
// ---------------------------------------------------------------------------
void SetLogDirectory(const std::wstring& dir);
void LogMessage(LogLevel level, const wchar_t* fmt, ...) noexcept;

// ---------------------------------------------------------------------------
// String / path helpers.
// ---------------------------------------------------------------------------
std::wstring FormatString(const wchar_t* fmt, ...);
std::wstring Trim(const std::wstring& s);
std::wstring ToLowerAscii(const std::wstring& s);
bool EqualsNoCase(const std::wstring& a, const wchar_t* b);
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);
std::wstring GetDirectoryPart(const std::wstring& path);
std::wstring GetFileNamePart(const std::wstring& path);
std::wstring GetModuleDirectory();
std::wstring GetPicturesDirectory();
std::wstring ExpandEnv(const std::wstring& text);
bool DirectoryExists(const std::wstring& dir);
bool EnsureDirectory(const std::wstring& dir);
bool IsDirectoryWritable(const std::wstring& dir);
std::wstring GetTimeText(const SYSTEMTIME& st);

// ---------------------------------------------------------------------------
// Screen / desktop helpers.
// ---------------------------------------------------------------------------
RECT VirtualScreenRect();
bool IsSecureDesktop();
void SetThreadDpiAwareness();
UINT GetDpiForHwndLazy(HWND hwnd);
int GetSystemDpiLazy();

} // namespace snap

// Convenience log macros.  `fmt` must always be a wide string literal.
#define SNAP_LOG(fmt, ...) ::snap::LogMessage(::snap::LogLevel::Info, fmt, ##__VA_ARGS__)
#define SNAP_LOGE(code, fmt, ...) \
    ::snap::LogMessage(::snap::LogLevel::Error, L"(0x%08X) " fmt, static_cast<unsigned int>(code), ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Settings data model.
// ---------------------------------------------------------------------------
enum class ImageFormat
{
    Png = 0,
    Jpg = 1,
    Bmp = 2
};

const wchar_t* ImageFormatExtension(ImageFormat format);
const wchar_t* ImageFormatDisplayName(ImageFormat format);

// 窗口选择器：活动窗口截图模式下的前置选择步骤（v1.2.0）。
struct WindowPickEntry
{
    HWND hwnd{nullptr};
    std::wstring title;
    std::wstring className;
    HICON icon{nullptr};
    bool ownedIcon{false};
};

struct HotkeyBinding
{
    UINT modifiers{MOD_CONTROL | MOD_ALT};
    UINT vk{'A'};

    bool IsValid() const;
    std::wstring ToText() const;

    static HotkeyBinding Default();
    // Parses "ctrl+alt+shift+win" style modifier text plus a VK number.
    static bool FromIni(const std::wstring& modsText, const std::wstring& vkText, HotkeyBinding& out);
    // Renders the modifier part back to the ini representation.
    std::wstring ModifiersToIniText() const;
};

// 热键槽位：每个槽位 = 开关 + 按键 + 动作。
// action 取值：0=全屏 1=区域 2=活动窗口 3=延时（延时使用 delay_seconds 设置值）。
struct HotkeySlot
{
    bool enabled{false};
    HotkeyBinding binding;
    int action{0};

    bool IsValidAction() const { return action >= 0 && action <= 3; }
};

struct PluginSettings
{
    std::wstring saveDir;
    ImageFormat format{ImageFormat::Png};
    int jpgQuality{90};
    std::wstring fileNameTemplate;
    bool copyToClipboard{true};
    bool includeCursor{false};
    // 热键 1：兼容旧版 hotkey_enabled/hotkey_mods/hotkey_vk 三个 ini 键（默认启用、Ctrl+Alt+A、全屏）。
    // 热键 2：默认关闭、Ctrl+Alt+R、区域截图。
    HotkeySlot hotkey1{true, HotkeyBinding::Default(), 0};
    HotkeySlot hotkey2{false, HotkeyBinding{MOD_CONTROL | MOD_ALT, 'R'}, 1};
    int delaySeconds{0};
    bool fallbackToPicturesRoot{true};
    std::wstring lastSavedPath;
};

class CPluginSettings
{
public:
    CPluginSettings();
    ~CPluginSettings();

    // Resolves the config directory using the 3-level fallback chain and loads
    // the ini file.  Never fails: on error the object keeps in-memory defaults.
    bool Initialize(const std::wstring& hostDir, const std::wstring& extendedDir);

    bool Load();
    bool Save();
    bool SaveLastSavedPath(const std::wstring& path);

    void ResetToDefault();
    void ApplyDefaults(PluginSettings& out) const;

    std::wstring EffectiveSaveDir() const;
    std::wstring BuildFileName(const SYSTEMTIME& st) const;
    std::wstring ExpandTemplate(const SYSTEMTIME& st) const;
    std::wstring ExpandTemplateFor(const std::wstring& tmpl, const SYSTEMTIME& st) const;
    bool EnsureSaveDir(std::wstring& outErr) const;

    // Dialog-side validation; returns false with a human readable reason.
    bool ValidateSettings(PluginSettings& io, std::wstring& outErr) const;

    static std::wstring DefaultTemplate();
    static std::wstring DefaultSaveDir();
    static std::wstring SanitizeFileName(const std::wstring& name);

    bool IsPersistent() const { return m_persistent; }
    const std::wstring& ConfigDir() const { return m_configDir; }
    const std::wstring& IniPath() const { return m_iniPath; }

    PluginSettings data;

private:
    bool ResolveConfigDir(const std::wstring& hostDir, const std::wstring& extendedDir);
    bool EnsureIniUnicode();

    std::wstring m_configDir;
    std::wstring m_iniPath;
    bool m_persistent{false};
};

// ---------------------------------------------------------------------------
// Localisation.
// ---------------------------------------------------------------------------
enum class StringId : int
{
    kPluginName = 0,
    kPluginDescription,
    kPluginAuthor,
    kPluginCopyright,
    kPluginVersion,
    kPluginUrl,

    kItemName,

    kCmdFullScreen,
    kCmdRegion,
    kCmdActiveWindow,
    kCmdDelay,
    kCmdOpenFolder,
    kCmdSettings,

    kTipIdle,
    kTipLast,
    kTipHotkeyDisabled,

    kNotifySaved,
    kNotifySavedWithDetail,
    kNotifyFailed,
    kNotifyClipboardFailed,
    kNotifyHotkeyConflict,
    kNotifyHotkeyEnabled,
    kNotifyHotkeyDisabled,
    kNotifyFolderMissing,
    kNotifyTestOk,
    kNotifyTestFailed,

    kCodeOk,
    kCodeCancelled,
    kCodeBusy,
    kCodeDebounce,
    kCodeOptionsOpen,
    kCodeErrCaptureDC,
    kCodeErrBitBlt,
    kCodeErrWindowNotFound,
    kCodeErrTooLarge,
    kCodeErrBlackFrame,
    kCodeErrWicFactory,
    kCodeErrWicEncoder,
    kCodeErrWrite,
    kCodeErrDirNotWritable,
    kCodeErrDirCreate,
    kCodeErrClipboard,
    kCodeErrHotkeyConflict,
    kCodeErrNoMemory,
    kCodeErrInternal,

    kDlgTitle,
    kGrpSave,
    kGrpCapture,
    kGrpAbout,
    kLblSaveDir,
    kBtnBrowse,
    kBtnOpenDir,
    kLblFormat,
    kLblQuality,
    kLblTemplate,
    kLblExample,
    kChkClipboard,
    kLblDelay,
    kChkCursor,
    kChkHotkeyEnabled,
    kLblHotkey,
    kBtnTest,
    kBtnOk,
    kBtnCancel,
    kBtnReset,
    kAboutText,
    kHotkeyPress,
    kMsgNeedModifier,
    kMsgHotkeyConflict,
    kMsgDirNotWritable,
    kMsgTemplateFallback,
    kMsgSaveFailed,
    kMsgStatusReady,
    kMsgDirMissing,
    kMsgTesting,

    kChkHotkey2Enabled,
    kLblAction,
    kMsgHotkeyDuplicate,

    kPickTitle,

    kCount
};

class CUxText
{
public:
    CUxText();

    void Init(unsigned short hostLangId);
    bool IsChinese() const { return m_chinese; }
    unsigned short LanguageId() const { return m_langId; }

    const wchar_t* Str(StringId id) const;
    const wchar_t* SnapCodeText(SnapCode code) const;

private:
    unsigned short m_langId{2052};
    bool m_chinese{true};
};
