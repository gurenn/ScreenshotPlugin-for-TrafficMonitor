// TMPlugin.cpp - plugin object, display item, capture session, DLL entry points.
#include "TMPlugin.h"

#include "OptionsDialog.h"
#include "RegionSelector.h"
#include "WindowPicker.h"

#include <cstring>
#include <new>

namespace
{

const COLORREF kIconColor = RGB(70, 130, 190);

const StringId kCommandStringIds[kCommandCount] = {
    StringId::kCmdFullScreen,  StringId::kCmdRegion,     StringId::kCmdActiveWindow,
    StringId::kCmdDelay,       StringId::kCmdOpenFolder, StringId::kCmdSettings};

const int kCommandGlyphs[kCommandCount] = {kGlyphMonitor, kGlyphCrop, kGlyphWindow,
                                           kGlyphClock,   kGlyphFolder, kGlyphGear};

HICON CreateGlyphIcon(int size, int shape, COLORREF color)
{
    if (size < 8 || size > 64)
    {
        return nullptr;
    }

    HDC screenDC = GetDC(nullptr);
    if (screenDC == nullptr)
    {
        return nullptr;
    }
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDC);

    if (memDC == nullptr || dib == nullptr || bits == nullptr)
    {
        if (dib != nullptr)
        {
            DeleteObject(dib);
        }
        if (memDC != nullptr)
        {
            DeleteDC(memDC);
        }
        return nullptr;
    }

    HGDIOBJ oldBitmap = SelectObject(memDC, dib);
    memset(bits, 0, static_cast<size_t>(size) * static_cast<size_t>(size) * 4);

    {
        HdcSink sink(memDC);
        DrawShapeGlyph(sink, 1, 1, size - 2, size - 2, static_cast<unsigned long>(color), shape);
    }

    // GDI does not touch the alpha channel; make every painted pixel opaque.
    unsigned char* pixels = static_cast<unsigned char*>(bits);
    const size_t count = static_cast<size_t>(size) * static_cast<size_t>(size);
    for (size_t i = 0; i < count; ++i)
    {
        unsigned char* pixel = pixels + i * 4;
        pixel[3] = ((pixel[0] | pixel[1] | pixel[2]) != 0) ? 255 : 0;
    }

