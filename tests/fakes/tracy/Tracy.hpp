#pragma once
#include <cstddef>
void test_tracy_app_info(const char *, std::size_t);
void test_tracy_frame_mark(const char *);
#define TracyAppInfo(text, size) test_tracy_app_info(text, size)
#define FrameMarkNamed(name) test_tracy_frame_mark(name)
#define TracyMessage(text, size) ((void)0)
#define TracyPlot(name, value) ((void)0)
