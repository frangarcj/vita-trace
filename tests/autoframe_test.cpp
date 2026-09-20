#include <doctest/doctest.h>
#include <psp2/display.h>
#include <string>

extern "C" void vita_tracy_auto_activate(int);
extern "C" void vita_tracy_auto_report(const char *, int);
extern "C" int __wrap_sceDisplaySetFrameBuf(const SceDisplayFrameBuf *, SceDisplaySetBufSync);
namespace {
int result = 0, frames = 0, calls = 0;
const SceDisplayFrameBuf *last_frame;
SceDisplaySetBufSync last_sync;
std::string info;
struct Fixture {
    SceDisplayFrameBuf frame{};
    Fixture() { vita_tracy_auto_activate(0); result = frames = calls = 0; info.clear(); frame.base = this; }
    ~Fixture() { vita_tracy_auto_activate(0); }
};
}
void test_tracy_app_info(const char *s, std::size_t n) { info.assign(s, n); }
void test_tracy_frame_mark(const char *s) { CHECK(std::string(s) == "Vita display submit"); ++frames; }
extern "C" int __real_sceDisplaySetFrameBuf(const SceDisplayFrameBuf *frame, SceDisplaySetBufSync sync) {
    ++calls; last_frame = frame; last_sync = sync; return result;
}
TEST_CASE_FIXTURE(Fixture, "automatic frame wrapping forwards the real call and only emits while active") {
    CHECK(__wrap_sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME) == 0);
    CHECK(frames == 0);
    vita_tracy_auto_activate(1);
    CHECK(__wrap_sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_IMMEDIATE) == 0);
    CHECK(calls == 2); CHECK(last_frame == &frame); CHECK(last_sync == SCE_DISPLAY_SETBUF_IMMEDIATE);
    CHECK(frames == 1);
    vita_tracy_auto_activate(0);
    __wrap_sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    CHECK(frames == 1);
}
TEST_CASE_FIXTURE(Fixture, "failed display calls and blackout do not fabricate frames") {
    vita_tracy_auto_activate(1);
    result = -99;
    CHECK(__wrap_sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME) == -99);
    result = 0;
    __wrap_sceDisplaySetFrameBuf(nullptr, SCE_DISPLAY_SETBUF_NEXTFRAME);
    frame.base = nullptr;
    __wrap_sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    CHECK(frames == 0); CHECK(calls == 3);
}
TEST_CASE_FIXTURE(Fixture, "bootstrap diagnostics retain stage and failure code") {
    vita_tracy_auto_report("PMU start", -7);
    CHECK(info == "vita-tracy automatic PMU start: -7");
}