    const size_t maskStride = static_cast<size_t>(((size + 31) / 32) * 4);
    std::vector<BYTE> maskBits(maskStride * static_cast<size_t>(size), 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = dib;
    iconInfo.hbmMask = mask;
    HICON icon = CreateIconIndirect(&iconInfo);

    if (oldBitmap != nullptr)
    {
        SelectObject(memDC, oldBitmap);
    }
    DeleteObject(dib);
    if (mask != nullptr)
    {
        DeleteObject(mask);
    }
    DeleteDC(memDC);
    return icon;
}

CTMPlugin* g_pluginInstance = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// Glyph sinks
// ---------------------------------------------------------------------------
void DrawerSink::Fill(int x, int y, int w, int h, unsigned long color)
{
    if (m_drawer != nullptr)
    {
        m_drawer->FillRect(x, y, w, h, color, 255);
    }
}

void DrawerSink::Frame(int x, int y, int w, int h, unsigned long color, int width, bool dotted, int radius)
{
    if (m_drawer != nullptr)
    {
        m_drawer->DrawRectOutLine(x, y, w, h, color, width, dotted, 255, radius);
    }
}

void DrawerSink::Line(int x1, int y1, int x2, int y2, unsigned long color, int width)
{
    if (m_drawer != nullptr)
    {
        m_drawer->DrawLine(x1, y1, x2, y2, color, width, false, 255);
    }
}

void HdcSink::Fill(int x, int y, int w, int h, unsigned long color)
{
    if (m_dc == nullptr || w <= 0 || h <= 0)
    {
        return;
    }
    RECT rect{x, y, x + w, y + h};
    HBRUSH brush = CreateSolidBrush(static_cast<COLORREF>(color));
    ::FillRect(m_dc, &rect, brush);
    DeleteObject(brush);
}

void HdcSink::Frame(int x, int y, int w, int h, unsigned long color, int width, bool dotted, int radius)
{
    if (m_dc == nullptr || w <= 0 || h <= 0)
    {
        return;
    }
    if (width < 1)
    {
        width = 1;
    }
    HPEN pen = CreatePen(dotted ? PS_DOT : PS_SOLID, width, static_cast<COLORREF>(color));
    HGDIOBJ oldPen = SelectObject(m_dc, pen);
    HGDIOBJ oldBrush = SelectObject(m_dc, GetStockObject(NULL_BRUSH));
    Rectangle(m_dc, x, y, x + w, y + h);
    SelectObject(m_dc, oldBrush);
    SelectObject(m_dc, oldPen);
    DeleteObject(pen);
}

void HdcSink::Line(int x1, int y1, int x2, int y2, unsigned long color, int width)
{
    if (m_dc == nullptr)
    {
        return;
    }
    if (width < 1)
    {
        width = 1;
    }
    HPEN pen = CreatePen(PS_SOLID, width, static_cast<COLORREF>(color));
    HGDIOBJ oldPen = SelectObject(m_dc, pen);
    MoveToEx(m_dc, x1, y1, nullptr);
    LineTo(m_dc, x2, y2);
    SelectObject(m_dc, oldPen);
    DeleteObject(pen);
}

void DrawCameraGlyph(GlyphSink& sink, int x, int y, int w, int h, unsigned long color)
{
    if (w <= 2 || h <= 2)
    {
        return;
    }
    const int stroke = (w >= 20) ? 2 : 1;
    const int bodyTop = y + h / 3;
    const int bodyHeight = h - h / 3 - 1;

    // Body with slightly rounded corners.
    sink.Frame(x + 1, bodyTop, w - 2, bodyHeight, color, stroke, false, 2);
    // Viewfinder hump on top.
    sink.Fill(x + w / 4, y + h / 4, w / 2, h / 8 + 1, color);
    // Lens.
    const int lens = (w >= 20) ? 4 : 2;
    sink.Frame(x + w / 2 - lens, bodyTop + bodyHeight / 2 - lens, lens * 2, lens * 2, color, stroke, false, 0);
}

void DrawShapeGlyph(GlyphSink& sink, int x, int y, int w, int h, unsigned long color, int shape)
{
    if (w <= 2 || h <= 2)
    {
        return;
    }
    const int stroke = (w >= 20) ? 2 : 1;

    switch (shape)
    {
    case kGlyphMonitor:
    {
        const int bodyHeight = (h * 3) / 4;
        sink.Frame(x + 1, y + 1, w - 2, bodyHeight - 1, color, stroke, false, 1);
        sink.Fill(x + w / 2 - 1, y + bodyHeight, 2, h - bodyHeight - 2 > 0 ? h - bodyHeight - 2 : 1, color);
        sink.Fill(x + w / 4, y + h - 2, w / 2, 2, color);
        break;
    }
    case kGlyphCrop:
    {
        sink.Line(x + 3, y, x + 3, y + h - 4, color, stroke);
        sink.Line(x, y + h - 4, x + w - 4, y + h - 4, color, stroke);
        sink.Line(x + 5, y + 4, x + w, y + 4, color, stroke);
        sink.Line(x + w, y + 4, x + w, y + h, color, stroke);
        break;
    }
    case kGlyphWindow:
    {
        sink.Frame(x + 1, y + 1, w - 2, h - 2, color, stroke, false, 0);
        const int titleHeight = (h >= 20) ? 5 : 3;
        sink.Fill(x + 1, y + 1, w - 2, titleHeight, color);
        break;
    }
    case kGlyphClock:
    {
        const int radius = (w - 4) / 2;
        sink.Frame(x + 2, y + 2, w - 4, h - 4, color, stroke, false, radius);
        sink.Line(x + w / 2, y + h / 3, x + w / 2, y + h / 2, color, stroke);
        sink.Line(x + w / 2, y + h / 2, x + (w * 3) / 4, y + h / 2, color, stroke);
        break;
    }
    case kGlyphFolder:
    {
        const int tabHeight = (h >= 20) ? 5 : 3;
        sink.Fill(x + 1, y + h / 3 - tabHeight, w / 2, tabHeight + 1, color);
        sink.Fill(x + 1, y + h / 3, w - 2, h - h / 3 - 1, color);
        break;
    }
    case kGlyphGear:
    {
        sink.Frame(x + 1, y + 1, w - 2, h - 2, color, stroke, false, 2);
        sink.Fill(x + w / 3, y + h / 3, w / 3, h / 3, color);
        sink.Line(x + w / 2, y, x + w / 2, y + h / 5, color, stroke);
        sink.Line(x + w / 2, y + (h * 4) / 5, x + w / 2, y + h, color, stroke);
        sink.Line(x, y + h / 2, x + w / 5, y + h / 2, color, stroke);
        sink.Line(x + (w * 4) / 5, y + h / 2, x + w, y + h / 2, color, stroke);
        break;
    }
    case kGlyphCamera:
    default:
        DrawCameraGlyph(sink, x, y, w, h, color);
        break;
    }
}

// ---------------------------------------------------------------------------
// CScreenshotItem
// ---------------------------------------------------------------------------
CScreenshotItem::CScreenshotItem(CTMPlugin* owner) : m_owner(owner)
{
    // Localised at OnInitialize(); this is only the pre-initialisation fallback.
    m_name = L"\x622A\x56FE"; // 截图
    m_id = L"TMSnap_Capture_Item";
}

const wchar_t* CScreenshotItem::GetItemName() const
{
    return m_name.c_str();
}

const wchar_t* CScreenshotItem::GetItemId() const
{
    return m_id.c_str();
}

const wchar_t* CScreenshotItem::GetItemLableText() const
{
    return L"";
}

const wchar_t* CScreenshotItem::GetItemValueText() const
{
    return L"";
}

const wchar_t* CScreenshotItem::GetItemValueSampleText() const
{
    return L"M";
}

bool CScreenshotItem::IsCustomDraw() const
{
    return true;
}

int CScreenshotItem::GetItemWidth() const
{
    return 24;
}

int CScreenshotItem::GetItemWidthEx(void* hDC) const
{
    int dpi = 96;
    if (hDC != nullptr)
    {
        int value = GetDeviceCaps(static_cast<HDC>(hDC), LOGPIXELSY);
        if (value >= 48 && value <= 960)
        {
            dpi = value;
        }
    }
    return MulDiv(24, dpi, 96);
}

void CScreenshotItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)
{
    if (hDC == nullptr)
    {
        return;
    }
    HdcSink sink(static_cast<HDC>(hDC));
    const unsigned long color = dark_mode ? 0x00E6E6E6UL : 0x002A2A2AUL;
    DrawCameraGlyph(sink, x, y, w, h, color);
}

