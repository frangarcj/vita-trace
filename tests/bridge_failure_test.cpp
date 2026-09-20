#include <doctest/doctest.h>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <client/TracyProfiler.hpp>
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
    int resolve_calls = 0;
    VitaTracyStats *stats_pointer = nullptr;
    std::map<uint32_t, int> resolutions;
    std::vector<uint32_t> sample_threads;
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
void test_tracy_commit(const tracy::TestQueueItem &item) {
    if (!item.callstackSampleFat.ptr) return;
    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime->sample_threads.push_back(item.callstackSampleFat.thread);
    runtime->cv.notify_all();
}

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
int vitaTracyResolveThread(uint32_t guid) {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    ++runtime->resolve_calls;
    auto it = runtime->resolutions.find(guid);
    return it == runtime->resolutions.end() ? VITA_TRACY_ERROR_TARGET : it->second;
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

TEST_CASE_FIXTURE(Fixture, "bridge preserves sparse segment labels and bounds unterminated module names") {
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    VitaTraceControlRecord record{};
    record.type = VITA_TRACE_MODULE_SNAPSHOT;
    auto &module = record.payload.module_snapshot;
    std::memset(module.module_name, 'A', sizeof(module.module_name));
    module.segment_count = 3;
    module.segments[0] = {0x81230000u, 0x1000u, 5u};
    module.segments[2] = {0x90000000u, 0x2000u, 6u};
    REQUIRE(vita_trace_ring_try_push(vita_trace_shared_control_ring(runtime->memory), &record));
    REQUIRE(vitaTracyWakeup() == 0);
    REQUIRE(vita_tracy_kernel_detach_checked() == 0);
    REQUIRE(runtime->messages.size() == 2);
    const auto prefix = "vita-tracy module " + std::string(sizeof(module.module_name), 'A') + " nid=";
    CHECK(runtime->messages[0].find(prefix) == 0);
    CHECK(runtime->messages[0].find("seg=0 ") != std::string::npos);
    CHECK(runtime->messages[1].find("seg=2 ") != std::string::npos);
    CHECK(runtime->messages[1].find("vaddr=0x90000000") != std::string::npos);
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

TEST_CASE_FIXTURE(Fixture, "bridge preserves asynchronous PC sampler failure messages") {
    runtime->start_events = VITA_TRACY_WAKE_SAMPLE_IRQ(2);
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    std::unique_lock<std::mutex> lock(runtime->mutex);
    REQUIRE(runtime->cv.wait_for(lock, std::chrono::seconds(5),
        [&] { return !runtime->messages.empty(); }));
    CHECK(runtime->messages[0].find("PC sampler c2 stopped") != std::string::npos);
    CHECK(runtime->stats_calls == 0);
}

TEST_CASE_FIXTURE(Fixture, "bridge resolves every IRQ global thread ID outside the sample producer") {
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    runtime->resolutions[0x700] = 0x55;

    auto push = [&](uint64_t timestamp) {
        VitaTraceSample sample{};
        sample.timestamp = timestamp;
        sample.pid = 123;
        sample.tid = 0x700;
        sample.pc = 0x81001234;
        sample.cpu = 0;
        sample.flags = VITA_TRACE_SAMPLE_PMU_IRQ | VITA_TRACE_SAMPLE_GLOBAL_TID;
        REQUIRE(vita_trace_ring_try_push(vita_trace_shared_core_ring(runtime->memory, 0), &sample));
        REQUIRE(vitaTracyWakeup() == 0);
    };

    push(10000);
    {
        std::unique_lock<std::mutex> lock(runtime->mutex);
        REQUIRE(runtime->cv.wait_for(lock, std::chrono::seconds(5),
            [&] { return runtime->sample_threads.size() == 1; }));
        CHECK(runtime->sample_threads[0] == 0x55);
        CHECK(runtime->resolve_calls == 1);
    }

    push(500000);
    {
        std::unique_lock<std::mutex> lock(runtime->mutex);
        REQUIRE(runtime->cv.wait_for(lock, std::chrono::seconds(5),
            [&] { return runtime->sample_threads.size() == 2; }));
        CHECK(runtime->sample_threads[1] == 0x55);
        CHECK(runtime->resolve_calls == 2);
    }

    runtime->resolutions[0x700] = 0x56;
    push(1200000);
    {
        std::unique_lock<std::mutex> lock(runtime->mutex);
        REQUIRE(runtime->cv.wait_for(lock, std::chrono::seconds(5),
            [&] { return runtime->sample_threads.size() == 3; }));
        CHECK(runtime->sample_threads[2] == 0x56);
        CHECK(runtime->resolve_calls == 3);
    }
}

TEST_CASE_FIXTURE(Fixture, "bridge drops an unresolved IRQ thread instead of inventing attribution") {
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    VitaTraceSample sample{};
    sample.timestamp = 10000;
    sample.pid = 123;
    sample.tid = 0xBAD;
    sample.pc = 0x81001234;
    sample.cpu = 0;
    sample.flags = VITA_TRACE_SAMPLE_PMU_IRQ | VITA_TRACE_SAMPLE_GLOBAL_TID;
    REQUIRE(vita_trace_ring_try_push(vita_trace_shared_core_ring(runtime->memory, 0), &sample));
    REQUIRE(vitaTracyWakeup() == 0);

    // A control command fences behind the drain worker, proving it consumed the sample.
    VitaTracyStats stats{};
    stats.size = sizeof(stats);
    stats.abi_version = VITA_TRACY_ABI_VERSION;
    REQUIRE(vita_tracy_kernel_get_stats(&stats) == 0);
    CHECK(runtime->resolve_calls == 1);
    CHECK(runtime->sample_threads.empty());
}

TEST_CASE_FIXTURE(Fixture, "bridge excludes only resolved profiler workers from IRQ samples") {
    REQUIRE(vita_tracy_kernel_attach(8, 8) == 0);
    runtime->resolutions[0x701] = 0x66;
    auto *header = static_cast<VitaTraceSharedHeader *>(runtime->memory);
    REQUIRE(vita_trace_thread_add(&header->profiler_threads, 0x66));

    VitaTraceSample sample{};
    sample.timestamp = 10000;
    sample.pid = 123;
    sample.tid = 0x701;
    sample.pc = 0x81001234;
    sample.cpu = 0;
    sample.flags = VITA_TRACE_SAMPLE_PMU_IRQ | VITA_TRACE_SAMPLE_GLOBAL_TID;
    REQUIRE(vita_trace_ring_try_push(vita_trace_shared_core_ring(runtime->memory, 0), &sample));
    REQUIRE(vitaTracyWakeup() == 0);

    VitaTracyStats stats{};
    stats.size = sizeof(stats);
    stats.abi_version = VITA_TRACY_ABI_VERSION;
    REQUIRE(vita_tracy_kernel_get_stats(&stats) == 0);
    CHECK(runtime->resolve_calls == 1);
    CHECK(runtime->sample_threads.empty());
}
