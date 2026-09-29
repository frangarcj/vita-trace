#pragma once
#include <cstdint>
#include <cstdlib>
namespace tracy {
void StartupProfiler();
void ShutdownProfiler();
enum class QueueType { PlotDataDouble, CallstackSample, ContextSwitch };
struct TestQueueItem {
    struct { uint64_t name; int64_t time; double val; } plotDataDouble;
    struct { int64_t time; uint32_t thread; uint64_t ptr; } callstackSampleFat;
    struct {
        int64_t time; uint32_t oldThread; uint32_t newThread; uint8_t cpu; uint8_t oldThreadWaitReason;
        uint8_t oldThreadState; uint8_t previousCState; int8_t newThreadPriority; int8_t oldThreadPriority;
    } contextSwitch;
};
template<class D, class S> void MemWrite(D *dst, S src) { *dst = static_cast<D>(src); }
inline void *tracy_malloc(std::size_t bytes) { return std::malloc(bytes); }
}
void test_tracy_commit(const tracy::TestQueueItem &item);
#define TracyLfqPrepare(type) tracy::TestQueueItem queued{}; auto *item = &queued
#define TracyLfqCommit do { test_tracy_commit(queued); if (queued.callstackSampleFat.ptr) std::free(reinterpret_cast<void *>(queued.callstackSampleFat.ptr)); } while (0)