bool CScreenshotItem::DrawItemEx(IPluginDrawer* pDrawer, int x, int y, int w, int h, bool dark_mode)
{
    if (pDrawer == nullptr)
    {
        return false;
    }
    DrawerSink sink(pDrawer);
    const unsigned long color = dark_mode ? 0x00E6E6E6UL : 0x002A2A2AUL;
    DrawCameraGlyph(sink, x, y, w, h, color);
    return true;
}

int CScreenshotItem::OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag)
{
    (void)x;
    (void)y;
    (void)hWnd;
    (void)flag;

    if (m_owner == nullptr)
    {
        return 0;
    }
    switch (type)
    {
    case MT_LCLICKED:
        m_owner->RequestCapture(CaptureMode::FullScreen, 0, CaptureSource::ItemClick);
        return 1;
    case MT_DBCLICKED:
        // The first click already fired; swallowing this avoids a second file.
        return 1;
    case MT_RCLICKED:
        // Must return 0 so the host shows its own context menu with our commands.
        return 0;
    default:
        return 0;
    }
}

int CScreenshotItem::IsDoubleLineExclusive() const
{
    return 0;
}

// ---------------------------------------------------------------------------
// CScreenshotSession
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
namespace
{
// 一次截图作业的全部上下文：作业线程自己 new、自己 delete。
struct JobContext
{
    CaptureRequest request;
    CScreenshotSession* session{nullptr};
    CPluginSettings* settings{nullptr};
    CUxText* text{nullptr};
    HWND hSink{nullptr};
    HANDLE hStop{nullptr};
};
} // namespace

CScreenshotSession::CScreenshotSession()
{
}

CScreenshotSession::~CScreenshotSession()
{
    Stop();
}

bool CScreenshotSession::Start(CPluginSettings* settings, CUxText* text, HWND hSink)
{
    m_pSettings = settings;
    m_pText = text;
    m_hSink = hSink;

    if (m_hStop == nullptr)
    {
        m_hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (m_hStop == nullptr)
        {
            // 没有取消信号也能工作，只是延时/框选无法被打断。
            SNAP_LOGE(kErrInternal, L"CreateEvent for the capture pipeline failed");
        }
    }
    m_shutdown = false;
    m_started = true;
    return true;
}

void CScreenshotSession::Stop()
{
    // 只发信号，**绝不等待**。DllMain(DLL_PROCESS_DETACH) 持有 loader lock，而线程退出
    // 本身要拿 loader lock，所以在这里 join 必然超时（实测 500+1000ms 超时、放大到 3s+3s
    // 后 FreeLibrary 精确多花 6s）。作业线程会在下一个检查点自行退出。
    // m_hStop 故意**不关闭**：避免正在 WaitForSingleObject 的作业线程拿到无效句柄。
    m_shutdown = true;
    if (m_hStop != nullptr)
    {
        SetEvent(m_hStop);
    }
    m_started = false;
}

void CScreenshotSession::Cancel()
{
    if (m_hStop != nullptr)
    {
        SetEvent(m_hStop);
    }
}

SnapCode CScreenshotSession::Request(const CaptureRequest& request)
{
    if (!m_started || m_shutdown.load())
    {
        return kIgnoredBusy;
    }
    bool expected = false;
    if (!m_busy.compare_exchange_strong(expected, true))
    {
        return kIgnoredBusy;
    }

    const ULONGLONG now = GetTickCount64();
    if (now - m_lastTick < static_cast<ULONGLONG>(snap::kDebounceMs))
    {
        m_busy = false;
        return kIgnoredDebounce;
    }
    m_lastTick = now;

    JobContext* context = new (std::nothrow) JobContext();
    if (context == nullptr)
    {
        m_busy = false;
        return kErrNoMemory;
    }
    context->request = request;
    context->session = this;
    context->settings = m_pSettings;
    context->text = m_pText;
    context->hSink = m_hSink;
    context->hStop = m_hStop;

    if (m_hStop != nullptr)
    {
        ResetEvent(m_hStop);
    }

    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, &CScreenshotSession::JobThreadProc, context, 0, &threadId);
    if (thread == nullptr)
    {
        SNAP_LOGE(kErrInternal, L"CreateThread for the capture job failed");
        delete context;
        m_busy = false;
        return kErrInternal;
    }
    // 不 join、不等待：作业线程跑完自己退出，句柄立刻关掉，线程对象由内核随后回收。
    CloseHandle(thread);
    return kOk;
}

