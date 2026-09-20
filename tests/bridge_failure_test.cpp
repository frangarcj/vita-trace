#include <doctest/doctest.h>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <psp2/kernel/threadmgr.h>
#include "vita_tracy/client.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/kernel_events.h"
#include "vita_tracy/shared_layout.h"
#include "vita_tracy/shared_ring.h"

namespace {
struct Runtime {
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    SceKernelThreadEntry entry = nullptr;
    void *memory = nullptr;
    bool registered = false, started = false;
    uint32_t events = 0, start_events = 0;
    std::vector<std::string> messages;
    bool fail_wait = false, fail_join = false, fail_delete = false, fail_sema = false, fail_free = false;
    bool fail_start = false, block_stats = false, stats_entered = false, stats_completed = false;
    int tokens = 0, joins = 0, deletes = 0, frees = 0, stats_calls = 0;
    VitaTracyStats *stats_pointer = nullptr;
};
std::unique_ptr<Runtime> runtime;
struct Fixture {
    Fixture() { runtime = std::make_unique<Runtime>(); }
    ~Fixture() {
        {
            std::lock_guard<std::mutex> lock(runtime->mutex);
            runtime->fail_wait = runtime->fail_join = runtime->fail_delete = runtime->fail_sema = false;
            runtime->fail_free = runtime->fail_start = runtime->block_stats = false;
            runtime->cv.notify_all();
        }
        CHECK(vita_tracy_kernel_detach_checked() == 0);
        CHECK(runtime->memory == nullptr);
        CHECK_FALSE(runtime->worker.joinable());
        runtime.reset();
    }
};
bool consume(bool &flag) { bool ret = flag; flag = false; return ret; }
}
void test_tracy_app_info(const char *text, std::size_t size) {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime->messages.emplace_back(text, size);
    runtime->cv.notify_all();
}
void test_tracy_frame_mark(const char *) {}

