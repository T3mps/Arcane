#pragma once
#if !defined(ARC_BUILD_DIST)
#include <Arcane/Remote/Registry.hpp>
#include <functional>
#include <utility>

namespace RemoteTest {

// Sets the process registry's host kind; restores the previous kind on exit.
class ScopedHostKind {
public:
    explicit ScopedHostKind(Arcane::Remote::HostKind kind)
        : m_before(Arcane::Remote::CommandRegistry::Get().GetHostKind()) {
        Arcane::Remote::CommandRegistry::Get().SetHostKind(kind);
    }
    ~ScopedHostKind() { Arcane::Remote::CommandRegistry::Get().SetHostKind(m_before); }
    ScopedHostKind(const ScopedHostKind&) = delete;
    ScopedHostKind& operator=(const ScopedHostKind&) = delete;
private:
    Arcane::Remote::HostKind m_before;
};

// Calls setter(value) now and setter(nullptr) on scope exit, so a process-global
// command never keeps a pointer to a test's dead stack object. The setter is
// any callable taking T* (a Register*/Set* function, or a lambda adapting one;
// its return value is ignored).
template <class T>
class ScopedRemotePointer {
public:
    template <class Fn>
    ScopedRemotePointer(Fn setter, T* value)
        : m_set([setter](T* p) mutable { (void)setter(p); }) {
        m_set(value);
    }
    ~ScopedRemotePointer() { m_set(nullptr); }
    ScopedRemotePointer(const ScopedRemotePointer&) = delete;
    ScopedRemotePointer& operator=(const ScopedRemotePointer&) = delete;
private:
    std::function<void(T*)> m_set;
};

}  // namespace RemoteTest
#endif
