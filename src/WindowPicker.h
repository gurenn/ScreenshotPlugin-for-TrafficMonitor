// WindowPicker.h - pre-capture window selection for the active-window mode (v1.2.0).
//
// 运行在作业线程上（与区域选择覆盖层同一条管线约定）：弹出当前所有可截取窗口的
// 列表（图标 + 标题），用户选中后确认。列表与布局规则：
//   - 仅列出 可见 / 非最小化 / 有标题 / 非工具窗口 / 未被 cloak（UWP 幽灵窗口） 的顶层窗口；
//   - 跳过插件自己的窗口（sink/overlay/picker 等）；
//   - 当前前台窗口排在第一位并默认选中。
#pragma once

#include "PluginSettings.h"

#include <vector>

class CWindowPicker
{
public:
    CWindowPicker() = default;
    ~CWindowPicker() = default;

    CWindowPicker(const CWindowPicker&) = delete;
    CWindowPicker& operator=(const CWindowPicker&) = delete;

    // 阻塞（内部模态循环）。返回 true 表示用户确认了一个窗口（out）；
    // 返回 false 表示用户取消（Esc / 取消按钮 / 关闭）或没有可截取的窗口。
    bool Pick(CUxText* text, HWND& out);

private:
    static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static BOOL CALLBACK CollectProc(HWND hwnd, LPARAM lp);
    void FillList(HWND hDlg) const;

    std::vector<WindowPickEntry> m_entries;
    WNDPROC m_oldListProc{nullptr};
};
