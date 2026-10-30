// lang_check: verify language selection when host reports GetLanguageId()==0 (TrafficMonitor 1.8.6 with language=follow-system).
// Expected: plugin falls back to GetUserDefaultUILanguage(); on a zh-CN system the command names must be Chinese.
#include "../include/PluginInterface.h"
#include <windows.h>
#include <cstdio>

class MockApp : public ITrafficMonitor
{
public:
    int GetAPIVersion() override { return 0; }              // 1.8.6 宿主实际返回 0
    const wchar_t* GetVersion() override { return L"1.8.6"; }
    double GetMonitorValue(MonitorItem) override { return 0; }
    const wchar_t* GetMonitorValueString(MonitorItem, int) override { return L""; }
    void ShowNotifyMessage(const wchar_t* msg) override { wprintf(L"[notify] %s\n", msg); }
    unsigned short GetLanguageId() const override { return 0; }  // 跟随系统
    const wchar_t* GetPluginConfigDir() const override { return L""; }
    int GetDPI(DPIType) const override { return 96; }
    unsigned int GetThemeColor() const override { return 0; }
    const wchar_t* GetStringRes(const wchar_t*, const wchar_t*) override { return L""; }
    void* GetMainWindowHwnd() override { return nullptr; }
    void* GetTaskbarWindowHwnd() override { return nullptr; }
};

int main()
{
    HMODULE dll = LoadLibraryW(L"TMPluginScreenshot.dll");
    if (!dll) { printf("LoadLibrary failed\n"); return 1; }
    auto get = reinterpret_cast<ITMPlugin* (*)()>(reinterpret_cast<void*>(GetProcAddress(dll, "TMPluginGetInstance")));
    if (!get) { printf("GetProcAddress failed\n"); return 1; }
    ITMPlugin* plugin = get();

    MockApp app;
    plugin->OnInitialize(&app);

    const wchar_t* cmd0 = plugin->GetCommandName(0);
    const wchar_t* cmd1 = plugin->GetCommandName(1);
    const wchar_t* tip = plugin->GetTooltipInfo();
    wprintf(L"cmd0 = %s\n", cmd0 ? cmd0 : L"(null)");
    wprintf(L"cmd1 = %s\n", cmd1 ? cmd1 : L"(null)");
    wprintf(L"tip  = %s\n", tip ? tip : L"(null)");

    bool chinese = (cmd0 && wcscmp(cmd0, L"全屏截图") == 0) && (cmd1 && wcscmp(cmd1, L"区域截图") == 0);
    bool english = (cmd0 && wcscmp(cmd0, L"Full screen screenshot") == 0);
    if (chinese) { printf("RESULT: CHINESE (PASS on a zh system)\n"); return 0; }
    if (english) { printf("RESULT: ENGLISH (FAIL on a zh system)\n"); return 3; }
    printf("RESULT: UNKNOWN TEXTS\n"); return 4;
}