extern "C" {
int64_t tracy_vita_get_time(void) { return 1000000000; }
uint32_t vita_tracy_timebase_frequency(void) { return 1000000; }
void tracy_vita_profiler_thread_enter(void) {}
void tracy_vita_profiler_thread_exit(void) {}
void tracy_vita_profiler_threads_bind(void *) {}
SceUID sceKernelGetProcessId(void) { return 123; }
SceUID sceKernelAllocMemBlock(const char *, int, SceSize size, const void *) {
    runtime->memory = std::malloc(size);
    return runtime->memory ? 10 : -10;
}
int sceKernelGetMemBlockBase(SceUID, void **base) { *base = runtime->memory; return 0; }
int sceKernelFreeMemBlock(SceUID) {
    if (consume(runtime->fail_free)) return -44;
    CHECK_FALSE(runtime->registered); CHECK_FALSE(runtime->worker.joinable());
    std::free(runtime->memory); runtime->memory = nullptr; ++runtime->frees; return 0;
}
SceUID sceKernelCreateThread(const char *, SceKernelThreadEntry entry, int, SceSize, unsigned, int, const void *) {
    runtime->entry = entry; return 20;
}
int sceKernelStartThread(SceUID, SceSize size, void *args) {
    if (consume(runtime->fail_start)) return -45;
    runtime->started = true;
    runtime->events |= runtime->start_events;
    runtime->worker = std::thread([=] { runtime->entry(size, args); });
    return 0;
}
int sceKernelWaitThreadEnd(SceUID, int *, unsigned *) {
    ++runtime->joins; CHECK(runtime->started);
    if (consume(runtime->fail_join)) return -41;
    if (runtime->worker.joinable()) runtime->worker.join();
    return 0;
}
int sceKernelDeleteThread(SceUID) {
    if (consume(runtime->fail_delete)) return -42;
    CHECK_FALSE(runtime->worker.joinable()); ++runtime->deletes; return 0;
}
SceUID sceKernelCreateSema(const char *, unsigned, int initial, int, const void *) {
    runtime->tokens = initial; return 30;
}
int sceKernelDeleteSema(SceUID) {
    if (consume(runtime->fail_sema)) return -43;
    return 0;
}
int sceKernelSignalSema(SceUID, int count) {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime->tokens += count; runtime->cv.notify_all(); return 0;
}
int sceKernelWaitSema(SceUID, int count, unsigned *) {
    std::unique_lock<std::mutex> lock(runtime->mutex);
    if (consume(runtime->fail_wait)) return -40;
    runtime->cv.wait(lock, [&] { return runtime->tokens >= count; });
    runtime->tokens -= count; return 0;
}
int vitaTracyRegister(const VitaTracyRegisterArgs *) {
    runtime->registered = true;
    vita_trace_shared_acknowledge(runtime->memory);
    VitaTraceControlRecord sync{};
    sync.type = VITA_TRACE_CLOCK_SYNC; sync.payload.clock_sync.kernel_tick = 1000;
    CHECK(vita_trace_ring_try_push(vita_trace_shared_control_ring(runtime->memory), &sync));
    return 0;
}
int vitaTracyUnregister(uint32_t) {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (!runtime->registered) return VITA_TRACY_ERROR_TARGET;
    runtime->registered = false; return 0;
}
int vitaTracyWakeup(void) {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime->events |= VITA_TRACY_WAKE_DATA; runtime->cv.notify_all(); return 0;
}
int vitaTracyWaitForData(uint32_t) {
    std::unique_lock<std::mutex> lock(runtime->mutex);
    runtime->cv.wait(lock, [&] { return runtime->events != 0; });
    const auto events = runtime->events;
    runtime->events = 0; return (int)events;
}
int vitaTracySetSampling(const VitaTracySamplingConfig *) { return 0; }
int vitaTracySetPmu(const VitaTracyPmuConfig *) { return 0; }
int vitaTracyPmuSampleStart(void) { return 0; }
int vitaTracyPmuSampleStop(void) { return 0; }
int vitaTracyGetStats(VitaTracyStats *stats) {
    std::unique_lock<std::mutex> lock(runtime->mutex);
    runtime->stats_pointer = stats; runtime->stats_entered = true;
    runtime->cv.notify_all();
    runtime->cv.wait(lock, [&] { return !runtime->block_stats; });
    stats->uptime_ms = ++runtime->stats_calls;
    runtime->stats_completed = true; runtime->cv.notify_all(); return 0;
}
}

