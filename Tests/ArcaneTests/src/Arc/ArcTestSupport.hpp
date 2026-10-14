#pragma once
#if !defined(ARC_BUILD_DIST)
#include <Arcane/Arc/Registry.hpp>
#include <functional>
#include <utility>

namespace ArcTest {

// Sets the process registry's host kind; restores the previous kind on exit.
class ScopedHostKind {
public:
    explicit ScopedHostKind(Arcane::Arc::HostKind kind)
        : m_before(Arcane::Arc::CommandRegistry::Get().GetHostKind()) {
        Arcane::Arc::CommandRegistry::Get().SetHostKind(kind);
    }
    ~ScopedHostKind() { Arcane::Arc::CommandRegistry::Get().SetHostKind(m_before); }
    ScopedHostKind(const ScopedHostKind&) = delete;
    ScopedHostKind& operator=(const ScopedHostKind&) = delete;
private:
    Arcane::Arc::HostKind m_before;
};

// Calls setter(value) now and setter(nullptr) on scope exit, so a process-global
// command never keeps a pointer to a test's dead stack object. The setter is
// any callable taking T* (a Register*/Set* function, or a lambda adapting one;
// its return value is ignored).
template <class T>
class ScopedArcPointer {
public:
    template <class Fn>
    ScopedArcPointer(Fn setter, T* value)
        : m_set([setter](T* p) mutable { (void)setter(p); }) {
        m_set(value);
    }
    ~ScopedArcPointer() { m_set(nullptr); }
    ScopedArcPointer(const ScopedArcPointer&) = delete;
    ScopedArcPointer& operator=(const ScopedArcPointer&) = delete;
private:
    std::function<void(T*)> m_set;
};

}  // namespace ArcTest
#endif