void CScreenshotSession::PostResult(SnapCode code, const std::wstring& path, const std::wstring& detail, int width,
                                    int height, bool clipboardOk)
{
    HWND sink = m_hSink;
    if (sink == nullptr)
    {
        return;
    }
    SessionResult* result = new (std::nothrow) SessionResult();
    if (result == nullptr)
    {
        return;
    }
    result->code = code;
    result->path = path;
    result->detail = detail;
    result->width = width;
    result->height = height;
    result->clipboardOk = clipboardOk;

    if (PostMessageW(sink, WM_SNAP_RESULT, static_cast<WPARAM>(static_cast<INT_PTR>(code)),
                     reinterpret_cast<LPARAM>(result)) == FALSE)
    {
        delete result;
    }
}

DWORD WINAPI CScreenshotSession::JobThreadProc(LPVOID param)
{
    JobContext* context = static_cast<JobContext*>(param);
    if (context == nullptr)
    {
        return 0;
    }

    CScreenshotSession* session = context->session;
    CPluginSettings* settings = context->settings;
    CUxText* text = context->text;
    const HWND sink = context->hSink;
    HANDLE hStop = context->hStop;
    const CaptureRequest request = context->request;
    delete context;
    context = nullptr;

    struct BusyReset
    {
        CScreenshotSession* session;
        ~BusyReset()
        {
            if (session != nullptr)
            {
                session->m_busy = false;
            }
        }
    } busyReset{session};

    if (session == nullptr || settings == nullptr)
    {
        return 0;
    }

    // 引擎与编码器都是这个作业线程私有的：DPI 上下文与 COM apartment 都是线程局部的，
    // 跨线程复用会直接踩到 DEF-02 那类问题。
    CCaptureEngine engine;
    CImageWriter writer;
    snap::SetThreadDpiAwareness();

    auto postResult = [&](SnapCode code, const std::wstring& path, const std::wstring& detail, int width,
                          int height, bool clipboardOk) {
        session->PostResult(code, path, detail, width, height, clipboardOk);
    };

    if (session->m_shutdown.load())
    {
        return 0;
    }

    // 延时：100ms 分片，可被取消/退出信号打断。
    if (request.delaySeconds > 0)
    {
        const ULONGLONG deadline =
            GetTickCount64() + static_cast<ULONGLONG>(request.delaySeconds) * static_cast<ULONGLONG>(1000);
        while (GetTickCount64() < deadline)
        {
            if (hStop != nullptr)
            {
                if (WaitForSingleObject(hStop, 100) != WAIT_TIMEOUT)
                {
                    postResult(kCancelled, L"", L"", 0, 0, false);
                    return 0;
                }
            }
            else
            {
                Sleep(100);
            }
            if (session->m_shutdown.load())
            {
                return 0;
            }
        }
    }

    SnapCode code = kOk;
    std::wstring detail;
    CapturedFrame frame;

    switch (request.mode)
    {
    case CaptureMode::Region:
    {
        CRegionSelector selector;
        SelectionResult selection = selector.Select(hStop);
        if (!selection.confirmed)
        {
            postResult(kCancelled, L"", L"", 0, 0, false);
            return 0;
        }
        frame = engine.CaptureRegion(selection.rect, request.includeCursor, code);
        break;
    }
    case CaptureMode::ActiveWindow:
    {
        // v1.2.0：先弹窗口选择列表（与区域覆盖层同在作业线程上），用户确认后再截取；
        // 取消走静默结果（kCancelled 属于 1..9 静默段，不弹气泡）。
        CWindowPicker picker;
        HWND target = nullptr;
        if (!picker.Pick(text, target))
        {
            postResult(kCancelled, L"", L"", 0, 0, false);
            return 0;
        }
        frame = engine.CaptureWindow(target, request.includeCursor, code, detail);
        break;
    }
    case CaptureMode::FullScreen:
    default:
        frame = engine.CaptureFullScreen(request.includeCursor, code);
        break;
    }

    if (code != kOk || !frame.IsValid())
    {
        if (code == kOk)
        {
            code = kErrInternal;
        }
        frame.Reset();
        postResult(code, L"", detail, 0, 0, false);
        return 0;
    }

    const int width = frame.width;
    const int height = frame.height;

    std::wstring path;
    std::wstring writeDetail;
    bool clipboardOk = false;

    // WIC / COM 的初始化与收尾只包住"保存"这一段，绝不留在退出路径上
    // （CThreadScope 在 shutdown 时还会主动跳过 COM 收尾）。
    {
        CImageWriter::CThreadScope writerScope(&writer, &session->m_shutdown);
        (void)writerScope;

        code = writer.Save(frame, *settings, path, writeDetail);

        if (code == kOk && settings->data.copyToClipboard)
        {
            clipboardOk = (writer.CopyToClipboard(frame, sink) == kOk);
            if (!clipboardOk)
            {
                if (!writeDetail.empty())
                {
                    writeDetail += L"; ";
                }
                writeDetail += (text != nullptr) ? text->Str(StringId::kNotifyClipboardFailed) : L"clipboard";
            }
        }
    }

    frame.Reset();
    postResult(code, path, writeDetail, width, height, clipboardOk);
    return 0;
}

