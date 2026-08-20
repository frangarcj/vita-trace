#include "vita_tracy/pmu.h"

uint32_t vita_tracy_pmu_delta(uint32_t previous, uint32_t current) {
    return current - previous;
}
