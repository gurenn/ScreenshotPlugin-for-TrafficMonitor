// TMPlugin.h - ITMPlugin / IPluginItem / session orchestration declarations.
#pragma once

#include "PluginSettings.h"
#include "CaptureEngine.h"
#include "ImageWriter.h"
#include "RegionSelector.h"
#include "HotkeyManager.h"
#include "PluginInterface.h"

// ---------------------------------------------------------------------------
// Command indices.  THE ORDER IS PART OF THE CONTRACT (PRD 5.1) - never change.
// ---------------------------------------------------------------------------
enum : int
{
    kCmdIndexFullScreen = 0,
    kCmdIndexRegion = 1,
    kCmdIndexActiveWindow = 2,
    kCmdIndexDelay = 3,
    kCmdIndexOpenFolder = 4,
    kCmdIndexSettings = 5,
    kCommandCount = 6
};

// Item glyph shapes.
enum : int
{
    kGlyphCamera = 0,
    kGlyphMonitor = 1,
    kGlyphCrop = 2,
    kGlyphWindow = 3,
    kGlyphClock = 4,
    kGlyphFolder = 5,
    kGlyphGear = 6
};

class CTMPlugin;

// ---------------------------------------------------------------------------
// Drawing abstraction: the same glyph code serves IPluginDrawer and a raw HDC.
// Colors are 0xBBGGRR (the format IPluginDrawer expects).
// ---------------------------------------------------------------------------
class GlyphSink
{
public:
    virtual ~GlyphSink() {}
    virtual void Fill(int x, int y, int w, int h, unsigned long color) = 0;
    virtual void Frame(int x, int y, int w, int h, unsigned long color, int width, bool dotted, int radius) = 0;
    virtual void Line(int x1, int y1, int x2, int y2, unsigned long color, int width) = 0;
};

class DrawerSink : public GlyphSink
{
public:
    explicit DrawerSink(IPluginDrawer* drawer) : m_drawer(drawer) {}
    void Fill(int x, int y, int w, int h, unsigned long color) override;
    void Frame(int x, int y, int w, int h, unsigned long color, int width, bool dotted, int radius) override;
    void Line(int x1, int y1, int x2, int y2, unsigned long color, int width) override;

private:
    IPluginDrawer* m_drawer;
};

class HdcSink : public GlyphSink
{
public:
    explicit HdcSink(HDC dc) : m_dc(dc) {}
    void Fill(int x, int y, int w, int h, unsigned long color) override;
    void Frame(int x, int y, int w, int h, unsigned long color, int width, bool dotted, int radius) override;
    void Line(int x1, int y1, int x2, int y2, unsigned long color, int width) override;

private:
    HDC m_dc;
};

void DrawCameraGlyph(GlyphSink& sink, int x, int y, int w, int h, unsigned long color);
void DrawShapeGlyph(GlyphSink& sink, int x, int y, int w, int h, unsigned long color, int shape);

// ---------------------------------------------------------------------------
// The "camera" display item.
// ---------------------------------------------------------------------------
class CScreenshotItem : public IPluginItem
{
public:
    explicit CScreenshotItem(CTMPlugin* owner);
    ~CScreenshotItem() {}

    void SetName(const std::wstring& name) { m_name = name; }

    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;
    bool IsCustomDraw() const override;
    int GetItemWidth() const override;
    int GetItemWidthEx(void* hDC) const override;
    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override;
    bool DrawItemEx(IPluginDrawer* pDrawer, int x, int y, int w, int h, bool dark_mode) override;
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;
    int IsDoubleLineExclusive() const override;

private:
    CTMPlugin* m_owner;
    std::wstring m_name;
    std::wstring m_id;
};