// ---------------------------------------------------------------------------
// CTMPlugin
// ---------------------------------------------------------------------------
CTMPlugin::CTMPlugin() : m_item(this)
{
}

CTMPlugin::~CTMPlugin()
{
}

bool CTMPlugin::RegisterSinkClass()
{
    static bool registered = false;
    if (registered)
    {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &CTMPlugin::SinkProc;
    wc.hInstance = g_hDllInstance;
    wc.lpszClassName = TMSNAP_SINK_CLASS;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        SNAP_LOGE(kErrInternal, L"RegisterClassEx(%s) failed", TMSNAP_SINK_CLASS);
        return false;
    }
    registered = true;
    return true;
}

void CTMPlugin::OnInitialize(ITrafficMonitor* pApp)
{
    if (m_initialized)
    {
        m_app = pApp;
        return;
    }

    m_app = pApp;

    // ITrafficMonitor::GetAPIVersion 位于槽 0，任何版本的宿主都实现它（1.8.6 返回 0）。
    m_appApiVersion = (m_app != nullptr ? m_app->GetAPIVersion() : 0);

    // GetLanguageId（槽5）/ GetPluginConfigDir（槽6）是接口最初版就存在的函数，
    // 所有版本的宿主都实现，无需版本门禁；只有槽 10/11（两个窗口句柄函数）需要 m_appApiVersion >= 1。
    unsigned short languageId = 2052;
    std::wstring hostConfigDir;
    if (m_app != nullptr)
    {
        languageId = m_app->GetLanguageId();
        const wchar_t* dir = m_app->GetPluginConfigDir();
        if (dir != nullptr)
        {
            hostConfigDir = dir;
        }
    }
    m_text.Init(languageId);
    m_item.SetName(m_text.Str(StringId::kItemName));

    m_settings.Initialize(hostConfigDir, m_extConfigDir);

    if (!RegisterSinkClass())
    {
        return;
    }
    m_hSink = CreateWindowExW(0, TMSNAP_SINK_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hDllInstance,
                              this);
    if (m_hSink == nullptr)
    {
        SNAP_LOGE(kErrInternal, L"cannot create the sink window");
        return;
    }

    m_session.Start(&m_settings, &m_text, m_hSink);

    // 热键直接注册在 sink 窗口所在的宿主 UI 线程上（见 HotkeyManager.h 的说明），
    // 注册与注销天然同线程，卸载时也不需要 join 任何自建线程。
    m_hotkeys.Attach(m_hSink);
    m_hotkeys.Apply(m_settings.data.hotkey1, m_settings.data.hotkey2);

    EnsureIcons();
    UpdateTooltip();

    m_initialized = true;
    m_shutdown = false;

    if ((m_settings.data.hotkey1.enabled && !m_hotkeys.IsRegistered(0)) ||
        (m_settings.data.hotkey2.enabled && !m_hotkeys.IsRegistered(1)))
    {
        // Report the conflict asynchronously so the host start-up is not blocked.
        PostMessageW(m_hSink, WM_SNAP_HOTKEY_RESULT, static_cast<WPARAM>(kErrHotkeyConflict), 0);
    }
    SNAP_LOG(L"plugin initialised, config dir: %s", m_settings.ConfigDir().c_str());
}

IPluginItem* CTMPlugin::GetItem(int index)
{
    if (index != 0)
    {
        return nullptr;
    }
    return &m_item;
}

void CTMPlugin::DataRequired()
{
    // Never collect data / do IO here: the host calls this about once a second.
    m_dataTick = GetTickCount64();
}

const wchar_t* CTMPlugin::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME:
        return m_text.Str(StringId::kPluginName);
    case TMI_DESCRIPTION:
        return m_text.Str(StringId::kPluginDescription);
    case TMI_AUTHOR:
        return m_text.Str(StringId::kPluginAuthor);
    case TMI_COPYRIGHT:
        return m_text.Str(StringId::kPluginCopyright);
    case TMI_VERSION:
        return m_text.Str(StringId::kPluginVersion);
    case TMI_URL:
        return m_text.Str(StringId::kPluginUrl);
    default:
        return L"";
    }
}

void CTMPlugin::OnMonitorInfo(const MonitorInfo& monitor_info)
{
    (void)monitor_info;
}

const wchar_t* CTMPlugin::GetTooltipInfo()
{
    if (m_tooltip.empty())
    {
        UpdateTooltip();
    }
    return m_tooltip.c_str();
}

void CTMPlugin::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
{
    if (index == EI_CONFIG_DIR && data != nullptr)
    {
        m_extConfigDir = data;
    }
}

