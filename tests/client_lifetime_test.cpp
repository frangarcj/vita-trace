#include <doctest/doctest.h>
#include <psp2/sysmodule.h>
#include "vita_tracy/client.h"

namespace {
struct Fake {
    bool resident = false;
    int loads = 0, unloads = 0, adopts = 0, starts = 0, stops = 0;
    int load_error = 0, unload_error = 0, detach_error = 0;
} fake;
struct Fixture {
    Fixture() { fake = {}; }
    ~Fixture() {
        fake.detach_error = fake.unload_error = 0;
        CHECK(vita_tracy_shutdown_checked() == 0);
    }
};
}
namespace tracy {
void StartupProfiler() { ++fake.starts; }
void ShutdownProfiler() { ++fake.stops; }
}
extern "C" {
int sceSysmoduleIsLoaded(int) { return fake.resident ? SCE_SYSMODULE_LOADED : -1; }
int sceSysmoduleLoadModule(int) {
    ++fake.loads;
    if (!fake.load_error) fake.resident = true;
    return fake.load_error;
}
int sceSysmoduleUnloadModule(int) {
    ++fake.unloads;
    if (!fake.unload_error) fake.resident = false;
    return fake.unload_error;
}
int vita_tracy_timebase_adopt_perf(int available) { ++fake.adopts; return available; }
int vita_tracy_kernel_detach_checked(void) { return fake.detach_error; }
}

TEST_CASE_FIXTURE(Fixture, "repeated client init neither restarts Tracy nor changes its timebase") {
    REQUIRE(vita_tracy_init() == 0);
    REQUIRE(vita_tracy_init() == 0);
    CHECK(fake.starts == 1); CHECK(fake.loads == 1); CHECK(fake.adopts == 1);
    REQUIRE(vita_tracy_shutdown_checked() == 0);
    CHECK(fake.stops == 1); CHECK(fake.unloads == 1);
}
TEST_CASE_FIXTURE(Fixture, "optional ScePerf failure does not prevent profiling") {
    fake.load_error = -20;
    REQUIRE(vita_tracy_init() == 0);
    CHECK(vita_tracy_perf_module_status() == -20);
    CHECK(fake.starts == 1);
    REQUIRE(vita_tracy_shutdown_checked() == 0);
    CHECK(fake.unloads == 0);
}
TEST_CASE_FIXTURE(Fixture, "failed detach keeps Tracy and its clock alive until a successful retry") {
    REQUIRE(vita_tracy_init() == 0);
    fake.detach_error = -30;
    CHECK(vita_tracy_shutdown_checked() == -30);
    CHECK(fake.stops == 0); CHECK(fake.unloads == 0);
    REQUIRE(vita_tracy_init() == 0);
    CHECK(fake.starts == 1); CHECK(fake.adopts == 1);
    fake.detach_error = 0;
    REQUIRE(vita_tracy_shutdown_checked() == 0);
    CHECK(fake.stops == 1); CHECK(fake.unloads == 1);
}
TEST_CASE_FIXTURE(Fixture, "failed ScePerf unload retains ownership for retry without stopping Tracy twice") {
    REQUIRE(vita_tracy_init() == 0);
    fake.unload_error = -40;
    CHECK(vita_tracy_shutdown_checked() == -40);
    fake.unload_error = 0;
    REQUIRE(vita_tracy_shutdown_checked() == 0);
    CHECK(fake.stops == 1); CHECK(fake.unloads == 2);
}
TEST_CASE_FIXTURE(Fixture, "client does not unload a ScePerf instance owned by the application") {
    fake.resident = true;
    REQUIRE(vita_tracy_init() == 0);
    CHECK(vita_tracy_perf_module_status() == 0);
    REQUIRE(vita_tracy_shutdown_checked() == 0);
    CHECK(fake.loads == 0); CHECK(fake.unloads == 0);
}

TEST_CASE_FIXTURE(Fixture, "client can restart a completed lifetime with a fresh clock") {
    REQUIRE(vita_tracy_init() == 0);
    REQUIRE(vita_tracy_shutdown_checked() == 0);
    REQUIRE(vita_tracy_init() == 0);
    CHECK(fake.starts == 2); CHECK(fake.adopts == 2); CHECK(fake.loads == 2);
}