TEST_CASE_FIXTURE(Fixture, "bridge retains private stats payload after a failed wait and serializes the next request") {
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    VitaTracyStats stats{}; stats.size = sizeof(stats); stats.abi_version = VITA_TRACY_ABI_VERSION;
    {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        runtime->fail_wait = true; runtime->block_stats = true;
    }
    CHECK(vita_tracy_kernel_get_stats(&stats) == -40);
    stats.uptime_ms = 999;
    {
        std::unique_lock<std::mutex> lock(runtime->mutex);
        REQUIRE(runtime->cv.wait_for(lock, std::chrono::seconds(5), [&] { return runtime->stats_entered; }));
        CHECK(runtime->stats_pointer != &stats);
        runtime->block_stats = false; runtime->cv.notify_all();
        REQUIRE(runtime->cv.wait_for(lock, std::chrono::seconds(5), [&] { return runtime->stats_completed; }));
    }
    CHECK(stats.uptime_ms == 999);
    REQUIRE(vita_tracy_kernel_get_stats(&stats) == 0);
    CHECK(stats.uptime_ms == 2);
}
TEST_CASE_FIXTURE(Fixture, "bridge failed join cannot release memory still reachable by its worker") {
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    runtime->fail_join = true;
    CHECK(vita_tracy_kernel_detach_checked() == -41);
    CHECK(runtime->frees == 0); CHECK(runtime->memory != nullptr);
    REQUIRE(vita_tracy_kernel_detach_checked() == 0);
    CHECK(runtime->frees == 1); CHECK(runtime->joins == 2);
}
TEST_CASE_FIXTURE(Fixture, "bridge retains a thread handle if deletion fails after a successful join") {
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    runtime->fail_delete = true;
    CHECK(vita_tracy_kernel_detach_checked() == -42); CHECK(runtime->frees == 0);
    REQUIRE(vita_tracy_kernel_detach_checked() == 0); CHECK(runtime->deletes == 1);
}
TEST_CASE_FIXTURE(Fixture, "bridge retains a semaphore on failed deletion and can retry") {
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    runtime->fail_sema = true;
    CHECK(vita_tracy_kernel_detach_checked() == -43); CHECK(runtime->frees == 0);
    REQUIRE(vita_tracy_kernel_detach_checked() == 0); CHECK(runtime->frees == 1);
}
TEST_CASE_FIXTURE(Fixture, "bridge retains a memblock after a failed free rather than losing its ID") {
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    runtime->fail_free = true;
    CHECK(vita_tracy_kernel_detach_checked() == -44); CHECK(runtime->memory != nullptr);
    REQUIRE(vita_tracy_kernel_detach_checked() == 0); CHECK(runtime->frees == 1);
}
TEST_CASE_FIXTURE(Fixture, "bridge attach rollback never joins a thread that failed to start") {
    runtime->fail_start = true;
    CHECK(vita_tracy_kernel_attach(8, 8) == -45);
    CHECK(runtime->joins == 0); CHECK(runtime->deletes == 1); CHECK(runtime->frees == 1);
}
TEST_CASE_FIXTURE(Fixture, "bridge preserves a failed startup handle when rollback also fails") {
    runtime->fail_start = runtime->fail_delete = true;
    CHECK(vita_tracy_kernel_attach(8, 8) == -42);
    CHECK(runtime->joins == 0); CHECK(runtime->frees == 0);
    REQUIRE(vita_tracy_kernel_detach_checked() == 0);
    CHECK(runtime->joins == 0); CHECK(runtime->deletes == 1); CHECK(runtime->frees == 1);
}

TEST_CASE_FIXTURE(Fixture, "bridge reports retained PMU failures before the first wait without polling stats") {
    runtime->start_events = VITA_TRACY_WAKE_PMU_CPU(1) | VITA_TRACY_WAKE_PMU_COUNTER(2);
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    std::unique_lock<std::mutex> lock(runtime->mutex);
    REQUIRE(runtime->cv.wait_for(lock, std::chrono::seconds(5), [&] { return runtime->messages.size() == 2; }));
    CHECK(runtime->messages[0].find("c1 reader stopped: IRQ routed to wrong CPU") != std::string::npos);
    CHECK(runtime->messages[1].find("c2 reader stopped: counter access or ownership failure") != std::string::npos);
    CHECK(runtime->stats_calls == 0);
}

TEST_CASE_FIXTURE(Fixture, "bridge remains usable after receiving asynchronous PMU failure bits") {
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    {
        std::unique_lock<std::mutex> lock(runtime->mutex);
        runtime->events |= VITA_TRACY_WAKE_DATA | VITA_TRACY_WAKE_PMU_CPU(0);
        runtime->cv.notify_all();
        REQUIRE(runtime->cv.wait_for(lock, std::chrono::seconds(5), [&] { return !runtime->messages.empty(); }));
    }
    VitaTracyStats stats{};
    stats.size = sizeof(stats); stats.abi_version = VITA_TRACY_ABI_VERSION;
    REQUIRE(vita_tracy_kernel_get_stats(&stats) == 0);
    CHECK(stats.uptime_ms == 1);
    REQUIRE(vita_tracy_kernel_detach_checked() == 0);
}

TEST_CASE_FIXTURE(Fixture, "bridge data-only wakes do not manufacture PMU failure messages") {
    runtime->start_events = VITA_TRACY_WAKE_DATA;
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    REQUIRE(vita_tracy_kernel_detach_checked() == 0);
    CHECK(runtime->messages.empty());
    CHECK(runtime->stats_calls == 0);
}
