// tools/smoketest.cpp - throw-away smoke test for TMPluginScreenshot.dll.
// Builds a standalone console EXE that LoadLibrary's the plugin, exercises the
// whole ITMPlugin contract, then drives one real full screen capture through a
// mock ITrafficMonitor and verifies the PNG dimensions.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "PluginInterface.h"

namespace
{

int g_failures = 0;

void Print(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

void Check(bool condition, const char* what)
{
    Print("  [%s] %s", condition ? "PASS" : "FAIL", what);
    if (!condition)
    {
        ++g_failures;
    }
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
    if (pos == std::wstring::npos)
    {
        return L".";
    }
    return path.substr(0, pos);
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

bool ReadPngSize(const std::wstring& path, int& width, int& height)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    unsigned char buffer[33] = {0};
    DWORD read = 0;
    ReadFile(file, buffer, 33, &read, nullptr);
    CloseHandle(file);
    if (read < 33)
    {
        return false;
    }
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(buffer, signature, 8) != 0 || memcmp(buffer + 12, "IHDR", 4) != 0)
    {
        return false;
    }
    width = static_cast<int>((static_cast<unsigned>(buffer[16]) << 24) |
                             (static_cast<unsigned>(buffer[17]) << 16) |
                             (static_cast<unsigned>(buffer[18]) << 8) | static_cast<unsigned>(buffer[19]));
    height = static_cast<int>((static_cast<unsigned>(buffer[20]) << 24) |
                              (static_cast<unsigned>(buffer[21]) << 16) |
                              (static_cast<unsigned>(buffer[22]) << 8) | static_cast<unsigned>(buffer[23]));
    return true;
}

class CMockApp : public ITrafficMonitor
{
public:
    explicit CMockApp(const std::wstring& configDir) : m_configDir(configDir) {}

    int GetAPIVersion() override { return 1; }
    const wchar_t* GetVersion() override { return L"smoketest"; }
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
        Print("  [notify] %s", Narrow(strMsg).c_str());
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

int CountFiles(const std::wstring& directory, const std::wstring& prefix)
{
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = directory + L"\\" + prefix + L"*";
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

void DeleteMatching(const std::wstring& directory, const std::wstring& prefix)
{
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = directory + L"\\" + prefix + L"*";
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

bool ReadBmpSize(const std::wstring& path, int& width, int& height)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    unsigned char header[54] = {0};
    DWORD read = 0;
    ReadFile(file, header, 54, &read, nullptr);
    CloseHandle(file);
    if (read < 54 || header[0] != 'B' || header[1] != 'M')
    {
        return false;
    }
    int w = 0;
    int h = 0;
    memcpy(&w, header + 18, 4);
    memcpy(&h, header + 22, 4);
    width = w;
    height = h < 0 ? -h : h;
    return true;
}

// --- helper that drives the region selector overlay -------------------------
bool SaveWindowToBmp(HWND hwnd, const wchar_t* path, bool fromScreen);

struct RegionWatch
{
    DWORD timeoutMs;
    bool cancel;
    const wchar_t* shotPath;
    volatile LONG found;
};

DWORD WINAPI RegionWatchThread(LPVOID param)
{
    RegionWatch* watch = static_cast<RegionWatch*>(param);
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(watch->timeoutMs);
    while (GetTickCount64() < deadline)
    {
        HWND overlay = FindWindowW(L"TMPluginScreenshotOverlay", nullptr);
        if (overlay != nullptr)
        {
            InterlockedExchange(&watch->found, 1);
            Sleep(120);
            if (watch->cancel)
            {
                PostMessageW(overlay, WM_KEYDOWN, VK_ESCAPE, 0);
                return 0;
            }

            PostMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
            Sleep(40);
            for (int step = 1; step <= 6; ++step)
            {
                const int x = 100 + (300 * step) / 6;
                const int y = 100 + (200 * step) / 6;
                PostMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
                Sleep(25);
            }
            Sleep(150);
            if (watch->shotPath != nullptr)
            {
                SaveWindowToBmp(overlay, watch->shotPath, true);
            }
            PostMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(400, 300));
            return 0;
        }
        Sleep(30);
    }
    return 0;
}

bool ReadJpegSize(const std::wstring& path, int& width, int& height)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 4 || size.QuadPart > 64 * 1024 * 1024)
    {
        CloseHandle(file);
        return false;
    }
    std::vector<unsigned char> data(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);
    if (read < 4 || data[0] != 0xFF || data[1] != 0xD8)
    {
        return false;
    }
    size_t i = 2;
    while (i + 9 < read)
    {
        if (data[i] != 0xFF)
        {
            ++i;
            continue;
        }
        const unsigned char marker = data[i + 1];
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
        {
            i += 2;
            continue;
        }
        const unsigned segmentLength = (static_cast<unsigned>(data[i + 2]) << 8) | data[i + 3];
        if (segmentLength < 2)
        {
            return false;
        }
        const bool isStartOfFrame =
            (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (isStartOfFrame)
        {
            height = (static_cast<int>(data[i + 5]) << 8) | data[i + 6];
            width = (static_cast<int>(data[i + 7]) << 8) | data[i + 8];
            return width > 0 && height > 0;
        }
        i += 2 + segmentLength;
    }
    return false;
}

bool ReadImageSize(const std::wstring& path, int& width, int& height)
{
    return ReadPngSize(path, width, height) || ReadBmpSize(path, width, height) ||
           ReadJpegSize(path, width, height);
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
        Sleep(20);
    }
}

