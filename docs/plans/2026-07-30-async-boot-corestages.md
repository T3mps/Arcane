# Async boot: CoreStages, ServiceThread, and the loading screen — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the editor and runtime hosts boot from one canonical stage list (killing a recurring bug class), behind a real loading screen that appears immediately.

**Architecture:** A dependency-DAG `BootSequence` schedules boot stages; `HostBoot::CoreStages()` returns the canonical sequence both hosts take whole and may only append to. A pre-device OS splash covers the ~1s before the GPU exists; `BootPresenter` takes over once it does. `Arcane::ServiceThread` becomes the one shape for long-lived blocking workers.

**Tech Stack:** C++23, SDL3, Dear ImGui, NVRHI, Catch2 v3, premake5.

**Spec:** `docs/superpowers/specs/2026-07-29-async-boot-loading-screen-design.md`

## Global Constraints

- **/MD everywhere** (dynamic CRT). All modules share one heap.
- **UTF-8 without BOM. ASCII-only comments.** No non-ASCII characters in source.
- **No `/fp:fast`** in engine builds (determinism rule).
- **ABI:** this arc adds engine files and changes host boot. It does **not** change `EngineContext`, the plugin entry points, or header-only component layout — so **do NOT bump `kGamePluginABIVersion`** (currently 9). If you believe you must, stop and re-read what actually changed.
- **Engine sources are globbed** (`Arcane/premake5.lua:171-174`). New files under `Arcane/Arcane/src/` require re-running `GenerateProjects.bat`.
- **Editor sources are NOT globbed into tests** — anything under `ArcaneEditor/src/` a test drives must be listed explicitly in the `ArcaneTests` `files{}` block.
- **Run tests FROM THE EXE DIR.**
- **ArcaneTests runs in RANDOM order.** Capture the seed; reproduce with `--rng-seed N`.
- **Never construct a bare `Arcane::Runtime` in a test** — use `Arcane::Test::SharedTypeContext()` (see `Tests/src/ShaderEditorDocumentTest.cpp`).
- **Build with the VS 2026 (v18) msbuild:** `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`. The `msbuild` first on PATH may be older.
- **The editor holds file locks while running.** If a build fails with `LNK1168` or `MSB3073` on a copy into `ArcaneEditor/`, close the running editor. To gate without closing it, build `/t:ArcaneTests`.

**Commands:**

```bat
cd Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal

cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[threading]"
ArcaneTests.exe "[boot]"
ArcaneTests.exe "[shadercompile]"
ArcaneTests.exe ~[gpu]
```

**Baseline:** run `ArcaneTests.exe ~[gpu]` before Task 1 and record the counts. Current reference: **31082 assertions / 639 test cases**. Every task reports its delta.

---

## File Structure

**Create (engine):**
- `Arcane/Arcane/src/Arcane/Base/ServiceThread.{hpp,cpp}` — thread lifetime for long-lived blocking workers.
- `Arcane/Arcane/src/Arcane/Host/BootSequence.{hpp,cpp}` — the DAG scheduler. Zero GPU/window/ImGui.
- `Arcane/Arcane/src/Arcane/Host/BootPresenter.{hpp,cpp}` — one loading frame through the real swapchain.
- `Arcane/Arcane/src/Arcane/Host/BootSplashWindow.{hpp,cpp}` — pre-device OS splash (Windows; no-op elsewhere).

**Modify (engine):**
- `Arcane/Arcane/src/Arcane/Host/HostBoot.hpp` / `ProjectBoot.hpp` — add `CoreStages()`.
- `Arcane/Arcane/src/Arcane/Render/ShaderCompiler.cpp` — retrofit onto `ServiceThread`.
- `Arcane/Arcane/src/Arcane/Platform/Window.{hpp,cpp}` — add `Show()`.

**Modify (hosts):**
- `Arcane/ArcaneEditor/src/EditorApp.{hpp,cpp}`, `EditorAppProject.cpp`, `main.cpp`.
- `Arcane/ArcaneRuntime/src/RuntimeApp.{hpp,cpp}`, `main.cpp`.

**Create (tests):**
- `Arcane/Tests/src/ServiceThreadTest.cpp` — `[threading]`
- `Arcane/Tests/src/BootSequenceTest.cpp` — `[boot]`
- `Arcane/Tests/src/BootStageParityTest.cpp` — `[boot]` — **the bug-class gate**

---

## Task 1: `Arcane::ServiceThread`

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Base/ServiceThread.hpp`, `.cpp`
- Test: `Arcane/Tests/src/ServiceThreadTest.cpp`

**Interfaces:**
- Produces: `Arcane::ServiceThread(std::string debugName, std::function<void()> main, std::function<void()> wake = {})`, `~ServiceThread()`, `void RequestStop() noexcept`, `[[nodiscard]] bool StopRequested() const noexcept`, `[[nodiscard]] const std::string& DebugName() const noexcept`. Tasks 2 and 4 consume these.

**Design note — read before writing.** This owns **thread lifetime only**, not a work queue. `ShaderCompiler`'s `std::deque<Job>` under its own mutex/cv **is** its debounce-and-coalesce machinery (`ShaderCompiler.cpp:415-421`); a generic FIFO would destroy it. So `main` is the consumer's whole loop body, and `wake` is how we unblock a consumer sleeping on its own condition variable.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/ServiceThreadTest.cpp`:

```cpp
// ServiceThread: lifetime for long-lived BLOCKING workers (compile, build,
// file IO). Not for fork-join compute -- that is JobSystem. CPU-only.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/ServiceThread.hpp>

TEST_CASE("ServiceThread runs its body on another thread", "[threading]")
{
    const std::thread::id callerId = std::this_thread::get_id();
    std::atomic<bool> ran{false};
    std::atomic<bool> sameThread{true};
    {
        Arcane::ServiceThread svc("test.runs", [&]
        {
            sameThread = (std::this_thread::get_id() == callerId);
            ran = true;
        });
        // Destructor joins, so by the closing brace the body has completed.
    }
    CHECK(ran.load());
    CHECK_FALSE(sameThread.load());
}

TEST_CASE("StopRequested is observable from the body and the destructor joins", "[threading]")
{
    std::atomic<int> ticks{0};
    std::atomic<const Arcane::ServiceThread*> self{nullptr};
    {
        Arcane::ServiceThread svc("test.stopflag", [&]
        {
            while (!self.load()) std::this_thread::yield();   // wait for construction to publish
            while (!self.load()->StopRequested())
            {
                ++ticks;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        self = &svc;
        while (ticks.load() < 3) std::this_thread::yield();
    }   // dtor: RequestStop + join
    const int settled = ticks.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(ticks.load() == settled);   // truly joined, not detached
}

TEST_CASE("wake unblocks a body sleeping on its own condition variable", "[threading]")
{
    // This is the ShaderCompiler shape: the consumer owns the mutex/cv/queue,
    // ServiceThread owns only the stop flag and the join.
    std::mutex mx;
    std::condition_variable cv;
    std::atomic<bool> finished{false};
    std::atomic<const Arcane::ServiceThread*> self{nullptr};

    {
        Arcane::ServiceThread svc("test.wake",
            [&]
            {
                while (!self.load()) std::this_thread::yield();
                std::unique_lock lk(mx);
                cv.wait(lk, [&] { return self.load()->StopRequested(); });
                finished = true;
            },
            [&]
            {
                std::lock_guard lk(mx);   // lock before notify: the waiter
                cv.notify_all();          // re-checks the predicate under it
            });
        self = &svc;
    }   // dtor: RequestStop -> wake -> join. Hangs forever if wake is broken.

    CHECK(finished.load());
}

TEST_CASE("ServiceThread with no wake callback still destructs cleanly", "[threading]")
{
    Arcane::ServiceThread svc("test.nowake", [] {});
    CHECK(svc.DebugName() == "test.nowake");
}
```

- [ ] **Step 2: Run to verify it fails**

```bat
cd Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m /v:minimal
```

Expected: **compile error** — `Cannot open include file: 'Arcane/Base/ServiceThread.hpp'`.

- [ ] **Step 3: Write the header**

Create `Arcane/Arcane/src/Arcane/Base/ServiceThread.hpp`:

