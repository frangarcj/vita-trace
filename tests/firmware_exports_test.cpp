#include <doctest/doctest.h>
#include <array>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "firmware_exports.h"
#include "vita_tracy/kernel_abi.h"
}

namespace {
struct Spec { const char *module; uint32_t library[2], function[2]; };
const Spec specs[] = {
    {"SceKernelModulemgr", {0xC445FA63, 0x92C9FFC2}, {0x97CF7B4E, 0xB72C75A4}},
    {"SceKernelModulemgr", {0xC445FA63, 0x92C9FFC2}, {0xD269F915, 0xDAA90093}},
    {"SceKernelThreadMgr", {0xA8CA0EFD, 0x7F8593BA}, {0xD8B9AC8D, 0x6C1F092F}},
    {"SceExcpmgr", {0x4CA0FDD5, 0x1496A5B5}, {0x03499636, 0x00063675}},
};
struct Fake {
    int generation = 0, missing = -1, null_success = -1;
    int lookups = 0, calls = 0;
    int error = 0;
} fake;
int list(SceUID pid, int flags, int type, SceUID *ids, SceSize *count) {
    ++fake.calls;
    CHECK(pid == 123); CHECK(flags == 9); CHECK(type == 1);
    CHECK(*count == 8); ids[0] = 42; *count = 1;
    return fake.error;
}
int info(SceUID pid, SceUID module, SceKernelModuleInfo *value) {
    ++fake.calls; CHECK(pid == 123); CHECK(module == 42);
    CHECK(value->size == sizeof(*value)); value->modid = 42;
    return fake.error;
}
int context(SceKernelThreadContextInfo *value) {
    ++fake.calls; value->process_id = 123; value->thread_id = 99;
    return fake.error;
}
int handler(SceExcpKind kind, int priority, void *node) {
    ++fake.calls; CHECK(kind == SCE_EXCP_IRQ); CHECK(priority == 0);
    CHECK(node != nullptr); *static_cast<uint32_t *>(node) = 0x100000;
    return fake.error;
}
const std::array<uintptr_t,4> addresses{{reinterpret_cast<uintptr_t>(list),
    reinterpret_cast<uintptr_t>(info), reinterpret_cast<uintptr_t>(context),
    reinterpret_cast<uintptr_t>(handler)}};
struct Fixture {
    Fixture() { fake = {}; vita_tracy_firmware_test_reset(); }
    void exercise() {
        const int before = fake.lookups;
        SceUID ids[8]{}; SceSize count = 8;
        CHECK(vita_tracy_fw_module_list(123, 9, 1, ids, &count) == fake.error);
        CHECK(count == 1); CHECK(ids[0] == 42);
        SceKernelModuleInfo module{}; module.size = sizeof(module);
        CHECK(vita_tracy_fw_module_info(123, 42, &module) == fake.error);
        CHECK(module.modid == 42);
        SceKernelThreadContextInfo thread{};
        CHECK(vita_tracy_fw_thread_context(&thread) == fake.error);
        CHECK(thread.process_id == 123); CHECK(thread.thread_id == 99);
        uint32_t node[2]{};
        CHECK(vita_tracy_fw_register_handler(SCE_EXCP_IRQ, 0, node) == fake.error);
        CHECK(node[0] == 0x100000);
        CHECK(fake.lookups == before); // Including the call made in IRQ context.
    }
};
}

extern "C" int vita_tracy_lookup_export(const char *module, uint32_t library,
                                         uint32_t function, uintptr_t *address) {
    ++fake.lookups;
    for (int i = 0; i < 4; ++i) {
        const auto &s = specs[i];
        if (std::strcmp(module, s.module) || library != s.library[fake.generation] ||
            function != s.function[fake.generation]) continue;
        if (i == fake.missing) { *address = addresses[i]; return -123; }
        *address = i == fake.null_success ? 0 : addresses[i];
        return 0;
    }
    *address = 0;
    return -100;
}

TEST_CASE_FIXTURE(Fixture, "firmware exports resolve the 360 variant and forward typed arguments") {
    REQUIRE(vita_tracy_firmware_init() == 0);
    CHECK(fake.lookups == 4);
    exercise();
}
TEST_CASE_FIXTURE(Fixture, "firmware exports fall back to the known 363 variant") {
    fake.generation = 1;
    REQUIRE(vita_tracy_firmware_init() == 0);
    CHECK(fake.lookups == 8);
    exercise();
}
TEST_CASE_FIXTURE(Fixture, "firmware lookup failure never publishes a partial callable table") {
    for (int missing = 0; missing < 4; ++missing) {
        vita_tracy_firmware_test_reset(); fake.missing = missing;
        CHECK(vita_tracy_firmware_init() == VITA_TRACY_ERROR_UNSUPPORTED);
        CHECK(vita_tracy_fw_module_list(0, 0, 0, nullptr, nullptr) == VITA_TRACY_ERROR_UNSUPPORTED);
        CHECK(vita_tracy_fw_module_info(0, 0, nullptr) == VITA_TRACY_ERROR_UNSUPPORTED);
        CHECK(vita_tracy_fw_thread_context(nullptr) == VITA_TRACY_ERROR_UNSUPPORTED);
        CHECK(vita_tracy_fw_register_handler(SCE_EXCP_IRQ, 0, nullptr) == VITA_TRACY_ERROR_UNSUPPORTED);
        CHECK(fake.calls == 0);
    }
}
TEST_CASE_FIXTURE(Fixture, "firmware resolver rejects success with a null export address") {
    fake.null_success = 3;
    CHECK(vita_tracy_firmware_init() == VITA_TRACY_ERROR_UNSUPPORTED);
    CHECK(vita_tracy_fw_thread_context(nullptr) == VITA_TRACY_ERROR_UNSUPPORTED);
    CHECK(fake.calls == 0);
}
TEST_CASE_FIXTURE(Fixture, "firmware initialization is idempotent and no lookups occur in callbacks") {
    REQUIRE(vita_tracy_firmware_init() == 0);
    int before = fake.lookups;
    fake.missing = 0;
    REQUIRE(vita_tracy_firmware_init() == 0);
    CHECK(fake.lookups == before);
    exercise();
}
TEST_CASE_FIXTURE(Fixture, "firmware function errors pass through unchanged") {
    REQUIRE(vita_tracy_firmware_init() == 0);
    fake.error = -77;
    exercise();
}
TEST_CASE_FIXTURE(Fixture, "firmware resolution can be retried after an incomplete lookup") {
    fake.missing = 2;
    REQUIRE(vita_tracy_firmware_init() < 0);
    fake.missing = -1;
    REQUIRE(vita_tracy_firmware_init() == 0);
    exercise();
}
