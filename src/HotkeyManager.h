// HotkeyManager.h - global hotkeys registered on the host UI thread.
//
// 为什么不再单开热键线程（DEF-02）：
//   DLL 里自建的任何线程在 FreeLibrary 时都无法在 DllMain(DLL_PROCESS_DETACH) 内被
//   join —— 线程退出要拿 loader lock，而 loader lock 正被我们自己的 DllMain 持有。
//   实测把两处等待放大到 3s，FreeLibrary 就精确地多花 6s，说明 join 必然超时；
//   超时后 DllMain 返回、映像被 unmap，线程再回来执行我们的代码就是 0xC0000005。
//   RegisterHotKey / UnregisterHotKey 本来就必须在同一个线程里配对，直接注册在
//   sink 窗口所在的宿主 UI 线程上天然满足这条约束，还省掉一次 SendMessageTimeout 换绑。
//   宿主 UI 线程只收 WM_HOTKEY（MOD_NOREPEAT），处理就是"投递一个请求"，O(1) 不阻塞。
//
// 双槽位（v1.1.0）：槽 0 → WM_HOTKEY wParam = kHotkeyId(1)；槽 1 → kHotkeyId2(2)。
// Apply 对两个槽位做原子换绑：任一启用槽注册失败则整体回滚到调用前状态并返回 false。
#pragma once

#include "PluginSettings.h"

class CHotkeyManager
{
public:
    CHotkeyManager();
    ~CHotkeyManager();

    CHotkeyManager(const CHotkeyManager&) = delete;
    CHotkeyManager& operator=(const CHotkeyManager&) = delete;

    // 必须在宿主 UI 线程调用（sink 窗口所在线程）。
    void Attach(HWND hSink);

    // 原子换绑两个槽位；全部成功返回 true。
    // 失败时（任一启用槽 RegisterHotKey 失败）恢复到调用前状态并返回 false，
    // 调用方据此不写 ini（PRD R-16：改键失败保留旧热键）。
    bool Apply(const HotkeySlot& slot1, const HotkeySlot& slot2);

    void Stop();

    // slotIndex: 0 / 1。禁用的槽位视为"未注册"。
    bool IsRegistered(int slotIndex) const;
    HotkeySlot Current(int slotIndex) const;
    HWND Window() const { return m_hwnd; }

private:
    struct SlotState
    {
        bool registered{false};
        bool enabled{false};
        HotkeyBinding binding{};
        int action{0};
    };

    void DoUnregisterAll();
    bool DoRegisterOne(int index);

    HWND m_hwnd{nullptr};
    SlotState m_slots[2]{};
};