```cpp
#pragma once

// ServiceThread: one dedicated thread for LONG-LIVED BLOCKING work -- shader
// compiles, msbuild waits, file IO. It owns thread lifetime and a stop flag,
// and NOTHING else.
//
// THIS IS NOT THE FORK-JOIN POOL. Compute parallelism (ECS iteration, physics)
// goes to JobSystem (enkiTS), whose workers must never block -- JobSystem.hpp
// calls it "the only thread source for the simulation", and a blocked worker
// starves it. Never submit blocking work there. Two mechanisms, one rule.
//
// Deliberately NOT a work queue. ShaderCompiler's std::deque<Job> under its own
// mutex/cv IS its debounce-and-coalesce machinery; a generic FIFO would destroy
// it. So `main` is the consumer's entire loop body and it owns whatever queue it
// needs; `wake` exists so the destructor can unblock a body sleeping on the
// consumer's own condition variable.

#include <Arcane/Base/Api.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace Arcane
{
    class ARCANE_API ServiceThread
    {
    public:
        // `main` runs once, on the new thread; a service loops inside it until
        // StopRequested(). `wake` is invoked by RequestStop BEFORE the join --
        // it must take the same lock the body waits under, then notify.
        ServiceThread(std::string debugName,
                      std::function<void()> main,
                      std::function<void()> wake = {});

        // Always stops and joins. Never detaches.
        ~ServiceThread();

        ServiceThread(const ServiceThread&)            = delete;
        ServiceThread& operator=(const ServiceThread&) = delete;
        ServiceThread(ServiceThread&&)                 = delete;
        ServiceThread& operator=(ServiceThread&&)      = delete;

        // Idempotent. Sets the flag, then calls `wake`.
        void RequestStop() noexcept;

        [[nodiscard]] bool StopRequested() const noexcept { return m_stop.load(std::memory_order_acquire); }
        [[nodiscard]] const std::string& DebugName() const noexcept { return m_debugName; }

    private:
        std::string           m_debugName;
        std::function<void()> m_wake;
        std::atomic<bool>     m_stop{false};
        std::thread           m_thread;
    };
}
```

- [ ] **Step 4: Write the implementation**

Create `Arcane/Arcane/src/Arcane/Base/ServiceThread.cpp`:

```cpp
#include <Arcane/Base/ServiceThread.hpp>

#include <Arcane/Base/Log.hpp>

#include <utility>

namespace Arcane
{
    ServiceThread::ServiceThread(std::string debugName,
                                 std::function<void()> main,
                                 std::function<void()> wake)
        : m_debugName(std::move(debugName)), m_wake(std::move(wake))
    {
        m_thread = std::thread([this, body = std::move(main)]
        {
            if (body) body();
        });
    }

    void ServiceThread::RequestStop() noexcept
    {
        // Release so a body that reads StopRequested() with acquire sees every
        // write made before the stop was requested.
        m_stop.store(true, std::memory_order_release);
        if (m_wake)
        {
            // The callback is expected to lock the consumer's mutex and notify.
            // Swallow rather than propagate: this runs from the destructor.
            try { m_wake(); }
            catch (...) { ARC_ERROR("ServiceThread '{}': wake callback threw", m_debugName); }
        }
    }

    ServiceThread::~ServiceThread()
    {
        RequestStop();
        if (m_thread.joinable())
            m_thread.join();
    }
}
```

- [ ] **Step 5: Run the tests**

```bat
cd Arcane
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[threading]"
```

Expected: PASS, 4 test cases. If the `wake` case **hangs**, `wake` is not being called before the join — that is the bug it exists to catch.

- [ ] **Step 6: Full gate + commit**

```bat
ArcaneTests.exe ~[gpu]
```

```bash
git add Arcane/Arcane/src/Arcane/Base/ServiceThread.hpp Arcane/Arcane/src/Arcane/Base/ServiceThread.cpp Arcane/Tests/src/ServiceThreadTest.cpp
git commit -m "feat(arcane): Arcane::ServiceThread -- one shape for long-lived blocking workers"
```

---

## Task 2: Retrofit `ShaderCompiler` onto `ServiceThread`

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Render/ShaderCompiler.cpp:415-421` (members), `:463-472` (`WorkerMain`), `:598-625` (start/stop)

**Interfaces:**
- Consumes: `Arcane::ServiceThread` from Task 1.
- Produces: nothing new. **`ShaderCompiler`'s public API and observable behaviour must not change at all.**

**This task is the honest test of Task 1.** If `ShaderCompiler` cannot adopt `ServiceThread` without distorting, the abstraction is wrong. **The escape hatch is real:** if the retrofit fights the debounce/superseded machinery, revert this task, leave `ShaderCompiler` as it was, and continue — `ServiceThread` still earns its keep in Task 4. Do not force it, and do not edit a `[shadercompile]` test to make it pass.

- [ ] **Step 1: Record the pre-change baseline**

```bat
cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[shadercompile]"
```

Write down the assertion and test-case counts. This exact result must reproduce after the change.

- [ ] **Step 2: Replace the raw thread member**

In `ShaderCompiler.cpp`, add `#include <Arcane/Base/ServiceThread.hpp>` and `#include <optional>`. In `Impl` (`:415-421`), replace:

```cpp
        // Worker state.
        std::thread worker;
        std::mutex mx;
        std::condition_variable cv;
        std::deque<Job> queue;
        std::vector<ShaderCompileResult> done;
        int inflight = 0;      // guarded by mx
        bool stop = false;
```

with:

```cpp
        // Worker state. The queue/mutex/cv stay HERE -- they are the debounce
        // and coalescing machinery, not generic plumbing. ServiceThread owns
        // only the thread's lifetime and the stop flag.
        std::optional<ServiceThread> worker;
        std::mutex mx;
        std::condition_variable cv;
        std::deque<Job> queue;
        std::vector<ShaderCompileResult> done;
        int inflight = 0;      // guarded by mx
```

Note `bool stop` is gone — `ServiceThread::StopRequested()` replaces it.

- [ ] **Step 3: Point the wait predicate at the new flag**

In `WorkerMain` (`:463-472`), replace the wait and the check:

```cpp
                    cv.wait(lk, [this] { return stop || !queue.empty(); });
                    if (stop)
```

with:

```cpp
                    cv.wait(lk, [this] { return worker->StopRequested() || !queue.empty(); });
                    if (worker->StopRequested())
```

`worker` is engaged before the thread body can run (Step 4 constructs it in place), so the optional is never empty here.

- [ ] **Step 4: Construct it instead of the raw thread**

Replace `:603-604`:

```cpp
        im.stop = false;
        im.worker = std::thread([this] { m_impl->WorkerMain(); });
```

with:

```cpp
        im.worker.emplace("shader.compile",
                          [this] { m_impl->WorkerMain(); },
                          [this] { std::lock_guard lk(m_impl->mx); m_impl->cv.notify_all(); });
```

The wake callback takes `mx` before notifying, so a worker about to evaluate the predicate cannot miss the stop.

- [ ] **Step 5: Simplify Shutdown**

Replace the body of `ShaderCompiler::Shutdown`'s stop block (`:614-621`):

```cpp
        if (im.worker.joinable())
        {
            {
                std::lock_guard lk(im.mx);
                im.stop = true;
            }
            im.cv.notify_all();
            im.worker.join();
        }
```

with:

```cpp
        // ~ServiceThread requests stop, fires the wake callback (which takes mx
        // and notifies cv), and joins. Resetting the optional runs it here.
        im.worker.reset();
```

- [ ] **Step 6: Verify behaviour is unchanged**

```bat
cd Arcane
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[shadercompile]"
ArcaneTests.exe ~[gpu]
```

Expected: `[shadercompile]` matches Step 1's counts **exactly**, with zero test edits. `~[gpu]` green.

- [ ] **Step 7: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Render/ShaderCompiler.cpp
git commit -m "refactor(arcane): ShaderCompiler adopts ServiceThread -- queue/debounce policy unchanged"
```

---

## Task 3: `Window::Show()` and hidden creation

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Platform/Window.hpp`, `Window.cpp`
- Modify: `Arcane/Arcane/src/Arcane/Host/GpuContext.cpp:21-24`

**Interfaces:**
- Produces: `void Window::Show()`. Tasks 7 and 8 call it.

`WindowDesc::hidden` already exists (`Window.hpp:22`, used by tests) but nothing can un-hide.

- [ ] **Step 1: Add the declaration**

In `Window.hpp`, beside the other window methods:

```cpp
        // Un-hide a window created with WindowDesc::hidden. Hosts create hidden
        // and show at the first presented frame, so a window never exists in an
        // undrawn state. Idempotent.
        void Show();
```

- [ ] **Step 2: Implement it**

In `Window.cpp`:

```cpp
    void Window::Show()
    {
        if (m_window) SDL_ShowWindow(m_window);
    }
```

Use whatever the file's existing SDL handle member is called — read the surrounding methods and match; do not rename anything.

- [ ] **Step 3: Create hidden**

In `GpuContext.cpp:21-24`, add `wd.hidden = true;` beside the other `WindowDesc` fields:

```cpp
        WindowDesc wd;
        wd.title  = "Arcane Runtime";
        wd.vulkan = (cfg.backend == GraphicsBackend::Vulkan);
        // Hidden until the first presented frame (Task 8's splash_ready stage
        // calls Show()). A window that exists but has never been drawn is the
        // black rectangle this arc exists to remove.
        wd.hidden = true;
```

- [ ] **Step 4: Build and gate**

```bat
cd Arcane
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe ~[gpu]
```

