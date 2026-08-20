#include <doctest/doctest.h>

#include "vita_tracy/state.h"

namespace {

bool allowed(VitaTracyState from, VitaTracyStateEvent event, VitaTracyState expected) {
    VitaTracyState next = VITA_TRACY_STATE_UNINITIALIZED;
    if (!vita_tracy_state_next(from, event, &next)) {
        return false;
    }
    return next == expected;
}

bool rejected(VitaTracyState from, VitaTracyStateEvent event) {
    VitaTracyState next = VITA_TRACY_STATE_UNINITIALIZED;
    return vita_tracy_state_next(from, event, &next) == 0;
}

} // namespace

TEST_CASE("the documented happy path is walkable") {
    CHECK(allowed(VITA_TRACY_STATE_UNINITIALIZED, VITA_TRACY_EVENT_INIT, VITA_TRACY_STATE_READY));
    CHECK(allowed(VITA_TRACY_STATE_READY, VITA_TRACY_EVENT_ATTACH, VITA_TRACY_STATE_ATTACHED));
    CHECK(allowed(VITA_TRACY_STATE_ATTACHED, VITA_TRACY_EVENT_START, VITA_TRACY_STATE_PROFILING));
    CHECK(allowed(VITA_TRACY_STATE_PROFILING, VITA_TRACY_EVENT_STOP, VITA_TRACY_STATE_STOPPED));
    CHECK(allowed(VITA_TRACY_STATE_STOPPED, VITA_TRACY_EVENT_DETACH, VITA_TRACY_STATE_READY));
}

TEST_CASE("a stopped session can be restarted without reattaching") {
    CHECK(allowed(VITA_TRACY_STATE_STOPPED, VITA_TRACY_EVENT_START, VITA_TRACY_STATE_PROFILING));
}

TEST_CASE("detach is accepted from every attached state") {
    // Process death mid-capture must still release the mapping.
    CHECK(allowed(VITA_TRACY_STATE_ATTACHED, VITA_TRACY_EVENT_DETACH, VITA_TRACY_STATE_READY));
    CHECK(allowed(VITA_TRACY_STATE_PROFILING, VITA_TRACY_EVENT_DETACH, VITA_TRACY_STATE_READY));
    CHECK(allowed(VITA_TRACY_STATE_STOPPED, VITA_TRACY_EVENT_DETACH, VITA_TRACY_STATE_READY));
}

TEST_CASE("nothing is accepted before the module is initialized") {
    CHECK(rejected(VITA_TRACY_STATE_UNINITIALIZED, VITA_TRACY_EVENT_ATTACH));
    CHECK(rejected(VITA_TRACY_STATE_UNINITIALIZED, VITA_TRACY_EVENT_START));
    CHECK(rejected(VITA_TRACY_STATE_UNINITIALIZED, VITA_TRACY_EVENT_STOP));
    CHECK(rejected(VITA_TRACY_STATE_UNINITIALIZED, VITA_TRACY_EVENT_DETACH));
}

TEST_CASE("sampling cannot start before a target is attached") {
    CHECK(rejected(VITA_TRACY_STATE_READY, VITA_TRACY_EVENT_START));
    CHECK(rejected(VITA_TRACY_STATE_READY, VITA_TRACY_EVENT_STOP));
    CHECK(rejected(VITA_TRACY_STATE_READY, VITA_TRACY_EVENT_DETACH));
}

TEST_CASE("a second target cannot be attached over a live session") {
    CHECK(rejected(VITA_TRACY_STATE_ATTACHED, VITA_TRACY_EVENT_ATTACH));
    CHECK(rejected(VITA_TRACY_STATE_PROFILING, VITA_TRACY_EVENT_ATTACH));
    CHECK(rejected(VITA_TRACY_STATE_STOPPED, VITA_TRACY_EVENT_ATTACH));
}

TEST_CASE("redundant start and stop are rejected") {
    CHECK(rejected(VITA_TRACY_STATE_PROFILING, VITA_TRACY_EVENT_START));
    CHECK(rejected(VITA_TRACY_STATE_ATTACHED, VITA_TRACY_EVENT_STOP));
    CHECK(rejected(VITA_TRACY_STATE_STOPPED, VITA_TRACY_EVENT_STOP));
}

TEST_CASE("initializing twice is rejected") {
    CHECK(rejected(VITA_TRACY_STATE_READY, VITA_TRACY_EVENT_INIT));
    CHECK(rejected(VITA_TRACY_STATE_PROFILING, VITA_TRACY_EVENT_INIT));
}

TEST_CASE("a rejected transition leaves the caller's state untouched") {
    VitaTracyState next = VITA_TRACY_STATE_PROFILING;
    CHECK(vita_tracy_state_next(VITA_TRACY_STATE_READY, VITA_TRACY_EVENT_START, &next) == 0);
    CHECK(next == VITA_TRACY_STATE_PROFILING);
}

TEST_CASE("every state has a name for logging") {
    CHECK(vita_tracy_state_name(VITA_TRACY_STATE_UNINITIALIZED) != nullptr);
    CHECK(vita_tracy_state_name(VITA_TRACY_STATE_READY) != nullptr);
    CHECK(vita_tracy_state_name(VITA_TRACY_STATE_ATTACHED) != nullptr);
    CHECK(vita_tracy_state_name(VITA_TRACY_STATE_PROFILING) != nullptr);
    CHECK(vita_tracy_state_name(VITA_TRACY_STATE_STOPPED) != nullptr);
}