// --- helper that opens the plugin settings dialog and closes it again -------
struct DialogWatch
{
    DWORD timeoutMs;
    int closeWith;              // 0 = WM_CLOSE, 1 = IDOK
    const wchar_t* bmpPath;     // optional: screenshot the dialog before closing
    volatile LONG found;
};

bool SaveWindowToBmp(HWND hwnd, const wchar_t* path, bool fromScreen = false)
{
    RECT rect;
    if (GetWindowRect(hwnd, &rect) == FALSE)
    {
        return false;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr)
    {
        if (dib != nullptr)
        {
            DeleteObject(dib);
        }
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return false;
    }
    HGDIOBJ old = SelectObject(memDC, dib);
    // A layered overlay must be grabbed from the screen; a normal dialog can be
    // asked to render itself even when it is partly covered.
    BOOL rendered = FALSE;
    if (!fromScreen)
    {
        rendered = PrintWindow(hwnd, memDC, 2 /* PW_RENDERFULLCONTENT */);
    }
    if (rendered == FALSE)
    {
        BitBlt(memDC, 0, 0, width, height, screenDC, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    }
    SelectObject(memDC, old);

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    memcpy(pixels.data(), bits, pixels.size());

    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    const int rowBytes = ((width * 3) + 3) & ~3;
    const DWORD imageBytes = static_cast<DWORD>(rowBytes) * static_cast<DWORD>(height);
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER infoHeader{};
    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = width;
    infoHeader.biHeight = height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 24;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = imageBytes;
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + imageBytes;

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0;
    WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr);
    WriteFile(file, &infoHeader, sizeof(infoHeader), &written, nullptr);

    std::vector<unsigned char> row(static_cast<size_t>(rowBytes), 0);
    const unsigned char* source = pixels.data();
    for (int y = height - 1; y >= 0; --y)
    {
        const unsigned char* srcRow = source + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        for (int x = 0; x < width; ++x)
        {
            row[x * 3 + 0] = srcRow[x * 4 + 0];
            row[x * 3 + 1] = srcRow[x * 4 + 1];
            row[x * 3 + 2] = srcRow[x * 4 + 2];
        }
        WriteFile(file, row.data(), static_cast<DWORD>(rowBytes), &written, nullptr);
    }
    CloseHandle(file);
    return true;
}

HWND g_dialogHandle = nullptr;

BOOL CALLBACK FindDialogProc(HWND hwnd, LPARAM param)
{
    (void)param;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
    {
        return TRUE;
    }
    wchar_t className[64] = {0};
    GetClassNameW(hwnd, className, static_cast<int>(_countof(className)));
    if (wcscmp(className, L"#32770") != 0)
    {
        return TRUE;
    }
    if (IsWindowVisible(hwnd) == FALSE)
    {
        return TRUE;
    }
    g_dialogHandle = hwnd;
    return FALSE;
}