void* CTMPlugin::GetPluginIcon()
{
    EnsureIcons();
    return m_hPluginIcon;
}

int CTMPlugin::GetCommandCount()
{
    return kCommandCount;
}

const wchar_t* CTMPlugin::GetCommandName(int command_index)
{
    if (command_index < 0 || command_index >= kCommandCount)
    {
        return L"";
    }
    return m_text.Str(kCommandStringIds[command_index]);
}

void* CTMPlugin::GetCommandIcon(int command_index)
{
    if (command_index < 0 || command_index >= kCommandCount)
    {
        return nullptr;
    }
    EnsureIcons();
    return m_hCmdIcons[command_index];
}

int CTMPlugin::IsCommandChecked(int command_index)
{
    (void)command_index;
    return 0;
}

void CTMPlugin::OnPluginCommand(int command_index, void* hWnd, void* para)
{
    (void)para;
    if (m_shutdown.load())
    {
        return;
    }
    switch (command_index)
    {
    case kCmdIndexFullScreen:
        RequestCapture(CaptureMode::FullScreen, 0, CaptureSource::Command);
        break;
    case kCmdIndexRegion:
        RequestCapture(CaptureMode::Region, 0, CaptureSource::Command);
        break;
    case kCmdIndexActiveWindow:
        RequestCapture(CaptureMode::ActiveWindow, 0, CaptureSource::Command);
        break;
    case kCmdIndexDelay:
        RequestCapture(CaptureMode::FullScreen, m_settings.data.delaySeconds, CaptureSource::Command);
        break;
    case kCmdIndexOpenFolder:
        OpenSaveFolder();
        break;
    case kCmdIndexSettings:
        ShowOptionsDialog(hWnd);
        break;
    default:
        break;
    }
}

ITMPlugin::OptionReturn CTMPlugin::ShowOptionsDialog(void* hParent)
{
    if (m_shutdown.load() || !m_initialized)
    {
        return ITMPlugin::OR_OPTION_UNCHANGED;
    }
    if (m_optionsOpen)
    {
        return ITMPlugin::OR_OPTION_UNCHANGED;
    }

    HWND parent = reinterpret_cast<HWND>(hParent);
    if (parent == nullptr && m_app != nullptr && m_appApiVersion >= 1)
    {
        parent = reinterpret_cast<HWND>(m_app->GetMainWindowHwnd());
    }

    m_optionsOpen = true;
    SNAP_LOG(L"options dialog: enter");
    ITMPlugin::OptionReturn result = ITMPlugin::OR_OPTION_UNCHANGED;
    {
        COptionsDialog dialog(&m_settings, &m_text, &m_hotkeys);
        result = dialog.Show(parent);
    }
    SNAP_LOG(L"options dialog: returned %d", static_cast<int>(result));
    m_optionsOpen = false;

    if (result == ITMPlugin::OR_OPTION_CHANGED)
    {
        UpdateTooltip();
        InvalidateHostWindows();
        if (m_settings.data.hotkey1.enabled && !m_hotkeys.IsRegistered(0))
        {
            ShowNotify(snap::FormatString(m_text.Str(StringId::kNotifyHotkeyConflict),
                                          m_settings.data.hotkey1.binding.ToText().c_str()));
        }
        else if (m_settings.data.hotkey2.enabled && !m_hotkeys.IsRegistered(1))
        {
            ShowNotify(snap::FormatString(m_text.Str(StringId::kNotifyHotkeyConflict),
                                          m_settings.data.hotkey2.binding.ToText().c_str()));
        }
    }
    return result;
}

bool CTMPlugin::RequestCapture(CaptureMode mode, int delaySeconds, CaptureSource source)
{
    if (m_shutdown.load() || !m_initialized)
    {
        return false;
    }
    if (m_optionsOpen)
    {
        return false;
    }

    CaptureRequest request;
    request.mode = mode;
    request.delaySeconds = delaySeconds < 0 ? 0 : (delaySeconds > 30 ? 30 : delaySeconds);
    request.includeCursor = m_settings.data.includeCursor;
    request.source = source;

    return m_session.Request(request) == kOk;
}

void CTMPlugin::OnSessionResult(SnapCode code, const std::wstring& path, const std::wstring& detail, int width,
                                int height)
{
    (void)width;
    (void)height;

    if (code == kOk)
    {
        m_lastFileName = snap::GetFileNamePart(path);
        GetLocalTime(&m_lastTime);
        m_settings.SaveLastSavedPath(path);
        UpdateTooltip();
        InvalidateHostWindows();

        std::wstring message;
        if (detail.empty())
        {
            message = snap::FormatString(m_text.Str(StringId::kNotifySaved), m_lastFileName.c_str());
        }
        else
        {
            message = snap::FormatString(m_text.Str(StringId::kNotifySavedWithDetail),
                                         m_lastFileName.c_str(), detail.c_str());
        }
        ShowNotify(message);
        return;
    }

    if (code >= 0x8000)
    {
        std::wstring message = snap::FormatString(m_text.Str(StringId::kNotifyFailed),
                                                  m_text.SnapCodeText(code), detail.c_str());
        ShowNotify(message);
        SNAP_LOGE(code, L"capture failed: %s", detail.c_str());
        return;
    }
    // 1..9 are silent results (cancel / busy / debounce / options open).
}

