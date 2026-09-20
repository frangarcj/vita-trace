#include <doctest/doctest.h>
#include <cstring>
#include <vector>
extern "C" {
#include "internal.h"
#include "firmware_exports.h"
#include "vita_tracy/kernel_abi.h"
}

namespace {
struct Fake {
    SceSize count = 1;
    int list_result = 0, info_result = 0, info_calls = 0;
    SceKernelModuleInfo info{};
    std::vector<VitaTraceControlRecord> records;
} fake;
struct Fixture {
    VitaTracyKernelState state{};
    Fixture() {
        fake = {};
        std::strcpy(fake.info.module_name, "probe");
        fake.info.segments[0].vaddr = reinterpret_cast<void *>(0x81230000u);
        fake.info.segments[0].memsz = 0x1000;
        fake.info.segments[0].perms = 5;
    }
};
}

extern "C" {
uint64_t vita_tracy_kernel_now(void) { return 1234567; }
int vita_tracy_fw_module_list(SceUID pid, int flags, int type, SceUID *ids, SceSize *count) {
    CHECK(pid == 123); CHECK(flags == 0x7FFFFFFF); CHECK(type == 1);
    SceSize capacity = *count;
    for (SceSize i = 0; i < capacity && i < fake.count; ++i) ids[i] = 42 + i;
    *count = fake.count;
    return fake.list_result;
}
int vita_tracy_fw_module_info(SceUID pid, SceUID id, SceKernelModuleInfo *info) {
    CHECK(pid == 123); CHECK(id >= 42);
    CHECK(info->size == sizeof(*info));
    ++fake.info_calls;
    *info = fake.info;
    return fake.info_result;
}
void vita_tracy_emit_control(VitaTracyKernelState *, const VitaTraceControlRecord *record) {
    fake.records.push_back(*record);
}
}

TEST_CASE_FIXTURE(Fixture, "module snapshots preserve original segment indices across empty slots") {
    fake.info.segments[2].vaddr = reinterpret_cast<void *>(0x90000000u);
    fake.info.segments[2].memsz = 0x2000;
    fake.info.segments[2].perms = 6;
    REQUIRE(vita_tracy_modules_snapshot(&state, 123) == 0);
    REQUIRE(fake.records.size() == 1);
    const auto &record = fake.records[0];
    CHECK(record.type == VITA_TRACE_MODULE_SNAPSHOT);
    CHECK(record.timestamp == 1234567);
    const auto &module = record.payload.module_snapshot;
    CHECK(module.pid == 123);
    CHECK(module.segment_count == 3);
    CHECK(module.segments[0].vaddr == 0x81230000u);
    CHECK(module.segments[1].memsz == 0);
    CHECK(module.segments[2].vaddr == 0x90000000u);
    CHECK(module.segments[2].perm == 6);
}
TEST_CASE_FIXTURE(Fixture, "module snapshots reject returned counts beyond the allocated UID array") {
    fake.count = 97;
    CHECK(vita_tracy_modules_snapshot(&state, 123) < 0);
    CHECK(fake.info_calls == 0);
    CHECK(fake.records.empty());
}
TEST_CASE_FIXTURE(Fixture, "module lookup failures never emit uninitialized metadata") {
    fake.info_result = -77;
    CHECK(vita_tracy_modules_snapshot(&state, 123) == 0);
    CHECK(fake.records.empty());
    fake.list_result = -88;
    fake.info_calls = 0;
    CHECK(vita_tracy_modules_snapshot(&state, 123) < 0);
    CHECK(fake.info_calls == 0);
}
TEST_CASE_FIXTURE(Fixture, "module names are bounded and an unavailable NID is explicitly zero") {
    std::memset(fake.info.module_name, 'A', sizeof(fake.info.module_name));
    REQUIRE(vita_tracy_modules_snapshot(&state, 123) == 0);
    const auto &module = fake.records.at(0).payload.module_snapshot;
    CHECK(module.module_name[VITA_TRACE_MODULE_NAME_MAX - 1] == '\0');
    CHECK(std::strlen(module.module_name) == VITA_TRACE_MODULE_NAME_MAX - 1);
    CHECK(module.module_nid == 0); // GetModuleInfo does not supply a NID.
}
TEST_CASE_FIXTURE(Fixture, "an empty module list performs no per-module queries") {
    fake.count = 0;
    CHECK(vita_tracy_modules_snapshot(&state, 123) == 0);
    CHECK(fake.info_calls == 0);
    CHECK(fake.records.empty());
}
