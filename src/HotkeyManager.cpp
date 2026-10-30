// HotkeyManager.cpp - RegisterHotKey / UnregisterHotKey on the host UI thread (two slots).
#include "HotkeyManager.h"

namespace
{
int SlotId(int index)
{
    return index == 0 ? snap::kHotkeyId : snap::kHotkeyId2;
}
} // namespace

CHotkeyManager::CHotkeyManager()
{
    m_slots[0].binding = HotkeyBinding::Default();
    m_slots[0].enabled = true;
    m_slots[0].action = 0;
    m_slots[1].binding = HotkeyBinding{MOD_CONTROL | MOD_ALT, 'R'};
    m_slots[1].enabled = false;
    m_slots[1].action = 1;
}

CHotkeyManager::~CHotkeyManager()
{
    DoUnregisterAll();
}

void CHotkeyManager::Attach(HWND hSink)
{
    m_hwnd = hSink;
}

void CHotkeyManager::DoUnregisterAll()
{
    if (m_hwnd == nullptr)
    {
        return;
    }
    for (int i = 0; i < 2; ++i)
    {
        if (m_slots[i].registered)
        {
            UnregisterHotKey(m_hwnd, SlotId(i));
        }
        m_slots[i].registered = false;
    }
}

bool CHotkeyManager::DoRegisterOne(int index)
{
    const SlotState& slot = m_slots[index];
    if (m_hwnd == nullptr || !slot.enabled)
    {
        return true; // 禁用的槽位不算失败
    }
    if (!slot.binding.IsValid())
    {
        SNAP_LOGE(kErrHotkeyConflict, L"invalid hotkey binding (slot %d)", index + 1);
        return false;
    }
    // MOD_NOREPEAT: 长按不连发（PRD R-18）。
    UINT modifiers = (slot.binding.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) | MOD_NOREPEAT;
    if (RegisterHotKey(m_hwnd, SlotId(index), modifiers, slot.binding.vk) == FALSE)
    {
        DWORD error = GetLastError();
        SNAP_LOGE(kErrHotkeyConflict, L"RegisterHotKey(%s) failed: %d", slot.binding.ToText().c_str(),
                  static_cast<int>(error));
        return false;
    }
    m_slots[index].registered = true;
    SNAP_LOG(L"hotkey %d registered: %s", index + 1, slot.binding.ToText().c_str());
    return true;
}

bool CHotkeyManager::Apply(const HotkeySlot& slot1, const HotkeySlot& slot2)
{
    if (m_hwnd == nullptr)
    {
        return false;
    }

    // 先全部解绑，再逐个注册；任一启用槽失败则整体回滚到调用前状态。
    const SlotState previous[2] = {m_slots[0], m_slots[1]};

    DoUnregisterAll();
    m_slots[0].enabled = slot1.enabled;
    m_slots[0].binding = slot1.binding;
    m_slots[0].action = slot1.action;
    m_slots[1].enabled = slot2.enabled;
    m_slots[1].binding = slot2.binding;
    m_slots[1].action = slot2.action;

    if (DoRegisterOne(0) && DoRegisterOne(1))
    {
        return true;
    }

    // 回滚：恢复旧槽位状态并重新注册旧绑定。
    const bool oldRegistered[2] = {previous[0].registered, previous[1].registered};
    m_slots[0] = previous[0];
    m_slots[1] = previous[1];
    m_slots[0].registered = false;
    m_slots[1].registered = false;
    if (oldRegistered[0])
    {
        DoRegisterOne(0);
    }
    if (oldRegistered[1])
    {
        DoRegisterOne(1);
    }
    return false;
}

void CHotkeyManager::Stop()
{
    DoUnregisterAll();
    for (int i = 0; i < 2; ++i)
    {
        m_slots[i].enabled = false;
    }
}

bool CHotkeyManager::IsRegistered(int slotIndex) const
{
    if (slotIndex < 0 || slotIndex > 1)
    {
        return false;
    }
    return m_slots[slotIndex].registered && m_slots[slotIndex].enabled;
}

HotkeySlot CHotkeyManager::Current(int slotIndex) const
{
    HotkeySlot out;
    if (slotIndex < 0 || slotIndex > 1)
    {
        return out;
    }
    out.enabled = m_slots[slotIndex].enabled;
    out.binding = m_slots[slotIndex].binding;
    out.action = m_slots[slotIndex].action;
    return out;
}
