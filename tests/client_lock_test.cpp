#include <doctest/doctest.h>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
#include "tracy_vita_lock.hpp"

static_assert(!std::is_copy_constructible<VitaTracyLockGuard>::value, "lock ownership is unique");
static_assert(!std::is_move_constructible<VitaTracyLockGuard>::value, "lock ownership cannot move");

TEST_CASE("client lock uses a static initializer and releases at scope exit") {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    {
        VitaTracyLockGuard lock(&mutex);
    }
    REQUIRE(pthread_mutex_trylock(&mutex) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
    CHECK(pthread_mutex_destroy(&mutex) == 0);
}

TEST_CASE("client lock releases while unwinding an exception") {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    CHECK_THROWS_AS(([&] {
        VitaTracyLockGuard lock(&mutex);
        throw std::runtime_error("test unwind");
    }()), std::runtime_error);
    REQUIRE(pthread_mutex_trylock(&mutex) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
    CHECK(pthread_mutex_destroy(&mutex) == 0);
}

TEST_CASE("client lock serializes real concurrent workers") {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    unsigned total = 0;
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 6; ++i) {
        workers.emplace_back([&] {
            for (unsigned j = 0; j < 2000; ++j) {
                VitaTracyLockGuard lock(&mutex);
                ++total;
            }
        });
    }
    for (auto &worker : workers) worker.join();
    CHECK(total == 12000);
    CHECK(pthread_mutex_destroy(&mutex) == 0);
}