DWORD WINAPI DialogWatchThread(LPVOID param)
{
    DialogWatch* watch = static_cast<DialogWatch*>(param);
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(watch->timeoutMs);
    while (GetTickCount64() < deadline)
    {
        g_dialogHandle = nullptr;
        EnumWindows(&FindDialogProc, 0);
        if (g_dialogHandle != nullptr)
        {
            InterlockedExchange(&watch->found, 1);
            if (watch->bmpPath != nullptr)
            {
                Sleep(400);
                SaveWindowToBmp(g_dialogHandle, watch->bmpPath);
                PostMessageW(g_dialogHandle, WM_CLOSE, 0, 0);
                return 0;
            }
            if (watch->closeWith == 1)
            {
                PostMessageW(g_dialogHandle, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
            }
            else
            {
                PostMessageW(g_dialogHandle, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        Sleep(50);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Print("=== TMPluginScreenshot smoke test ===");
    const char* formatArg = (argc > 1) ? argv[1] : "png";
    const wchar_t* formatWide = L"png";
    const wchar_t* extension = L".png";
    if (_stricmp(formatArg, "jpg") == 0)
    {
        formatWide = L"jpg";
        extension = L".jpg";
    }
    else if (_stricmp(formatArg, "bmp") == 0)
    {
        formatWide = L"bmp";
        extension = L".bmp";
    }
    Print("format under test: %s", formatArg);

    const std::wstring exeDir = ExeDirectory();
    const std::wstring rootDir = exeDir + L"\\..";
    const std::wstring dllPath = exeDir + L"\\TMPluginScreenshot.dll";
    const std::wstring outDir = rootDir + L"\\out-test";
    const std::wstring configDir = outDir + L"\\config";

    CreateDirectoryW(outDir.c_str(), nullptr);
    CreateDirectoryW(configDir.c_str(), nullptr);

    // The mock host must outlive every plugin call (the plugin keeps the
    // ITrafficMonitor* it receives in OnInitialize).
    CMockApp mock(configDir);

    Print("dll: %s", Narrow(dllPath.c_str()).c_str());

    HMODULE module = LoadLibraryW(dllPath.c_str());
    if (module == nullptr)
    {
        Print("FATAL: LoadLibraryW failed, GetLastError=%lu", static_cast<unsigned long>(GetLastError()));
        return 1;
    }
    Print("LoadLibraryW .......... OK");

    typedef ITMPlugin* (*GetInstanceFn)();
    GetInstanceFn getInstance = reinterpret_cast<GetInstanceFn>(
        GetProcAddress(module, "TMPluginGetInstance"));
    if (getInstance == nullptr)
    {
        Print("FATAL: GetProcAddress(TMPluginGetInstance) failed");
        return 1;
    }
    Print("GetProcAddress ........ OK");

    ITMPlugin* plugin = getInstance();
    if (plugin == nullptr)
    {
        Print("FATAL: TMPluginGetInstance returned nullptr");
        return 1;
    }

    Print("");
    Print("--- A. API contract (before OnInitialize) ---");
    Print("  GetAPIVersion()            = %d", plugin->GetAPIVersion());
    Check(plugin->GetAPIVersion() == 8, "GetAPIVersion() == 8");

    static const char* kInfoNames[] = {"NAME", "DESCRIPTION", "AUTHOR", "COPYRIGHT", "VERSION", "URL"};
    for (int i = 0; i < 6; ++i)
    {
        const wchar_t* info = plugin->GetInfo(static_cast<ITMPlugin::PluginInfoIndex>(i));
        Print("  GetInfo(%-11s) = %s", kInfoNames[i], Narrow(info).c_str());
        Check(info != nullptr && info[0] != L'\0', "GetInfo field is non null and non empty");
    }
    {
        const wchar_t* beyond = plugin->GetInfo(static_cast<ITMPlugin::PluginInfoIndex>(99));
        Check(beyond != nullptr && beyond[0] == L'\0', "GetInfo(99) returns L\"\"");
    }

    Check(plugin->GetItem(-1) == nullptr, "GetItem(-1) == nullptr");
    Check(plugin->GetItem(1) == nullptr, "GetItem(1) == nullptr");
    IPluginItem* item = plugin->GetItem(0);
    Check(item != nullptr, "GetItem(0) != nullptr");

    if (item != nullptr)
    {
        Print("  GetItemName()              = %s", Narrow(item->GetItemName()).c_str());
        Print("  GetItemId()                = %s", Narrow(item->GetItemId()).c_str());
        Print("  GetItemWidth()             = %d", item->GetItemWidth());
        Print("  IsCustomDraw()             = %d", item->IsCustomDraw() ? 1 : 0);
        Print("  GetItemWidthEx(nullptr)    = %d", item->GetItemWidthEx(nullptr));
        Check(item->GetItemName() != nullptr && item->GetItemName()[0] != L'\0', "GetItemName non empty");
        Check(item->GetItemId() != nullptr && wcscmp(item->GetItemId(), L"TMSnap_Capture_Item") == 0,
              "GetItemId == TMSnap_Capture_Item");
        Check(item->GetItemWidth() == 24, "GetItemWidth() == 24");
        Check(item->IsCustomDraw(), "IsCustomDraw() == true");
        Check(item->GetItemLableText() != nullptr, "GetItemLableText() != nullptr");
        Check(item->GetItemValueText() != nullptr, "GetItemValueText() != nullptr");
        Check(item->GetItemValueSampleText() != nullptr, "GetItemValueSampleText() != nullptr");
    }

    Print("  GetCommandCount()          = %d", plugin->GetCommandCount());
    Check(plugin->GetCommandCount() == 6, "GetCommandCount() == 6");
    for (int i = 0; i < plugin->GetCommandCount(); ++i)
    {
        const wchar_t* name = plugin->GetCommandName(i);
        Print("  GetCommandName(%d)          = %s", i, Narrow(name).c_str());
        Check(name != nullptr && name[0] != L'\0', "command name non empty");
        void* icon = plugin->GetCommandIcon(i);
        Print("  GetCommandIcon(%d)          = %p", i, icon);
    }
    {
        const wchar_t* outOfRange = plugin->GetCommandName(99);
        Print("  GetCommandName(99)         = %s", Narrow(outOfRange).c_str());
        Check(outOfRange == nullptr || outOfRange[0] == L'\0', "GetCommandName(99) is L\"\" or nullptr");
    }
    Check(plugin->GetCommandName(-1) != nullptr || plugin->GetCommandName(-1) == nullptr,
          "GetCommandName(-1) did not crash");

    plugin->DataRequired();
    Check(true, "DataRequired() did not crash");
    Print("  GetTooltipInfo()           = %s", Narrow(plugin->GetTooltipInfo()).c_str());
    Print("  GetPluginIcon()            = %p", plugin->GetPluginIcon());

    if (item != nullptr)
    {
        const int rightClick = item->OnMouseEvent(IPluginItem::MT_RCLICKED, 5, 5, nullptr, 0);
        Print("  OnMouseEvent(MT_RCLICKED)  = %d", rightClick);
        Check(rightClick == 0, "OnMouseEvent(MT_RCLICKED) == 0 (host must show its menu)");
    }

    Print("");
    Print("--- B. real full screen capture through a mock host ---");
    {
        SYSTEMTIME now;
        GetLocalTime(&now);
        std::wstring ini;
        ini += L"[TMSnap]\r\n";
        ini += L"save_dir=" + outDir + L"\r\n";
        ini += L"format=";
        ini += formatWide;
        ini += L"\r\n";
        ini += L"jpg_quality=90\r\n";
        ini += L"file_name_template=verify_fullscreen\r\n";
        ini += L"copy_to_clipboard=1\r\n";
        ini += L"include_cursor=0\r\n";
        ini += L"hotkey_enabled=1\r\n";
        ini += L"hotkey_mods=ctrl+alt\r\n";
        ini += L"hotkey_vk=65\r\n";
        ini += L"delay_seconds=0\r\n";
        ini += L"fallback_pictures_root=0\r\n";
        const std::wstring iniPath = configDir + L"\\TMPluginScreenshot.ini";
        DeleteFileW(iniPath.c_str());
        Check(WriteUtf16File(iniPath, ini), "wrote UTF-16LE ini");

        plugin->OnInitialize(&mock);
        PumpMessages(200);

        Print("  tooltip after init         = %s", Narrow(plugin->GetTooltipInfo()).c_str());
        {
            // The idle tooltip only shows the binding when RegisterHotKey worked.
            const std::string idleTooltip = Narrow(plugin->GetTooltipInfo());
            Check(idleTooltip.find("Ctrl+Alt") != std::string::npos,
                  "RegisterHotKey succeeded (idle tooltip shows Ctrl+Alt+A)");
        }

        DeleteMatching(outDir, L"verify_fullscreen");
        plugin->OnPluginCommand(0, nullptr, nullptr);
        PumpMessages(3000);

        const std::wstring shot = outDir + L"\\verify_fullscreen" + extension;
        const DWORD attributes = GetFileAttributesW(shot.c_str());
        const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
        Print("  expected first file        = %s", Narrow(shot.c_str()).c_str());
        Check(exists, "full screen image file was created");

        const int expectedWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int expectedHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        Print("  SM_CXVIRTUALSCREEN x SM_CYVIRTUALSCREEN = %d x %d", expectedWidth, expectedHeight);
        if (exists && extension[1] == L'p')
        {
            int width = 0;
            int height = 0;
            const bool sized = ReadPngSize(shot, width, height);
            Print("  png size                                = %d x %d", width, height);
            Check(sized, "PNG header parsed");
            Check(width == expectedWidth && height == expectedHeight, "PNG size == virtual screen size");
        }
        if (exists)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(shot.c_str(), GetFileExInfoStandard, &data))
            {
                LARGE_INTEGER size;
                size.LowPart = data.nFileSizeLow;
                size.HighPart = data.nFileSizeHigh;
                Print("  file bytes                              = %lld",
                      static_cast<long long>(size.QuadPart));
                Check(size.QuadPart > 1024, "output file is not empty");
            }
        }

        // Second capture right away must not overwrite the first one.
        Sleep(320);
        plugin->OnPluginCommand(0, nullptr, nullptr);
        PumpMessages(3000);
        const int fileCount = CountFiles(outDir, L"verify_fullscreen");
        Print("  files matching verify_fullscreen*       = %d", fileCount);
        Check(fileCount == 2, "second capture produced a second file (no overwrite)");

        // GDI handle soak: repeated captures must not grow the GDI object count.
        const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 6; ++i)
        {
            Sleep(320);
            plugin->OnPluginCommand(0, nullptr, nullptr);
            PumpMessages(700);
        }
        const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        Print("  GDI objects before/after 6 more captures = %lu / %lu",
              static_cast<unsigned long>(gdiBefore), static_cast<unsigned long>(gdiAfter));
        Check(gdiAfter <= gdiBefore + 3, "GDI object count did not grow over 6 captures");
    }

    Print("");
    Print("--- B1. region selection overlay ---");
    {
        const std::wstring overlayShot = outDir + L"\\overlay.bmp";
        const int before = CountFiles(outDir, L"verify_fullscreen");

        RegionWatch watch{6000, false, overlayShot.c_str(), 0};
        HANDLE thread = CreateThread(nullptr, 0, &RegionWatchThread, &watch, 0, nullptr);
        plugin->OnPluginCommand(1, nullptr, nullptr);
        if (thread != nullptr)
        {
            WaitForSingleObject(thread, 8000);
            CloseHandle(thread);
        }
        PumpMessages(2500);

        Check(watch.found == 1, "overlay window appeared");
        Print("  overlay screenshot        = %s", Narrow(overlayShot.c_str()).c_str());

        std::wstring indexText;
        if (before > 0)
        {
            indexText = L"_" + std::to_wstring(before);
        }
        const std::wstring regionFile = outDir + L"\\verify_fullscreen" + indexText + extension;
        Print("  region file               = %s", Narrow(regionFile.c_str()).c_str());
        const int after = CountFiles(outDir, L"verify_fullscreen");
        Check(after == before + 1, "region capture produced exactly one new file");

        int regionWidth = 0;
        int regionHeight = 0;
        const bool regionRead = ReadImageSize(regionFile, regionWidth, regionHeight);
        Print("  region image size         = %d x %d", regionWidth, regionHeight);
        Check(regionRead, "region image is readable");
        Check(regionWidth == 300 && regionHeight == 200, "region image is exactly 300x200 (zero DPI error)");
        Check(FindWindowW(L"TMPluginScreenshotOverlay", nullptr) == nullptr, "no overlay window left behind");

        // Cancel path: Esc must leave no file and no window behind.
        RegionWatch cancelWatch{6000, true, nullptr, 0};
        thread = CreateThread(nullptr, 0, &RegionWatchThread, &cancelWatch, 0, nullptr);
        plugin->OnPluginCommand(1, nullptr, nullptr);
        if (thread != nullptr)
        {
            WaitForSingleObject(thread, 8000);
            CloseHandle(thread);
        }
        PumpMessages(1500);
        Check(cancelWatch.found == 1, "overlay appeared for the cancel run");
        const int afterCancel = CountFiles(outDir, L"verify_fullscreen");
        Print("  files before/after Esc    = %d / %d", after, afterCancel);
        Check(afterCancel == after, "Esc cancel produced no file");
        Check(FindWindowW(L"TMPluginScreenshotOverlay", nullptr) == nullptr,
              "no overlay window left behind after cancel");
    }

    Print("");
    Print("--- B3. global hotkey (registered on the host UI thread) ---");
    {
        // 热键现在注册在 sink 窗口（宿主 UI 线程）上，不再有独立热键线程/窗口。
        HWND sinkWindow = FindWindowW(L"TMPluginScreenshotSink", nullptr);
        Print("  sink message window       = %p", sinkWindow);
        Check(sinkWindow != nullptr, "sink message window exists");

        const std::string idleTooltip = Narrow(plugin->GetTooltipInfo());
        Print("  tooltip (now shows last)  = %s", idleTooltip.c_str());

        const int before = CountFiles(outDir, L"verify_fullscreen");
        if (sinkWindow != nullptr)
        {
            PostMessageW(sinkWindow, WM_HOTKEY, 1, 0);
        }
        PumpMessages(2500);
        const int after = CountFiles(outDir, L"verify_fullscreen");
        Print("  files before/after hotkey = %d / %d", before, after);
        Check(after == before + 1, "WM_HOTKEY produced exactly one new file");
    }

    Print("");
    Print("--- B2. settings dialog (in-memory DLGTEMPLATE) ---");
    {
        const std::wstring dialogShot = outDir + L"\\dialog.bmp";
        DialogWatch watchShot{4000, 0, dialogShot.c_str(), 0};
        HANDLE thread = CreateThread(nullptr, 0, &DialogWatchThread, &watchShot, 0, nullptr);
        plugin->ShowOptionsDialog(nullptr);
        if (thread != nullptr)
        {
            WaitForSingleObject(thread, 6000);
            CloseHandle(thread);
        }
        Check(watchShot.found == 1, "settings dialog screenshot taken");
        Print("  dialog screenshot           = %s", Narrow(dialogShot.c_str()).c_str());

        DialogWatch watchCancel{4000, 0, nullptr, 0};
        thread = CreateThread(nullptr, 0, &DialogWatchThread, &watchCancel, 0, nullptr);
        ITMPlugin::OptionReturn resultCancel = plugin->ShowOptionsDialog(nullptr);
        if (thread != nullptr)
        {
            WaitForSingleObject(thread, 5000);
            CloseHandle(thread);
        }
        Print("  dialog created (cancel run) = %d", watchCancel.found ? 1 : 0);
        Check(watchCancel.found == 1, "settings dialog was created (DLGTEMPLATE is valid)");
        Print("  ShowOptionsDialog(cancel)   = %d", static_cast<int>(resultCancel));
        Check(resultCancel == ITMPlugin::OR_OPTION_UNCHANGED, "cancel returns OR_OPTION_UNCHANGED");

        DialogWatch watchOk{4000, 1, nullptr, 0};
        thread = CreateThread(nullptr, 0, &DialogWatchThread, &watchOk, 0, nullptr);
        ITMPlugin::OptionReturn resultOk = plugin->ShowOptionsDialog(nullptr);
        if (thread != nullptr)
        {
            WaitForSingleObject(thread, 5000);
            CloseHandle(thread);
        }
        Print("  dialog created (ok run)     = %d", watchOk.found ? 1 : 0);
        Check(watchOk.found == 1, "settings dialog reopened");
        Print("  ShowOptionsDialog(ok)       = %d", static_cast<int>(resultOk));
        Check(resultOk == ITMPlugin::OR_OPTION_CHANGED, "OK returns OR_OPTION_CHANGED");
    }

    Print("");
    Print("--- C. unload (DllMain DETACH path) ---");
    FreeLibrary(module);
    Print("FreeLibrary ........... OK");

    Print("");
    if (g_failures == 0)
    {
        Print("RESULT: ALL CHECKS PASSED");
        return 0;
    }
    Print("RESULT: %d CHECK(S) FAILED", g_failures);
    return 2;
}