// ---------------------------------------------------------------------------
// The single capture pipeline.
//
// 每次请求开一个"作业线程"，作业结束线程自行退出（句柄立即 CloseHandle，不需要 join）。
// 为什么不再常驻一个 worker 线程（DEF-02）：常驻线程在 FreeLibrary 时无法在
// DllMain(DLL_PROCESS_DETACH) 内被 join —— 线程退出要拿 loader lock，而 loader lock
// 正被 DllMain 持有。常驻线程 = 卸载时必然留下一个"正在执行插件代码"的线程。
// 改成作业线程后，"刚截完图就卸载"这个场景下插件已经没有任何自建线程，卸载是确定安全的。
// 代价：卸载恰好撞上进行中的作业（延时/框选/编码）时仍存在竞态，已在 docs/README.md 记录。
// ---------------------------------------------------------------------------
class CScreenshotSession
{
public:
    CScreenshotSession();
    ~CScreenshotSession();

    CScreenshotSession(const CScreenshotSession&) = delete;
    CScreenshotSession& operator=(const CScreenshotSession&) = delete;

    bool Start(CPluginSettings* settings, CUxText* text, HWND hSink);
    void Stop();

    SnapCode Request(const CaptureRequest& request);
    bool IsBusy() const { return m_busy.load(); }
    void Cancel();

    static DWORD WINAPI JobThreadProc(LPVOID param);

private:
    void PostResult(SnapCode code, const std::wstring& path, const std::wstring& detail, int width, int height,
                    bool clipboardOk);

    CPluginSettings* m_pSettings{nullptr};
    CUxText* m_pText{nullptr};
    HWND m_hSink{nullptr};
    HANDLE m_hStop{nullptr};
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_shutdown{false};
    ULONGLONG m_lastTick{0};
    bool m_started{false};
};

// ---------------------------------------------------------------------------
// The plugin object itself.
// ---------------------------------------------------------------------------
class CTMPlugin : public ITMPlugin
{
public:
    CTMPlugin();
    ~CTMPlugin();

    // ITMPlugin
    IPluginItem* GetItem(int index) override;
    void DataRequired() override;
    OptionReturn ShowOptionsDialog(void* hParent) override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    void OnMonitorInfo(const MonitorInfo& monitor_info) override;
    const wchar_t* GetTooltipInfo() override;
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;
    void* GetPluginIcon() override;
    int GetCommandCount() override;
    const wchar_t* GetCommandName(int command_index) override;
    void* GetCommandIcon(int command_index) override;
    void OnPluginCommand(int command_index, void* hWnd, void* para) override;
    int IsCommandChecked(int command_index) override;
    void OnInitialize(ITrafficMonitor* pApp) override;

    // Internal
    bool RequestCapture(CaptureMode mode, int delaySeconds, CaptureSource source);
    void Shutdown(bool processExit);
    void OnSessionResult(SnapCode code, const std::wstring& path, const std::wstring& detail, int width,
                         int height);
    void OpenSaveFolder();
    void UpdateTooltip();
    void EnsureIcons();
    void InvalidateHostWindows();
    void ShowNotify(const std::wstring& message);

    static LRESULT CALLBACK SinkProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    bool InitializeCore(ITrafficMonitor* pApp);
    static bool RegisterSinkClass();

    ITrafficMonitor* m_app{nullptr};
    // ITrafficMonitor 的接口版本（宿主自报，1.8.6 返回 0）。
    // 槽位 >=10 的函数（GetMainWindowHwnd/GetTaskbarWindowHwnd）仅在 >=1 时才允许调用，
    // 否则会读到宿主 vtable 末尾之后的垃圾指针并使宿主崩溃（真实事故：1.8.6 热键截图必崩）。
    int m_appApiVersion{0};
    CPluginSettings m_settings;
    CUxText m_text;
    CScreenshotItem m_item;
    CScreenshotSession m_session;
    CHotkeyManager m_hotkeys;
    HWND m_hSink{nullptr};
    HICON m_hPluginIcon{nullptr};
    HICON m_hCmdIcons[kCommandCount]{};
    std::wstring m_tooltip;
    std::wstring m_lastFileName;
    std::wstring m_extConfigDir;
    SYSTEMTIME m_lastTime{};
    ULONGLONG m_dataTick{0};
    bool m_initialized{false};
    bool m_optionsOpen{false};
    std::atomic<bool> m_shutdown{false};
};