void CTMPlugin::UpdateTooltip()
{
    if (m_lastFileName.empty())
    {
        std::wstring hotkeyText;
        if (m_settings.data.hotkey1.enabled && m_hotkeys.IsRegistered(0))
        {
            hotkeyText = m_settings.data.hotkey1.binding.ToText();
        }
        else if (m_settings.data.hotkey2.enabled && m_hotkeys.IsRegistered(1))
        {
            hotkeyText = m_settings.data.hotkey2.binding.ToText();
        }
        else
        {
            hotkeyText = m_text.Str(StringId::kTipHotkeyDisabled);
        }
        m_tooltip = snap::FormatString(m_text.Str(StringId::kTipIdle), hotkeyText.c_str());
    }
    else
    {
        m_tooltip = snap::FormatString(m_text.Str(StringId::kTipLast), m_lastFileName.c_str(),
                                       snap::GetTimeText(m_lastTime).c_str());
    }
}

void CTMPlugin::EnsureIcons()
{
    if (m_hPluginIcon == nullptr)
    {
        m_hPluginIcon = CreateGlyphIcon(32, kGlyphCamera, kIconColor);
    }
    for (int i = 0; i < kCommandCount; ++i)
    {
        if (m_hCmdIcons[i] == nullptr)
        {
            m_hCmdIcons[i] = CreateGlyphIcon(16, kCommandGlyphs[i], kIconColor);
        }
    }
}

void CTMPlugin::InvalidateHostWindows()
{
    if (m_app == nullptr)
    {
        return;
    }
    // GetMainWindowHwnd（槽10）/ GetTaskbarWindowHwnd（槽11）是 ITrafficMonitor 接口版本 1 才新增的函数。
    // 1.8.6 宿主的 m_appApiVersion == 0，虚表里没有这两个槽位，调用会读到 vtable 末尾之后的垃圾指针，
    // 直接导致宿主崩溃（真实事故）。旧宿主下跳过刷新即可，仅损失即时重绘，无功能影响。
    if (m_appApiVersion < 1)
    {
        return;
    }
    HWND mainWindow = reinterpret_cast<HWND>(m_app->GetMainWindowHwnd());
    if (mainWindow != nullptr && IsWindow(mainWindow) != FALSE)
    {
        InvalidateRect(mainWindow, nullptr, FALSE);
    }
    HWND taskbarWindow = reinterpret_cast<HWND>(m_app->GetTaskbarWindowHwnd());
    if (taskbarWindow != nullptr && IsWindow(taskbarWindow) != FALSE)
    {
        InvalidateRect(taskbarWindow, nullptr, FALSE);
    }
}

void CTMPlugin::ShowNotify(const std::wstring& message)
{
    if (m_app == nullptr || message.empty())
    {
        return;
    }
    // Only ever called from the host UI thread (sink WndProc / command handler).
    m_app->ShowNotifyMessage(message.c_str());
}

void CTMPlugin::OpenSaveFolder()
{
    const std::wstring dir = m_settings.EffectiveSaveDir();
    bool opened = false;

    // SHOpenFolderAndSelectItems 是 COM 消费者：宿主 UI 线程可能没有任何 COM 单元
    // （TrafficMonitor 1.8.6 全程未调 OleInitialize，见 DEF-09），这里按需初始化并配对释放。
    HRESULT oleHr = OleInitialize(nullptr);
    const bool oleByUs = (oleHr == S_OK);

    if (!dir.empty() && GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        const std::wstring lastFile = m_settings.data.lastSavedPath;
        if (!lastFile.empty() && GetFileAttributesW(lastFile.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            PIDLIST_ABSOLUTE idList = ILCreateFromPathW(lastFile.c_str());
            if (idList != nullptr)
            {
                if (SUCCEEDED(SHOpenFolderAndSelectItems(idList, 0, nullptr, 0)))
                {
                    opened = true;
                }
                ILFree(idList);
            }
        }
        if (!opened)
        {
            HINSTANCE result = ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            opened = reinterpret_cast<INT_PTR>(result) > 32;
        }
    }

    if (oleByUs)
    {
        OleUninitialize();
    }

    if (opened)
    {
        return;
    }

    // The configured folder is gone: open its parent and tell the user.
    const std::wstring parent = snap::GetDirectoryPart(dir);
    if (!parent.empty())
    {
        HINSTANCE result = ShellExecuteW(nullptr, L"open", parent.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) > 32)
        {
            ShowNotify(m_text.Str(StringId::kNotifyFolderMissing));
            return;
        }
    }
    ShowNotify(snap::FormatString(m_text.Str(StringId::kMsgDirNotWritable), dir.c_str()));
}

