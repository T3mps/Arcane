#pragma once

// The editor's two COM users (IdeLaunch's DTE walk, OsShell's IFileOperation)
// share these. Windows-only: include AFTER <windows.h>/<objbase.h>, inside the
// TU's own #ifdef _WIN32.

#include <utility>

namespace Arcane::Editor::WinCom
{
    // COM for the duration of one call. S_FALSE (already initialised on this
    // thread, same model) is fine; RPC_E_CHANGED_MODE (already initialised with
    // the OTHER model) is fine too -- the ROT/DTE proxies and IFileOperation work
    // from either apartment, and in that case the init is not ours to undo.
    struct CoScope
    {
        HRESULT hr;
        CoScope() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
        ~CoScope() { if (SUCCEEDED(hr)) ::CoUninitialize(); }
        [[nodiscard]] bool Usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
    };

    // Owned COM reference; Release on scope exit. `Out()` hands the slot to
    // an out-parameter API; `Detach()` gives the reference away.
    template <class T>
    class Com
    {
    public:
        Com() = default;
        Com(const Com&) = delete;
        Com& operator=(const Com&) = delete;
        Com(Com&& o) noexcept : m_p(std::exchange(o.m_p, nullptr)) {}
        Com& operator=(Com&& o) noexcept
        {
            if (this != &o)
            {
                if (m_p) m_p->Release();
                m_p = std::exchange(o.m_p, nullptr);
            }
            return *this;
        }
        ~Com() { if (m_p) m_p->Release(); }
        T** Out() { return &m_p; }
        T*  Get() const { return m_p; }
        T*  operator->() const { return m_p; }
        explicit operator bool() const { return m_p != nullptr; }
        T*  Detach() { return std::exchange(m_p, nullptr); }
    private:
        T* m_p = nullptr;
    };
}
