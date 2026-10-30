// CaptureEngine.h - screen capture primitives (full screen / region / window).
#pragma once

#include "PluginSettings.h"

enum class CaptureMode
{
    FullScreen = 0,
    Region = 1,
    ActiveWindow = 2
};

enum class CaptureSource
{
    ItemClick = 0,
    Command = 1,
    Hotkey = 2,
    OptionsTest = 3
};

struct CaptureRequest
{
    CaptureMode mode{CaptureMode::FullScreen};
    int delaySeconds{0};
    bool includeCursor{false};
    CaptureSource source{CaptureSource::Command};
    RECT region{};
};

// RAII-ish GDI frame.  There is deliberately NO destructor: the object is
// passed around by value (returned from the engine) and Reset() must be called
// explicitly on the thread that created it.
struct CapturedFrame
{
    HBITMAP hBitmap{nullptr};
    HDC hMemDC{nullptr};
    void* bits{nullptr}; // 32bpp BGRA, top-down (biHeight < 0)
    int width{0};
    int height{0};
    int stride{0};

    bool IsValid() const { return hBitmap != nullptr && hMemDC != nullptr && bits != nullptr && width > 0 && height > 0; }
    void Reset();
};

struct SessionResult
{
    SnapCode code{kOk};
    std::wstring path;   // empty when nothing was written
    std::wstring detail; // failure reason / HRESULT / extra note
    int width{0};
    int height{0};
    bool clipboardOk{false};
};

class CCaptureEngine
{
public:
    CCaptureEngine();
    ~CCaptureEngine();

    CCaptureEngine(const CCaptureEngine&) = delete;
    CCaptureEngine& operator=(const CCaptureEngine&) = delete;

    CapturedFrame CaptureFullScreen(bool includeCursor, SnapCode& outCode);
    CapturedFrame CaptureRegion(const RECT& rect, bool includeCursor, SnapCode& outCode);
    CapturedFrame CaptureActiveWindow(bool includeCursor, SnapCode& outCode, std::wstring& outErr);
    // 截取指定窗口（窗口选择器的落点）；调用方负责目标窗口的合法性校验。
    CapturedFrame CaptureWindow(HWND target, bool includeCursor, SnapCode& outCode, std::wstring& outErr);

    static RECT VirtualScreenRect();
    static bool IsSecureDesktop();

private:
    void EnsureDpiContext();
    CapturedFrame CaptureRect(const RECT& rect, bool includeCursor, SnapCode& outCode, std::wstring* outErr);
    static void DrawCursorOnFrame(HDC memDC, int originX, int originY, int width, int height);
    static bool LooksAllBlack(const CapturedFrame& frame);

    bool m_dpiCtxApplied{false};
};