void CTMPlugin::Shutdown(bool processExit)
{
    (void)processExit;
    if (m_shutdown.exchange(true))
    {
        return;
    }
    // No logging here: this can run inside DllMain under the loader lock.
    // Stop() 只发信号、不等待：DllMain 持有 loader lock，而线程退出要拿 loader lock，
    // 在这里 join 必然超时（DEF-02 的根因，见 CScreenshotSession::Stop 的注释）。
    m_session.Stop();
    m_hotkeys.Stop();

    // 此时插件已不再持有任何自建线程（热键注册在宿主 UI 线程；抓屏是"一次作业一个
    // 线程"、随作业结束自行退出），所以可以安全销毁 sink 窗口：它在当前线程创建，
    // DestroyWindow 不碰 loader lock，也不存在别的线程回头调用它的窗口过程。
    HWND sink = m_hSink;
    m_hSink = nullptr;
    if (sink != nullptr && IsWindow(sink) != FALSE)
    {
        DestroyWindow(sink);
    }
}

LRESULT CALLBACK CTMPlugin::SinkProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }

    CTMPlugin* self = reinterpret_cast<CTMPlugin*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr)
    {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg)
    {
    case WM_HOTKEY:
    {
        // 热键注册在宿主 UI 线程（sink 窗口）上，所以 WM_HOTKEY 直接送到这里；
        // 处理只是"投递一个请求"，仍然是 O(1)、不抓屏、不阻塞。
        // wParam 是 RegisterHotKey 时的 id：1 → 槽位 1，2 → 槽位 2。
        const HotkeySlot* slot = nullptr;
        if (static_cast<int>(wp) == snap::kHotkeyId && self->m_settings.data.hotkey1.enabled)
        {
            slot = &self->m_settings.data.hotkey1;
        }
        else if (static_cast<int>(wp) == snap::kHotkeyId2 && self->m_settings.data.hotkey2.enabled)
        {
            slot = &self->m_settings.data.hotkey2;
        }
        if (slot != nullptr)
        {
            CaptureMode mode = CaptureMode::FullScreen;
            int delay = 0;
            switch (slot->action)
            {
            case 1:
                mode = CaptureMode::Region;
                break;
            case 2:
                mode = CaptureMode::ActiveWindow;
                break;
            case 3: // 延时：使用设置里的 delay_seconds
                mode = CaptureMode::FullScreen;
                delay = self->m_settings.data.delaySeconds;
                break;
            case 0:
            default:
                mode = CaptureMode::FullScreen;
                break;
            }
            self->RequestCapture(mode, delay, CaptureSource::Hotkey);
        }
        return 0;
    }

    case WM_SNAP_RESULT:
    {
        SessionResult* result = reinterpret_cast<SessionResult*>(lp);
        if (result != nullptr)
        {
            self->OnSessionResult(result->code, result->path, result->detail, result->width, result->height);
            delete result;
        }
        (void)wp;
        return 0;
    }
    case WM_SNAP_HOTKEY_RESULT:
    {
        const SnapCode code = static_cast<SnapCode>(static_cast<int>(wp));
        // 冲突提示优先报槽位 1，其次槽位 2。
        const HotkeySlot* conflicted = nullptr;
        if (self->m_settings.data.hotkey1.enabled && !self->m_hotkeys.IsRegistered(0))
        {
            conflicted = &self->m_settings.data.hotkey1;
        }
        else if (self->m_settings.data.hotkey2.enabled && !self->m_hotkeys.IsRegistered(1))
        {
            conflicted = &self->m_settings.data.hotkey2;
        }
        const wchar_t* hotkeyText =
            conflicted != nullptr ? conflicted->binding.ToText().c_str()
                                  : self->m_settings.data.hotkey1.binding.ToText().c_str();
        if (code == kErrHotkeyConflict)
        {
            self->ShowNotify(snap::FormatString(self->m_text.Str(StringId::kNotifyHotkeyConflict), hotkeyText));
        }
        else if (code == kOk)
        {
            self->ShowNotify(snap::FormatString(self->m_text.Str(StringId::kNotifyHotkeyEnabled), hotkeyText));
        }
        else if (code == kCancelled)
        {
            self->ShowNotify(self->m_text.Str(StringId::kNotifyHotkeyDisabled));
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// DLL entry points
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    // Deliberately never destroyed: a function-local static CTMPlugin would run
    // its destructor (std::wstring / COM / handles) inside DllMain(DETACH) while
    // the loader lock is held, which is a classic deadlock source.
    if (g_pluginInstance == nullptr)
    {
        g_pluginInstance = new (std::nothrow) CTMPlugin();
    }
    return g_pluginInstance;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hDllInstance = hinst;
        DisableThreadLibraryCalls(hinst);
        return TRUE;
    }
    if (reason == DLL_PROCESS_DETACH)
    {
        if (g_pluginInstance != nullptr)
        {
            // reserved != nullptr means the process is exiting: signal only.
            g_pluginInstance->Shutdown(reserved != nullptr);
        }
        return TRUE;
    }
    return TRUE;
}
