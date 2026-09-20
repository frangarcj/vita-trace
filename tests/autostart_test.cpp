#include <doctest/doctest.h>
#include <string>
#include <vector>
#include <utility>
#include "vita_tracy/client.h"

extern "C" int __wrap_main(int, char **);
extern "C" int __wrap_sceKernelExitProcess(int);
extern "C" void vita_tracy_auto_stop(void);

namespace {
struct Fake {
    int init_result = 0, attach_result = 0, configure_result = 0, start_result = 0, stop_result = 0;
    int init_calls = 0, attach_calls = 0, configure_calls = 0, start_calls = 0, stop_calls = 0;
    int main_calls = 0, argc = 0, main_result = 47, exit_status = 0, exit_result = -12;
    char **argv = nullptr;
    bool active = false;
    VitaTracyPmuConfig config{};
    std::vector<std::pair<std::string,int>> reports;
} fake;
struct Fixture {
    Fixture() { fake = {}; }
    ~Fixture() { fake.stop_result = 0; vita_tracy_auto_stop(); }
};
}
extern "C" {
int vita_tracy_init(void) { ++fake.init_calls; return fake.init_result; }
int vita_tracy_shutdown_checked(void) { ++fake.stop_calls; CHECK_FALSE(fake.active); return fake.stop_result; }
int vita_tracy_kernel_attach(uint32_t, uint32_t) { ++fake.attach_calls; return fake.attach_result; }
int vita_tracy_kernel_configure_pmu(const VitaTracyPmuConfig *cfg) {
    ++fake.configure_calls; fake.config = *cfg; return fake.configure_result;
}
int vita_tracy_kernel_pmu_sample_start(void) { ++fake.start_calls; return fake.start_result; }
void vita_tracy_auto_activate(int enabled) { fake.active = enabled != 0; }
void vita_tracy_auto_report(const char *stage, int result) { fake.reports.emplace_back(stage,result); }
int __real_main(int argc, char **argv) {
    ++fake.main_calls; fake.argc = argc; fake.argv = argv; return fake.main_result;
}
int __real_sceKernelExitProcess(int status) { fake.exit_status = status; return fake.exit_result; }
}

TEST_CASE_FIXTURE(Fixture, "automatic bootstrap forwards main arguments and result without editing the HB") {
    char name[] = "homebrew"; char *args[] = {name, nullptr};
    CHECK(__wrap_main(1, args) == 47);
    CHECK(fake.main_calls == 1); CHECK(fake.argc == 1); CHECK(fake.argv == args);
    CHECK(fake.init_calls == 1); CHECK(fake.active);
    vita_tracy_auto_stop(); vita_tracy_auto_stop();
    CHECK(fake.stop_calls == 1); CHECK_FALSE(fake.active);
#if VITA_TRACY_AUTO_PMU
    CHECK(fake.attach_calls == 1); CHECK(fake.configure_calls == 1); CHECK(fake.start_calls == 1);
    CHECK(fake.config.size == sizeof(VitaTracyPmuConfig));
    CHECK(fake.config.abi_version == VITA_TRACY_ABI_VERSION);
    CHECK(fake.config.core_mask == 7); CHECK(fake.config.frequency_hz == 100);
    CHECK(fake.config.target_tid == 0); CHECK(fake.config.counter_count == 6);
    CHECK(fake.config.counters[0].event_code == 0x68);
#else
    CHECK(fake.attach_calls == 0); CHECK(fake.configure_calls == 0); CHECK(fake.start_calls == 0);
#endif
}
TEST_CASE_FIXTURE(Fixture, "a failed automatic initialization still runs the original main") {
    fake.init_result = -21;
    CHECK(__wrap_main(0, nullptr) == 47);
    CHECK(fake.main_calls == 1); CHECK(fake.attach_calls == 0);
    CHECK(fake.reports.empty()); CHECK_FALSE(fake.active);
    vita_tracy_auto_stop(); CHECK(fake.stop_calls == 0);
}
TEST_CASE_FIXTURE(Fixture, "automatic cleanup can retry a failed stop without re-enabling emission") {
    REQUIRE(__wrap_main(0, nullptr) == 47);
    fake.stop_result = -34;
    vita_tracy_auto_stop(); CHECK(fake.stop_calls == 1); CHECK_FALSE(fake.active);
    fake.stop_result = 0;
    vita_tracy_auto_stop(); CHECK(fake.stop_calls == 2); CHECK_FALSE(fake.active);
    vita_tracy_auto_stop(); CHECK(fake.stop_calls == 2);
}
TEST_CASE_FIXTURE(Fixture, "explicit process exit preserves status and real API result") {
    REQUIRE(__wrap_main(0, nullptr) == 47);
    CHECK(__wrap_sceKernelExitProcess(91) == -12);
    CHECK(fake.exit_status == 91); CHECK(fake.stop_calls == 1); CHECK_FALSE(fake.active);
}
#if VITA_TRACY_AUTO_PMU
TEST_CASE_FIXTURE(Fixture, "automatic PMU attach refusal leaves the HB and client running") {
    fake.attach_result = -41;
    CHECK(__wrap_main(0, nullptr) == 47);
    CHECK(fake.configure_calls == 0); CHECK(fake.start_calls == 0); CHECK(fake.active);
    REQUIRE(fake.reports.size() == 2); CHECK(fake.reports.back().second == -41);
}
TEST_CASE_FIXTURE(Fixture, "automatic PMU configuration failure never starts with stale settings") {
    fake.configure_result = -42;
    CHECK(__wrap_main(0, nullptr) == 47);
    CHECK(fake.start_calls == 0); CHECK(fake.active);
    CHECK(fake.reports.back().second == -42);
}
TEST_CASE_FIXTURE(Fixture, "automatic PMU start failures are present in capture metadata") {
    fake.start_result = -43;
    CHECK(__wrap_main(0, nullptr) == 47);
    CHECK(fake.reports.back().second == -43); CHECK(fake.active);
}
#endif