Expected: green. **The editor will now open with no visible window until Task 8 lands** — that is expected and temporary. Note it in the commit so nobody bisects it.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Platform/Window.hpp Arcane/Arcane/src/Arcane/Platform/Window.cpp Arcane/Arcane/src/Arcane/Host/GpuContext.cpp
git commit -m "feat(arcane): Window::Show + hosts create hidden (window invisible until Task 8 shows it)"
```

---

## Task 4: `BootSequence` — the DAG scheduler

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Host/BootSequence.hpp`, `.cpp`
- Test: `Arcane/Tests/src/BootSequenceTest.cpp`

**Interfaces:**
- Consumes: `Arcane::ServiceThread` (Task 1).
- Produces: `Arcane::BootStage`, `Arcane::BootThread`, `Arcane::BootPolicy`, `Arcane::BootProgress`, `Arcane::IBootPresenter`, `Arcane::BootResult`, `Arcane::BootSequence`. Tasks 5-9 consume these.

**Zero GPU, window, or ImGui includes may enter `BootSequence.hpp`** — that is what keeps it headless-testable and Core-promotable (spec §7).

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/BootSequenceTest.cpp`:

```cpp
// BootSequence: the boot-stage DAG. Pure scheduler, no GPU/window/ImGui.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/BootSequence.hpp>

namespace
{
    Arcane::BootStage Stage(std::string id, std::vector<std::string> deps,
                            std::function<bool()> run,
                            Arcane::BootThread thread = Arcane::BootThread::Main,
                            Arcane::BootPolicy policy = Arcane::BootPolicy::Fatal)
    {
        Arcane::BootStage s;
        s.id = std::move(id);
        s.dependsOn = std::move(deps);
        s.thread = thread;
        s.policy = policy;
        s.weight = 1;
        s.run = std::move(run);
        return s;
    }
}

TEST_CASE("stages run in dependency order", "[boot]")
{
    std::vector<std::string> order;
    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("c", {"b"}, [&] { order.push_back("c"); return true; }));
    stages.push_back(Stage("a", {},    [&] { order.push_back("a"); return true; }));
    stages.push_back(Stage("b", {"a"}, [&] { order.push_back("b"); return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(nullptr);

    REQUIRE(r.ok);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == "a");
    CHECK(order[1] == "b");
    CHECK(order[2] == "c");
}

TEST_CASE("a worker stage genuinely overlaps a main stage", "[boot]")
{
    // Proof, not assumption: the worker blocks until the main stage signals.
    // If they were serialised this deadlocks and the test times out.
    std::atomic<bool> mainRan{false};
    std::atomic<bool> workerSawOverlap{false};

    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("worker", {}, [&]
    {
        while (!mainRan.load()) std::this_thread::yield();
        workerSawOverlap = true;
        return true;
    }, Arcane::BootThread::Worker));
    stages.push_back(Stage("main", {}, [&]
    {
        mainRan = true;
        return true;
    }, Arcane::BootThread::Main));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(nullptr);

    CHECK(r.ok);
    CHECK(workerSawOverlap.load());
}

TEST_CASE("a dependency cycle is refused and names the offenders", "[boot]")
{
    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("x", {"y"}, [] { return true; }));
    stages.push_back(Stage("y", {"x"}, [] { return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(nullptr);

    CHECK_FALSE(r.ok);
    CHECK(r.failedStage.find("x") != std::string::npos);
}

TEST_CASE("an Optional stage's failure lets dependents run", "[boot]")
{
    bool dependentRan = false;
    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("opt", {}, [] { return false; },
                           Arcane::BootThread::Main, Arcane::BootPolicy::Optional));
    stages.push_back(Stage("dep", {"opt"}, [&] { dependentRan = true; return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(nullptr);

    CHECK(r.ok);              // Optional failure does not fail the boot
    CHECK(dependentRan);
}

TEST_CASE("a Fatal stage's failure aborts and skips dependents", "[boot]")
{
    bool dependentRan = false;
    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("bad", {}, [] { return false; }));
    stages.push_back(Stage("dep", {"bad"}, [&] { dependentRan = true; return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(nullptr);

    CHECK_FALSE(r.ok);
    CHECK(r.failedStage == "bad");
    CHECK_FALSE(dependentRan);
}

TEST_CASE("duplicate stage ids are refused", "[boot]")
{
    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("dup", {}, [] { return true; }));
    stages.push_back(Stage("dup", {}, [] { return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(nullptr);

    CHECK_FALSE(r.ok);
    CHECK(r.failedStage.find("dup") != std::string::npos);
}

TEST_CASE("progress reaches 1.0 and never goes backwards", "[boot]")
{
    struct Recorder final : Arcane::IBootPresenter
    {
        float last = 0.0f;
        bool monotonic = true;
        bool Present(const Arcane::BootProgress& p) override
        {
            if (p.fraction < last) monotonic = false;
            last = p.fraction;
            return true;   // do not request quit
        }
    } rec;

    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("a", {},    [] { return true; }));
    stages.push_back(Stage("b", {"a"}, [] { return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(&rec);

    CHECK(r.ok);
    CHECK(rec.monotonic);
    CHECK(rec.last == 1.0f);
}

TEST_CASE("a presenter requesting quit aborts the boot cleanly", "[boot]")
{
    struct Quitter final : Arcane::IBootPresenter
    {
        bool Present(const Arcane::BootProgress&) override { return false; }
    } quitter;

    std::atomic<int> ran{0};
    std::vector<Arcane::BootStage> stages;
    for (int i = 0; i < 8; ++i)
        stages.push_back(Stage("s" + std::to_string(i), {}, [&] { ++ran; return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(&quitter);

    CHECK_FALSE(r.ok);
    CHECK(r.quitRequested);
    CHECK(ran.load() < 8);   // stopped early
}
```

- [ ] **Step 2: Run to verify it fails**

```bat
cd Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m /v:minimal
```

Expected: **compile error** — `Cannot open include file: 'Arcane/Host/BootSequence.hpp'`.

- [ ] **Step 3: Write the header**

Create `Arcane/Arcane/src/Arcane/Host/BootSequence.hpp`:

```cpp
#pragma once

// BootSequence: the boot-stage DAG both hosts run. A pure scheduler over
// callables plus a progress model -- ZERO GPU, window, and ImGui dependency, by
// design (all presentation lives behind IBootPresenter). Keeping it that way is
// what makes it headless-testable and promotable to Core later; do not add an
// NVRHI, SDL, or ImGui include to this header.
//
// Ordering is a stable Kahn topological sort, the same shape as TopoSortPasses
// in the material pass chain.
//
// The DAG exists for exactly ONE overlap: the filesystem-bound project open
// against GPU device creation. Sequential-and-join is the safer default (it is
// what Unreal does); every new Worker stage owes its own disjoint-ownership
// proof before it is added.

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Arcane
{
    enum class BootThread : std::uint8_t { Main, Worker };

    // Fatal: failure aborts the boot and skips dependents.
    // Optional: failure is recorded, dependents still run. This exists because
    // OpenProject failing is NOT fatal today -- the host warns and continues
    // with the data/ + --plugin fallback. Preserve that, do not tighten it.
    enum class BootPolicy : std::uint8_t { Fatal, Optional };

    struct BootStage
    {
        std::string              id;
        std::vector<std::string> dependsOn;
        BootThread               thread = BootThread::Main;
        BootPolicy               policy = BootPolicy::Fatal;
        std::uint32_t            weight = 1;      // share of the progress bar
        std::function<bool()>    run;             // false == this stage failed
    };

    struct BootProgress
    {
        float       fraction = 0.0f;   // 0..1, monotonic
        std::string stageId;           // the stage being reported
        std::string detail;            // optional sub-progress, e.g. "412 / 1180"
    };

    // The presentation seam. Return false to request an abort (window closed).
    struct ARCANE_API IBootPresenter
    {
        virtual ~IBootPresenter() = default;
        virtual bool Present(const BootProgress& progress) = 0;
    };

    struct BootResult
    {
        bool        ok            = false;
        std::string failedStage;          // empty when ok
        bool        quitRequested = false;
    };

    class ARCANE_API BootSequence
    {
    public:
        explicit BootSequence(std::vector<BootStage> stages);
        ~BootSequence();

        BootSequence(const BootSequence&)            = delete;
        BootSequence& operator=(const BootSequence&) = delete;

        // Drives every stage to completion. `presenter` may be null (headless).
        BootResult Run(IBootPresenter* presenter);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
```

- [ ] **Step 4: Write the implementation**

Create `Arcane/Arcane/src/Arcane/Host/BootSequence.cpp`:

```cpp
#include <Arcane/Host/BootSequence.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/ServiceThread.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Arcane
{
    struct BootSequence::Impl
    {
        std::vector<BootStage> stages;

        // Worker handoff. One worker stage runs at a time by construction.
        std::mutex              mx;
        std::condition_variable cv;
        int                     pending  = -1;    // index queued for the worker
        int                     finished = -1;    // index the worker completed
        bool                    workerOk = true;
    };

    BootSequence::BootSequence(std::vector<BootStage> stages)
        : m_impl(std::make_unique<Impl>())
    {
        m_impl->stages = std::move(stages);
    }

    BootSequence::~BootSequence() = default;

    BootResult BootSequence::Run(IBootPresenter* presenter)
    {
        Impl& im = *m_impl;
        BootResult result;

        // ---- validate: unique ids -------------------------------------------
        std::unordered_map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < im.stages.size(); ++i)
        {
            if (!index.emplace(im.stages[i].id, i).second)
            {
                result.failedStage = "duplicate stage id '" + im.stages[i].id + "'";
                ARC_ERROR("BootSequence: {}", result.failedStage);
                return result;
            }
        }

        // ---- validate: dependencies resolve ---------------------------------
        for (const BootStage& s : im.stages)
            for (const std::string& d : s.dependsOn)
                if (!index.count(d))
                {
                    result.failedStage = "stage '" + s.id + "' depends on unknown '" + d + "'";
                    ARC_ERROR("BootSequence: {}", result.failedStage);
                    return result;
                }

        const std::size_t n = im.stages.size();
        std::vector<int>  remaining(n, 0);
        for (std::size_t i = 0; i < n; ++i)
            remaining[i] = static_cast<int>(im.stages[i].dependsOn.size());

        std::vector<bool> done(n, false), skipped(n, false), started(n, false);

        std::uint64_t totalWeight = 0;
        for (const BootStage& s : im.stages) totalWeight += s.weight;
        if (totalWeight == 0) totalWeight = 1;
        std::uint64_t doneWeight = 0;

        std::optional<ServiceThread> worker;
        int workerRunning = -1;

        auto ready = [&](std::size_t i)
        {
            return !done[i] && !skipped[i] && !started[i] && remaining[i] == 0;
        };

        auto complete = [&](std::size_t i, bool ok)
        {
            done[i] = true;
            doneWeight += im.stages[i].weight;
            if (!ok && im.stages[i].policy == BootPolicy::Fatal)
                return false;
            if (!ok)
                ARC_WARN("BootSequence: optional stage '{}' failed; continuing", im.stages[i].id);
            // Release dependents regardless: an Optional failure must not strand
            // them (OpenProject failing still lets the host boot project-less).
            for (std::size_t j = 0; j < n; ++j)
                for (const std::string& d : im.stages[j].dependsOn)
                    if (d == im.stages[i].id) --remaining[j];
            return true;
        };

        auto present = [&](const std::string& stageId)
        {
            if (!presenter) return true;
            BootProgress p;
            p.fraction = static_cast<float>(static_cast<double>(doneWeight) /
                                            static_cast<double>(totalWeight));
            p.stageId  = stageId;
            return presenter->Present(p);
        };

        std::size_t completed = 0;
        while (completed < n)
        {
            // 1. Dispatch a worker-eligible stage FIRST so the worker is never
            //    idle while main chews on a long stage. This is what produces
            //    the project_open / gpu_core overlap.
            if (workerRunning < 0)
            {
                for (std::size_t i = 0; i < n; ++i)
                {
                    if (ready(i) && im.stages[i].thread == BootThread::Worker)
                    {
                        started[i]    = true;
                        workerRunning = static_cast<int>(i);
                        {
                            std::lock_guard lk(im.mx);
                            im.pending  = workerRunning;
                            im.finished = -1;
                        }
                        if (!worker)
                        {
                            worker.emplace("boot.worker",
                                [&im]
                                {
                                    for (;;)
                                    {
                                        int job = -1;
                                        {
                                            std::unique_lock lk(im.mx);
                                            im.cv.wait(lk, [&] { return im.pending >= 0; });
                                            job = im.pending;
                                            im.pending = -1;
                                        }
                                        if (job < 0) return;   // stop sentinel
                                        bool ok = true;
                                        try { ok = im.stages[static_cast<std::size_t>(job)].run(); }
                                        catch (...) { ok = false; }
                                        {
                                            std::lock_guard lk(im.mx);
                                            im.workerOk = ok;
                                            im.finished = job;
                                        }
                                        im.cv.notify_all();
                                    }
                                },
                                [&im]
                                {
                                    std::lock_guard lk(im.mx);
                                    im.pending = -1;      // sentinel: exit the loop
                                    im.cv.notify_all();
                                });
                        }
                        im.cv.notify_all();
                        break;
                    }
                }
            }

            // 2. Run ONE ready main stage, in registration order.
            bool didMain = false;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (ready(i) && im.stages[i].thread == BootThread::Main)
                {
                    started[i] = true;
                    bool ok = true;
                    try { ok = im.stages[i].run(); }
                    catch (...) { ok = false; }
                    if (!complete(i, ok))
                    {
                        result.failedStage = im.stages[i].id;
                        ARC_ERROR("BootSequence: fatal stage '{}' failed", result.failedStage);
                        return result;
                    }
                    ++completed;
                    didMain = true;
                    break;
                }
            }

            if (!present(didMain ? std::string("main") : std::string("waiting")))
            {
                result.quitRequested = true;
                result.failedStage   = "quit requested";
                return result;
            }

            // 3. Reap the worker if it finished.
            if (workerRunning >= 0)
            {
                int fin = -1; bool ok = true;
                {
                    std::unique_lock lk(im.mx);
                    if (!didMain)
                        im.cv.wait(lk, [&] { return im.finished >= 0; });   // nothing else to do
                    fin = im.finished;
                    ok  = im.workerOk;
                    if (fin >= 0) im.finished = -1;
                }
                if (fin >= 0)
                {
                    if (!complete(static_cast<std::size_t>(fin), ok))
                    {
                        result.failedStage = im.stages[static_cast<std::size_t>(fin)].id;
                        ARC_ERROR("BootSequence: fatal worker stage '{}' failed", result.failedStage);
                        return result;
                    }
                    ++completed;
                    workerRunning = -1;
                }
            }
            else if (!didMain && completed < n)
            {
                // Nothing ready and nothing running: a cycle.
                std::string names;
                for (std::size_t i = 0; i < n; ++i)
                    if (!done[i]) { if (!names.empty()) names += ", "; names += im.stages[i].id; }
                result.failedStage = "dependency cycle among: " + names;
                ARC_ERROR("BootSequence: {}", result.failedStage);
                return result;
            }
        }

        present("done");
        result.ok = true;
        return result;
    }
}
```

- [ ] **Step 5: Run the tests**

```bat
cd Arcane
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[boot]"
```

Expected: PASS, 8 test cases. The final `present("done")` is what makes the progress test see exactly `1.0f`.

- [ ] **Step 6: Full gate + commit**

```bat
ArcaneTests.exe ~[gpu]
```

```bash
git add Arcane/Arcane/src/Arcane/Host/BootSequence.hpp Arcane/Arcane/src/Arcane/Host/BootSequence.cpp Arcane/Tests/src/BootSequenceTest.cpp
git commit -m "feat(arcane): BootSequence -- boot-stage DAG with worker overlap and failure policies"
```

---

## Task 5: `HostBoot::CoreStages()` and the parity gate

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Host/ProjectBoot.hpp` (this is where `HostBoot` lives)
- Test: `Arcane/Tests/src/BootStageParityTest.cpp`

**Interfaces:**
- Consumes: `BootStage`, `BootThread`, `BootPolicy` (Task 4).
- Produces: `Arcane::HostBoot::BootContext`, `Arcane::HostBoot::CoreStages(BootContext&)`, `Arcane::HostBoot::CoreStageIds()`. Tasks 8 and 9 consume them.

**This is the task the whole arc exists for.** Three bugs (camera, sprite tables, Astra TypeContext) came from the editor doing a boot step the runtime didn't. A shared *mechanism* would not have prevented any of them; a shared *list* does.

- [ ] **Step 1: Write the failing parity test**

Create `Arcane/Tests/src/BootStageParityTest.cpp`:

```cpp
// THE BUG-CLASS GATE. Three shipped bugs came from EditorApp::Init performing a
// step RuntimeApp did not. This asserts both hosts take every core stage.
//
// Honest limit: this proves both hosts RUN the same stages, not that a stage's
// body behaves identically in both modules. The Astra TypeContext bug was a
// per-module static slot, which is why type_context_install also carries a
// runtime VerifySharedTypeContext check. The two cover different halves.

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/BootSequence.hpp>
#include <Arcane/Host/ProjectBoot.hpp>

TEST_CASE("CoreStages ids are unique", "[boot]")
{
    std::vector<std::string> ids = Arcane::HostBoot::CoreStageIds();
    std::vector<std::string> sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    CHECK_FALSE(ids.empty());
}

TEST_CASE("CoreStages declares no dependency it does not contain", "[boot]")
{
    Arcane::HostBoot::BootContext ctx{};
    const std::vector<Arcane::BootStage> core = Arcane::HostBoot::CoreStages(ctx);
    std::vector<std::string> ids;
    for (const Arcane::BootStage& s : core) ids.push_back(s.id);

    for (const Arcane::BootStage& s : core)
        for (const std::string& d : s.dependsOn)
            CHECK(std::find(ids.begin(), ids.end(), d) != ids.end());
}

TEST_CASE("both hosts contain every core stage", "[boot]")
{
    // Built the same way the hosts build theirs: CoreStages, then appends.
    Arcane::HostBoot::BootContext ctx{};
    const std::vector<std::string> core = Arcane::HostBoot::CoreStageIds();

    const std::vector<std::string> editorIds  = Arcane::HostBoot::EditorStageIdsForTest(ctx);
    const std::vector<std::string> runtimeIds = Arcane::HostBoot::RuntimeStageIdsForTest(ctx);

    for (const std::string& id : core)
    {
        INFO("core stage missing from a host: " << id);
        CHECK(std::find(editorIds.begin(),  editorIds.end(),  id) != editorIds.end());
        CHECK(std::find(runtimeIds.begin(), runtimeIds.end(), id) != runtimeIds.end());
    }
}

TEST_CASE("an appended host stage cannot shadow a core id", "[boot]")
{
    // A duplicate id would silently replace a core stage. BootSequence refuses
    // duplicates at Run(), but catch it here where the message is clearer.
    Arcane::HostBoot::BootContext ctx{};
    const std::vector<std::string> core = Arcane::HostBoot::CoreStageIds();

    for (const std::vector<std::string>& hostIds :
         { Arcane::HostBoot::EditorStageIdsForTest(ctx),
           Arcane::HostBoot::RuntimeStageIdsForTest(ctx) })
    {
        std::vector<std::string> sorted = hostIds;
        std::sort(sorted.begin(), sorted.end());
        CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    }
}

TEST_CASE("the runtime host appends nothing today", "[boot]")
{
    // If this ever legitimately changes, change it deliberately HERE -- do not
    // weaken it in passing.
    Arcane::HostBoot::BootContext ctx{};
    CHECK(Arcane::HostBoot::RuntimeStageIdsForTest(ctx) == Arcane::HostBoot::CoreStageIds());
}
```

- [ ] **Step 2: Run to verify it fails**

Expected: **compile error** — `CoreStageIds` / `BootContext` are not members of `Arcane::HostBoot`.

- [ ] **Step 3: Add the contract to `ProjectBoot.hpp`**

Append inside `namespace Arcane::HostBoot`:

```cpp
    // What a boot stage needs to do its work. Pointers are host-owned and
    // outlive the sequence; null members mean "that facility is absent in this
    // host", which stages must tolerate (the parity tests build one with all
    // members null).
    struct BootContext
    {
        class Runtime*          runtime     = nullptr;
        class GpuContext*       gpu         = nullptr;
        class BootSplashWindow* splash      = nullptr;   // pre-device splash; closed by splash_ready
        const char*             projectPath = nullptr;
        const char*             pluginPath  = nullptr;
    };

    // THE CANONICAL BOOT SEQUENCE. Both hosts take this list WHOLE.
    //
    // A host may APPEND its own stages. It may NOT omit, reorder, or rewrite one
    // -- so divergence between the editor and the runtime has to be written
    // deliberately instead of forgotten. Three shipped bugs (camera, sprite
    // tables, Astra TypeContext) were exactly that forgetting.
    //
    // Adding an engine-wide install/publish step? Add it HERE and both hosts get
    // it. BootStageParityTest fails if a host drops one.
    [[nodiscard]] std::vector<BootStage> CoreStages(BootContext& ctx);

    // Ids only -- no context needed, so tests and tooling can ask "what is the
    // canonical list?" without constructing a host.
    [[nodiscard]] std::vector<std::string> CoreStageIds();

    // Exactly what each host builds, exposed for BootStageParityTest. These must
    // be the SAME functions the hosts call, not reimplementations -- a parallel
    // copy would test itself and prove nothing.
    [[nodiscard]] std::vector<BootStage> EditorStages(BootContext& ctx);
    [[nodiscard]] std::vector<BootStage> RuntimeStages(BootContext& ctx);
    [[nodiscard]] std::vector<std::string> EditorStageIdsForTest(BootContext& ctx);
    [[nodiscard]] std::vector<std::string> RuntimeStageIdsForTest(BootContext& ctx);
```

Add `#include <Arcane/Host/BootSequence.hpp>` and `#include <vector>` / `#include <string>` to the header.

- [ ] **Step 4: Implement in a new `ProjectBoot.cpp` (or the existing Host TU)**

```cpp
namespace Arcane::HostBoot
{
    namespace
    {
        BootStage Make(std::string id, std::vector<std::string> deps,
                       BootThread thread, BootPolicy policy, std::uint32_t weight,
                       std::function<bool()> run)
        {
            BootStage s;
            s.id = std::move(id);
            s.dependsOn = std::move(deps);
            s.thread = thread;
            s.policy = policy;
            s.weight = weight;
            s.run = std::move(run);
            return s;
        }
    }

    std::vector<BootStage> CoreStages(BootContext& ctx)
    {
        std::vector<BootStage> s;
        // Bodies are wired in Task 8. A null ctx member means the facility is
        // absent (the parity tests construct an all-null context), so every body
        // must tolerate that and return true rather than crash.
        s.push_back(Make("runtime_create",       {},                                  BootThread::Main,   BootPolicy::Fatal,     5, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("type_context_install", {"runtime_create"},                  BootThread::Main,   BootPolicy::Fatal,     1, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("gpu_core",             {},                                  BootThread::Main,   BootPolicy::Fatal,    25, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("project_open",         {"runtime_create"},                  BootThread::Worker, BootPolicy::Optional, 45, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("render_bridge",        {"gpu_core", "runtime_create"},      BootThread::Main,   BootPolicy::Fatal,     3, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("input_config",         {"project_open", "gpu_core"},        BootThread::Main,   BootPolicy::Optional,  2, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("sprite_tables",        {"project_open", "render_bridge"},   BootThread::Main,   BootPolicy::Optional,  2, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("plugin_load",          {"project_open", "render_bridge"},   BootThread::Main,   BootPolicy::Fatal,     9, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("finalize",             {"plugin_load", "input_config",
                                                  "sprite_tables"},                   BootThread::Main,   BootPolicy::Fatal,     1, [&ctx] { (void)ctx; return true; }));
        return s;
    }

    std::vector<std::string> CoreStageIds()
    {
        BootContext ctx{};
        std::vector<std::string> ids;
        for (const BootStage& s : CoreStages(ctx)) ids.push_back(s.id);
        return ids;
    }

    std::vector<BootStage> RuntimeStages(BootContext& ctx)
    {
        return CoreStages(ctx);   // appends nothing -- that is the point
    }

    std::vector<BootStage> EditorStages(BootContext& ctx)
    {
        std::vector<BootStage> s = CoreStages(ctx);
        s.push_back(Make("editor_fonts",  {"gpu_core"},      BootThread::Main,   BootPolicy::Fatal,    5, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("splash_ready",  {"editor_fonts"},  BootThread::Main,   BootPolicy::Fatal,    2, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("editor_shell",  {"editor_fonts"},  BootThread::Main,   BootPolicy::Fatal,    3, [&ctx] { (void)ctx; return true; }));
        s.push_back(Make("editor_lock",   {"project_open"},  BootThread::Worker, BootPolicy::Optional, 1, [&ctx] { (void)ctx; return true; }));
        return s;
    }

    std::vector<std::string> EditorStageIdsForTest(BootContext& ctx)
    {
        std::vector<std::string> ids;
        for (const BootStage& s : EditorStages(ctx)) ids.push_back(s.id);
        return ids;
    }

    std::vector<std::string> RuntimeStageIdsForTest(BootContext& ctx)
    {
        std::vector<std::string> ids;
        for (const BootStage& s : RuntimeStages(ctx)) ids.push_back(s.id);
        return ids;
    }
}
```

Note `editor_lock` is a second Worker stage. `BootSequence` runs one worker stage at a time, so it simply queues after `project_open` — no extra machinery, and no second disjoint-ownership proof needed because they never overlap each other.

- [ ] **Step 5: Run the tests + full gate**

```bat
cd Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[boot]"
ArcaneTests.exe ~[gpu]
```

Expected: `[boot]` now 13 cases (8 from Task 4 + 5 here).

- [ ] **Step 6: Prove the gate actually bites**

Temporarily delete the `type_context_install` line from `CoreStages`, rebuild, and run `ArcaneTests.exe "[boot]"`. The parity test must FAIL. **Restore the line.** A gate never seen red is not known to work.

- [ ] **Step 7: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Host/ Arcane/Tests/src/BootStageParityTest.cpp
git commit -m "feat(arcane): HostBoot::CoreStages -- one canonical boot list both hosts must contain"
```

---

## Task 6: `BootPresenter`

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Host/BootPresenter.hpp`, `.cpp`

**Interfaces:**
- Consumes: `IBootPresenter`, `BootProgress` (Task 4); `GpuContext`.
- Produces: `Arcane::BootPresenter(GpuContext&, BootPresenterMode)`, `Arcane::BootPresenterMode::{Fullscreen, Overlay}`, `void SetSplashTexture(...)`, `void SetShowProgress(bool)`.

No unit test — it is pure ImGui/NVRHI, covered by desk-verify (spec §9). Everything it depends on was unit-tested in Task 4.

- [ ] **Step 1: Write the header**

```cpp
#pragma once

// BootPresenter: draws and presents exactly ONE loading frame per call, through
// the existing backbuffer + ImGuiLayer path rather than a bespoke render chain
// (homogenized-rendering mandate), so it inherits the editor theme for free.
//
// It cannot present before gpu_core completes -- that is what BootSplashWindow
// covers.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Host/BootSequence.hpp>

#include <cstdint>
#include <string>

namespace Arcane
{
    class GpuContext;

    enum class BootPresenterMode : std::uint8_t
    {
        Fullscreen,   // boot: the whole backbuffer
        Overlay,      // project switch: a modal panel over the last editor frame
    };

    class ARCANE_API BootPresenter final : public IBootPresenter
    {
    public:
        BootPresenter(GpuContext& gpu, BootPresenterMode mode);
        ~BootPresenter() override;

        // Returns false when the window was closed -- BootSequence turns that
        // into BootResult::quitRequested.
        bool Present(const BootProgress& progress) override;

        void SetShowProgress(bool show) noexcept { m_showProgress = show; }

    private:
        GpuContext&       m_gpu;
        BootPresenterMode m_mode;
        bool              m_showProgress = true;
        bool              m_shownWindow  = false;
    };
}
```

- [ ] **Step 2: Write the implementation**

```cpp
#include <Arcane/Host/BootPresenter.hpp>

#include <Arcane/Host/GpuContext.hpp>

#include <imgui.h>

namespace Arcane
{
    BootPresenter::BootPresenter(GpuContext& gpu, BootPresenterMode mode)
        : m_gpu(gpu), m_mode(mode) {}

    BootPresenter::~BootPresenter() = default;

    bool BootPresenter::Present(const BootProgress& progress)
    {
        const WindowEvents ev = m_gpu.Win().PumpEvents();
        if (ev.quitRequested) return false;
        if (ev.resized) m_gpu.OnResize(ev.width, ev.height);

        nvrhi::ITexture* backbuffer = m_gpu.Swap().BeginFrame();
        if (!backbuffer) return true;   // swapchain not ready; try again next call

        m_gpu.Imgui().BeginFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        if (m_mode == BootPresenterMode::Fullscreen)
        {
            ImGui::SetNextWindowPos(vp->Pos);
            ImGui::SetNextWindowSize(vp->Size);
        }
        else
        {
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                           vp->Pos.y + vp->Size.y * 0.5f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(420.0f, 120.0f));
        }
        ImGui::Begin("##bootsplash", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);

        if (m_showProgress)
        {
            ImGui::ProgressBar(progress.fraction, ImVec2(-1.0f, 0.0f));
            if (!progress.detail.empty())
                ImGui::TextDisabled("%s", progress.detail.c_str());
            else if (!progress.stageId.empty())
                ImGui::TextDisabled("%s", progress.stageId.c_str());
        }

        ImGui::End();

        m_gpu.Imgui().EndFrame(backbuffer);
        m_gpu.Swap().Present();
        return true;
    }
}
```

**Before writing this**, open `GpuContext.hpp`, `Window.hpp`, and `ImGuiLayer.hpp` and use the exact accessor and method names they actually declare (`Win()`, `Swap()`, `Imgui()`, `PumpEvents`, `BeginFrame`, `EndFrame`, `Present`). If a name differs, use the real one — do not rename existing API to match this snippet.

- [ ] **Step 3: Build + gate + commit**

```bat
cd Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe ~[gpu]
```

```bash
git add Arcane/Arcane/src/Arcane/Host/BootPresenter.hpp Arcane/Arcane/src/Arcane/Host/BootPresenter.cpp
git commit -m "feat(arcane): BootPresenter -- one loading frame through the canonical present path"
```

---

## Task 7: `BootSplashWindow` — the pre-device splash

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Host/BootSplashWindow.hpp`, `.cpp`

**Interfaces:**
- Produces: `Arcane::BootSplashWindow(const char* imagePath)`, `~BootSplashWindow()`, `void Close()`.

**Why this exists:** `BootPresenter` cannot present until `gpu_core` finishes, so without this the user gets ~1s of *nothing* after clicking — which reads as a failed launch. UE avoids this with a separate OS window on its own thread that needs no device (`WindowsPlatformSplash.cpp:722`, shown before Slate exists at `LaunchEngineLoop.cpp:2913-2917`).

**Constraints:** Windows-only to start; a no-op elsewhere. It must never be able to fail boot. It is **not** a `BootSequence` stage — it exists before the sequence does.

- [ ] **Step 1: Write the header**

```cpp
#pragma once

// A plain OS window shown at process start, BEFORE any graphics device exists,
// so something is on screen within ~100ms of the click. Torn down once the real
// window is up (see the ordering note on Close()).
//
// Windows-only; a no-op elsewhere. It must NEVER be able to fail boot -- every
// failure path degrades to "no splash" silently.

#include <Arcane/Base/Api.hpp>

#include <memory>

namespace Arcane
{
    class ARCANE_API BootSplashWindow
    {
    public:
        // Never throws. A missing or unreadable image yields a live object whose
        // IsOpen() is false.
        explicit BootSplashWindow(const char* imagePath) noexcept;
        ~BootSplashWindow();

        BootSplashWindow(const BootSplashWindow&)            = delete;
        BootSplashWindow& operator=(const BootSplashWindow&) = delete;

        // ORDERING MATTERS: call this AFTER the real window is shown, never
        // before. Closing first leaves a frame with neither window on screen,
        // which is the flicker this whole component exists to avoid. UE gets
        // this right by hiding its splash during the game's first Tick rather
        // than at the end of Init (GameEngine.cpp:1975).
        void Close() noexcept;

        [[nodiscard]] bool IsOpen() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
```

- [ ] **Step 2: Write the implementation**

```cpp
#include <Arcane/Host/BootSplashWindow.hpp>

#include <Arcane/Base/Log.hpp>

#if defined(_WIN32)
#include <Windows.h>
#include <atomic>
#include <string>
#include <thread>
#endif

namespace Arcane
{
#if defined(_WIN32)
    struct BootSplashWindow::Impl
    {
        std::thread       thread;
        std::atomic<HWND> hwnd{nullptr};
        std::atomic<bool> open{false};
        std::string       imagePath;
    };

    namespace
    {
        LRESULT CALLBACK SplashProc(HWND h, UINT msg, WPARAM w, LPARAM l)
        {
            if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
            return DefWindowProcW(h, msg, w, l);
        }
    }

    BootSplashWindow::BootSplashWindow(const char* imagePath) noexcept
        : m_impl(nullptr)
    {
        try
        {
            m_impl = std::make_unique<Impl>();
            m_impl->imagePath = imagePath ? imagePath : "";
            m_impl->thread = std::thread([impl = m_impl.get()]
            {
                WNDCLASSEXW wc{};
                wc.cbSize        = sizeof(wc);
                wc.lpfnWndProc   = SplashProc;
                wc.hInstance     = GetModuleHandleW(nullptr);
                wc.hbrBackground = CreateSolidBrush(RGB(13, 13, 15));
                wc.lpszClassName = L"ArcaneBootSplash";
                RegisterClassExW(&wc);

                constexpr int kW = 480, kH = 270;
                const int x = (GetSystemMetrics(SM_CXSCREEN) - kW) / 2;
                const int y = (GetSystemMetrics(SM_CYSCREEN) - kH) / 2;

                HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                         wc.lpszClassName, L"Arcane",
                                         WS_POPUP, x, y, kW, kH,
                                         nullptr, nullptr, wc.hInstance, nullptr);
                if (!h) return;
                impl->hwnd.store(h);
                impl->open.store(true);
                ShowWindow(h, SW_SHOW);
                UpdateWindow(h);

                MSG msg;
                while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                impl->open.store(false);
            });
        }
        catch (...)
        {
            // Never fail boot for a splash.
            m_impl.reset();
        }
    }

    void BootSplashWindow::Close() noexcept
    {
        if (!m_impl) return;
        if (HWND h = m_impl->hwnd.exchange(nullptr))
            PostMessageW(h, WM_CLOSE, 0, 0);
        if (m_impl->thread.joinable())
            m_impl->thread.join();
        m_impl->open.store(false);
    }

    bool BootSplashWindow::IsOpen() const noexcept
    {
        return m_impl && m_impl->open.load();
    }

    BootSplashWindow::~BootSplashWindow() { Close(); }
#else
    struct BootSplashWindow::Impl {};
    BootSplashWindow::BootSplashWindow(const char*) noexcept : m_impl(nullptr) {}
    BootSplashWindow::~BootSplashWindow() = default;
    void BootSplashWindow::Close() noexcept {}
    bool BootSplashWindow::IsOpen() const noexcept { return false; }
#endif
}
```

The window is a solid themed rectangle in this task. Drawing the logo bitmap is a `WM_PAINT` + `StretchDIBits` addition; do it only after the ordering in Task 8 is desk-verified, so a painting bug cannot be confused with a lifecycle bug.

- [ ] **Step 3: Build + gate + commit**

```bat
cd Arcane
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe ~[gpu]
```

```bash
git add Arcane/Arcane/src/Arcane/Host/BootSplashWindow.hpp Arcane/Arcane/src/Arcane/Host/BootSplashWindow.cpp
git commit -m "feat(arcane): pre-device boot splash window (Windows; no-op elsewhere)"
```

---

## Task 8: Wire both hosts onto `CoreStages`

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp` (`Init`, `Run`), `EditorApp.hpp`, `Arcane/ArcaneEditor/src/main.cpp`
- Modify: `Arcane/ArcaneRuntime/src/RuntimeApp.cpp`, `Arcane/ArcaneRuntime/src/main.cpp`
- Modify: `Arcane/Arcane/src/Arcane/Host/ProjectBoot.cpp` (fill in the stage bodies)

**Interfaces:**
- Consumes: everything from Tasks 3-7.

This is the task where the placeholder stage bodies from Task 5 get their real work, moved out of `EditorApp::Init` and `RuntimeApp`.

- [ ] **Step 1: Move each `EditorApp::Init` block into its stage body**

Work through `EditorApp::Init` top to bottom, moving each block into the matching `CoreStages`/`EditorStages` lambda in `ProjectBoot.cpp`. The mapping:

| `Init` block | Stage |
|---|---|
| `GpuContext::Create` | `gpu_core` |
| `SetTitle`, `SetIcon`, toolbar logo, docking flag, theme, layout settings, console sink | `editor_shell` |
| `InstallEditorFonts` | `editor_fonts` |
| `new Astra::TypeContext` + `Astra::SetTypeContext` + `VerifySharedTypeContext` | `type_context_install` |
| `m_runtime.emplace(...)` | `runtime_create` |
| `SetRenderResources`, `OffscreenImGuiLayer::Create`, `SetImGui` | `render_bridge` |
| `m_runtime->OpenProject(...)` | `project_open` |
| `EditorLock::Write` | `editor_lock` |
| `HostBoot::LoadInputConfig` | `input_config` |
| `PluginHost` emplace + `AddPlugin` + `Load` | `plugin_load` |
| `UpdateWindowTitle`, raise-open-project | `finalize` |

Each body returns `false` only where `Init` returned `false` today. **`project_open` must keep returning `true` on failure while recording `m_projectOpenError`** — it is `Optional` precisely because a failed open is not fatal in the editor today (`EditorApp.cpp:257-264`).

- [ ] **Step 2: Add the `splash_ready` body with the correct ordering**

```cpp
// Show the REAL window first, THEN close the pre-device splash. Reversing these
// leaves a frame with neither on screen.
ctx.gpu->Win().Show();
if (ctx.splash) ctx.splash->Close();
```

Add `BootSplashWindow* splash = nullptr;` to `BootContext` and set it from each host.

- [ ] **Step 3: Rewrite `EditorApp::Run`**

```cpp
    int EditorApp::Run()
    {
        Arcane::HostBoot::BootContext ctx{};
        ctx.runtime     = nullptr;              // stages populate as they go
        ctx.splash      = m_splash.get();
        ctx.projectPath = m_config.projectPath.c_str();

        Arcane::BootSequence seq(Arcane::HostBoot::EditorStages(ctx));
        // Presenter is null until gpu_core creates the device; BootSequence
        // tolerates that and the pre-device splash covers the gap.
        const Arcane::BootResult boot = seq.Run(m_presenter.get());
        if (!boot.ok)
            return boot.quitRequested ? 0 : 1;

        MainLoop();
        Shutdown();
        return 0;
    }
```

**The presenter cannot exist before `gpu_core`, and the resolution is a lazy presenter — use this, do not invent an alternative.** `BootSequence::Run` takes one `IBootPresenter*` for the whole run, so pass a stable wrapper that forwards only once the device exists:

```cpp
// EditorApp.hpp
    // Forwards to a real BootPresenter once gpu_core has built the device.
    // Before that it reports "keep going" -- the pre-device splash is what the
    // user is looking at, so there is nothing to draw here yet.
    class LazyBootPresenter final : public Arcane::IBootPresenter
    {
    public:
        void Bind(Arcane::BootPresenter* p) noexcept { m_inner = p; }
        bool Present(const Arcane::BootProgress& progress) override
        {
            return m_inner ? m_inner->Present(progress) : true;
        }
    private:
        Arcane::BootPresenter* m_inner = nullptr;
    };
```

`EditorApp` owns a `LazyBootPresenter m_lazyPresenter;` and an `std::optional<BootPresenter> m_presenter;`. The `gpu_core` stage body ends with:

```cpp
        m_presenter.emplace(*m_gpu, Arcane::BootPresenterMode::Fullscreen);
        m_lazyPresenter.Bind(&*m_presenter);
```

and `Run` passes `&m_lazyPresenter`. This keeps `BootSequence`'s signature simple and puts the one piece of lifetime awkwardness in the host, where it belongs.

- [ ] **Step 4: Create the pre-device splash first thing in both `main.cpp`**

```cpp
    // Before ANY engine boot: something on screen within ~100ms.
    Arcane::BootSplashWindow splash("data/images/arcane_logo.png");
```

Place it after the `--print-engine-info` probe (which must stay free of any window) and before constructing the app.

- [ ] **Step 5: Give `ArcaneRuntime` the ABI gate it lacks**

`RuntimeApp.cpp:113-115` currently warns on a failed `OpenProject` and falls through to the `data/` + `--plugin` fallback, so an ABI-stale project silently boots the wrong content. In the runtime host's `project_open` handling, refuse instead:

```cpp
        ARC_ERROR("ArcaneRuntime: '{}' could not be opened (engine ABI {} -- is the "
                  "project's game DLL built against this engine?). Refusing to boot "
                  "with the data/ fallback.",
                  ctx.projectPath, static_cast<unsigned>(Arcane::PluginABIVersion()));
        return false;   // Fatal for the runtime host
```

The editor keeps its Optional behaviour (it can usefully run project-less and raise the Open Project dialog); the runtime host cannot.

- [ ] **Step 6: Build, gate, and verify the runtime still renders**

```bat
cd Arcane
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe ~[gpu]
cd ..\ArcaneRuntime
ArcaneRuntime.exe --project ..\..\..\SampleProject --frames 30 --screenshot boot.png
```

Expected: gate green; the runtime exits 0 and `boot.png` shows the scene. That screenshot flag exists precisely so "what did it actually draw" is self-serve rather than desk-only.

- [ ] **Step 7: Commit**

```bash
git add Arcane/ArcaneEditor/src Arcane/ArcaneRuntime/src Arcane/Arcane/src/Arcane/Host/
git commit -m "feat(arcane): both hosts boot from CoreStages; ArcaneRuntime gains its ABI gate"
```

---

## Task 9: Project-switch overlay

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorAppProject.cpp:309+` (`SwitchProject`)

**Interfaces:**
- Consumes: `BootSequence`, `BootPresenter` with `BootPresenterMode::Overlay`.

- [ ] **Step 1: Keep every existing guard exactly where it is**

`SwitchProject`'s early returns — rival editor (`:322`), invalid project (`:339`), ABI mismatch (`:346`), unsaved documents (`:363`) — all run **before** the sequence is built, so a refused switch still costs nothing and shows no overlay. Do not move them.

- [ ] **Step 2: Replace the teardown-and-reopen body with a four-stage sequence**

```cpp
        Arcane::HostBoot::BootContext ctx{};
        ctx.runtime     = &*m_runtime;
        ctx.gpu         = m_gpu.get();
        ctx.projectPath = path.string().c_str();

        auto stage = [](std::string id, std::vector<std::string> deps,
                        Arcane::BootThread thread, std::uint32_t weight,
                        std::function<bool()> run)
        {
            Arcane::BootStage s;
            s.id = std::move(id); s.dependsOn = std::move(deps);
            s.thread = thread; s.policy = Arcane::BootPolicy::Fatal;
            s.weight = weight; s.run = std::move(run);
            return s;
        };

        // NOT CoreStages: the process is already booted. This is the reopen
        // subset only, and every stage is Fatal because the caller already
        // validated the project and torn down the old one -- there is nothing
        // to fall back to.
        std::vector<Arcane::BootStage> stages;
        stages.push_back(stage("switch_teardown", {}, Arcane::BootThread::Main, 2, [&]
        {
            // Move SwitchProject's existing teardown here verbatim: CloseAll,
            // sprite/post/material cache Clear, ClearSceneReferences,
            // m_scene.Reset, EditorLock::Clear(outgoingRoot).
            return true;
        }));
        stages.push_back(stage("switch_project_open", {"switch_teardown"}, Arcane::BootThread::Worker, 6, [&]
        {
            return m_runtime->OpenProject(path);
        }));
        stages.push_back(stage("switch_render_bridge", {"switch_project_open"}, Arcane::BootThread::Main, 1, [&]
        {
            // The existing post-open re-bridge: SetRenderResources / SetImGui
            // re-publish against the new project's runtime state.
            return true;
        }));
        stages.push_back(stage("switch_plugin_load", {"switch_render_bridge"}, Arcane::BootThread::Main, 4, [&]
        {
            // Move the existing PluginHost emplace + AddPlugin + Load here.
            return true;
        }));

        Arcane::BootPresenter overlay(*m_gpu, Arcane::BootPresenterMode::Overlay);
        Arcane::BootSequence  seq(std::move(stages));
        const Arcane::BootResult r = seq.Run(&overlay);
        if (!r.ok)
            m_projectOpenError = "Switching to '" + path.generic_string() +
                                 "' failed at stage '" + r.failedStage + "'.";
```

Fill each stage's body from the code currently inline in `SwitchProject` — move it, do not rewrite it. `project_open` is `Fatal` here (unlike boot) because the caller already validated the project and torn down the old one; there is nothing to fall back to.

- [ ] **Step 3: Build, gate, commit**

```bat
cd Arcane
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe ~[gpu]
```

```bash
git add Arcane/ArcaneEditor/src/EditorAppProject.cpp
git commit -m "feat(editor): project switch runs through BootSequence with an overlay presenter"
```

---

## Task 10: Scan progress and the `.arcproj` splash block

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Project/AssetRegistry.hpp`, `.cpp` (`ScanContent`)
- Modify: `Arcane/Arcane/src/Arcane/Project/ProjectManifest.hpp`, `.cpp` (splash block)
- Test: extend `Arcane/Tests/src/AssetRegistryTest.cpp` and `ProjectManifestTest.cpp`

**Interfaces:**
- Produces: `ScanContent(dir, scheme, ScanProgressFn onProgress = {})`, `ProjectManifest::splash` (`SplashConfig{enabled, image, backgroundColor[3], showProgress, minDurationSeconds}`).

Spec §3 requires the bar's "Scanning content... 412 / 1180" detail line; spec §6 requires the manifest splash block with `showProgress` defaulting **false** for the runtime host.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("ScanContent reports monotonic progress ending at the total", "[project]")
{
    // Build a temp content dir with 3 material assets, then scan with a callback.
    // (Use the same tmp-dir + SaveMaterialAsset helpers the other cases here use.)
    std::vector<std::pair<std::size_t, std::size_t>> seen;
    Arcane::AssetRegistry reg;
    reg.ScanContent(content, "game",
                    [&](std::size_t done, std::size_t total) { seen.emplace_back(done, total); });

    REQUIRE_FALSE(seen.empty());
    for (std::size_t i = 1; i < seen.size(); ++i)
        CHECK(seen[i].first >= seen[i - 1].first);      // monotonic
    CHECK(seen.back().first == seen.back().second);      // terminates at total
}

TEST_CASE("a manifest with no splash block defaults showProgress to false", "[project]")
{
    // Absent block: engine branding, no progress -- a player does not care that
    // we are scanning asset 412 of 1180. UE reaches the same conclusion.
    const auto m = Arcane::ProjectManifest::Parse(R"({
        "formatVersion": 1, "name": "T", "engine": { "abi": 9 }
    })");
    REQUIRE(m.has_value());
    CHECK(m->splash.enabled);
    CHECK_FALSE(m->splash.showProgress);
}

TEST_CASE("a manifest splash block round-trips its fields", "[project]")
{
    const auto m = Arcane::ProjectManifest::Parse(R"({
        "formatVersion": 1, "name": "T", "engine": { "abi": 9 },
        "splash": { "enabled": false, "image": "game://B/s.png", "showProgress": true,
                    "minDurationSeconds": 1.5 }
    })");
    REQUIRE(m.has_value());
    CHECK_FALSE(m->splash.enabled);
    CHECK(m->splash.image == "game://B/s.png");
    CHECK(m->splash.showProgress);
    CHECK(m->splash.minDurationSeconds == 1.5f);
}
```

Match `ProjectManifest`'s real parse entry point — read `ProjectManifest.cpp:12-60` and use whatever it actually exposes rather than assuming `Parse`.

- [ ] **Step 2: Add the progress callback**

In `AssetRegistry.hpp`, give `ScanContent` a **defaulted** trailing parameter so every existing call site is untouched:

```cpp
        using ScanProgressFn = Arcane::FunctionRef<void(std::size_t done, std::size_t total)>;

        std::size_t ScanContent(const std::filesystem::path& contentDir,
                                std::string_view scheme,
                                ScanProgressFn onProgress = {});
```

Getting a denominator needs a cheap `directory_iterator` count pass before the real scan. That count is stat-only (no file reads) and is the one added cost in this design; do it once, up front.

- [ ] **Step 3: Add the splash block**

Add to `ProjectManifest.hpp`:

```cpp
    struct SplashConfig
    {
        bool        enabled            = true;
        std::string image;                        // empty -> engine branding
        float       backgroundColor[3] = {0.05f, 0.05f, 0.06f};
        // FALSE by default, deliberately. The editor always shows progress; a
        // player does not. UE enforces the same split structurally --
        // FFeedbackContext::ProgressReported is a no-op outside the editor.
        bool        showProgress       = false;
        float       minDurationSeconds = 0.0f;    // avoids an ~80ms splash flash
    };
```

Parse it leniently, exactly like `plugins` (`ProjectManifest.cpp:47-58`): a missing or wrong-typed block yields defaults rather than failing the manifest.

- [ ] **Step 4: Wire both into their consumers**

`project_open`'s stage body passes a callback that writes `"Scanning content... N / M"` into the `BootProgress::detail` the presenter renders. The runtime host reads `manifest.splash` and calls `SetShowProgress(cfg.showProgress)`; the editor always passes `true`.

- [ ] **Step 5: Build, gate, commit**

```bat
cd Arcane
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "[project]"
ArcaneTests.exe ~[gpu]
```

```bash
git add Arcane/Arcane/src/Arcane/Project/ Arcane/Tests/src/AssetRegistryTest.cpp Arcane/Tests/src/ProjectManifestTest.cpp
git commit -m "feat(arcane): ScanContent progress callback + .arcproj splash block"
```

---

## Final verification

- [ ] **Both seeds**

```bat
cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe ~[gpu]
ArcaneTests.exe ~[gpu] --rng-seed 1
```

Record both counts; attribute every delta from the 31082/639 baseline.

- [ ] **The gate bites** — re-confirm Task 5 Step 6 (delete a core stage, parity test fails, restore).
- [ ] **ABI unchanged** — `git diff main -- Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp` shows no version change.
- [ ] **ShaderCompiler untouched behaviourally** — `[shadercompile]` matches its pre-Task-2 counts with zero test edits.
- [ ] **Editor relinked** — `ArcaneEditor.exe` timestamp is newer than the last edit to `EditorApp*.cpp`. A green gate proves nothing about the editor; it is not compiled into `ArcaneTests`.
- [ ] **Desk-verify** the ten items in spec §9, especially 1b (real window appears *before* the pre-device splash closes) and 1c (splash path disabled, boot still completes).

---

## Notes for the implementer

- **Read before you rename.** Tasks 6, 8, and 9 reference accessor names (`Win()`, `Swap()`, `Imgui()`, `PumpEvents`, `m_documents`, `m_projectOpenError`) read from the codebase but which may have drifted. Use the name that is actually there; add a thin wrapper rather than renaming existing API.
- **Task 2 has a real escape hatch and you are expected to use it if needed.** If `ShaderCompiler` fights the retrofit, revert that task and continue. Do not edit a `[shadercompile]` test to make it pass — that is the signal the abstraction is wrong, not that the test is.
- **`project_open` stays `Optional` in the editor and becomes `Fatal` in the runtime.** That asymmetry is deliberate and preserves today's behaviour on both sides; do not "fix" it into symmetry.
- **The pre-device splash must never fail boot.** Every error path degrades to "no splash", silently.
- **Do not add a Worker stage without a disjoint-ownership proof.** Only `project_open` and `editor_lock` are Worker today, they never overlap each other, and the DAG exists for exactly the `project_open`/`gpu_core` overlap.
