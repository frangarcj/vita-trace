#include "vita_tracy/shared_layout.h"

#include "vita_tracy/abi.h"
#include "vita_tracy/kernel_events.h"
#include "vita_tracy/shared_ring.h"
#include <string.h>

static size_t align_up(size_t value) {
    return (value + (VITA_TRACE_SHARED_ALIGN - 1u)) & ~(size_t)(VITA_TRACE_SHARED_ALIGN - 1u);
}

static int capacities_valid(uint32_t sample_capacity, uint32_t control_capacity) {
    return vita_trace_is_pow2(sample_capacity) && vita_trace_is_pow2(control_capacity);
}

size_t vita_trace_shared_layout_size(uint32_t sample_capacity, uint32_t control_capacity) {
    if (!capacities_valid(sample_capacity, control_capacity)) {
        return 0;
    }

    uint64_t sample_bytes = vita_trace_ring_layout_size(sample_capacity, sizeof(VitaTraceSample));
    uint64_t control_bytes = vita_trace_ring_layout_size(control_capacity, sizeof(VitaTraceControlRecord));
    if (!sample_bytes || !control_bytes) return 0;
    uint64_t offset = align_up(sizeof(VitaTraceSharedHeader));
    uint64_t sample_ring = (sample_bytes + VITA_TRACE_SHARED_ALIGN - 1u) &
                           ~(uint64_t)(VITA_TRACE_SHARED_ALIGN - 1u);
    offset += sample_ring * VITA_TRACE_CORE_COUNT;
    offset += (control_bytes + VITA_TRACE_SHARED_ALIGN - 1u) &
               ~(uint64_t)(VITA_TRACE_SHARED_ALIGN - 1u);
    return offset <= UINT32_MAX ? (size_t)offset : 0;
}

int vita_trace_shared_init(void *mem, size_t mem_size, uint32_t target_pid, uint32_t timebase_hz,
                           uint32_t sample_capacity, uint32_t control_capacity) {
    if (mem == NULL || !capacities_valid(sample_capacity, control_capacity)) {
        return 0;
    }

    size_t required = vita_trace_shared_layout_size(sample_capacity, control_capacity);
    if (required == 0 || mem_size < required) {
        return 0;
    }

    VitaTraceSharedHeader *hdr = (VitaTraceSharedHeader *)mem;
    hdr->kernel_ack = 0;
    hdr->abi_version = VITA_TRACY_ABI_VERSION;
    hdr->target_pid = target_pid;
    hdr->timebase_hz = timebase_hz;
    hdr->enabled_features = 0;
    hdr->core_count = VITA_TRACE_CORE_COUNT;
    hdr->sample_capacity = sample_capacity;
    hdr->control_capacity = control_capacity;

    size_t offset = align_up(sizeof(VitaTraceSharedHeader));
    size_t sample_ring = align_up(vita_trace_ring_layout_size(sample_capacity, sizeof(VitaTraceSample)));

    for (uint32_t cpu = 0; cpu < VITA_TRACE_CORE_COUNT; ++cpu) {
        if (!vita_trace_ring_init((uint8_t *)mem + offset, mem_size - offset, sample_capacity,
                                  sizeof(VitaTraceSample))) {
            return 0;
        }
        hdr->core_ring_offset[cpu] = (uint32_t)offset;
        offset += sample_ring;
    }

    if (!vita_trace_ring_init((uint8_t *)mem + offset, mem_size - offset, control_capacity,
                              sizeof(VitaTraceControlRecord))) {
        return 0;
    }
    hdr->control_ring_offset = (uint32_t)offset;

    /* Published last so a consumer never sees the magic before the rings
     * behind it are initialized. */
    __atomic_store_n(&hdr->magic, VITA_TRACE_SHARED_MAGIC, __ATOMIC_RELEASE);
    return 1;
}

int vita_trace_shared_is_valid(const void *mem) {
    if (mem == NULL) {
        return 0;
    }
    const VitaTraceSharedHeader *hdr = (const VitaTraceSharedHeader *)mem;
    if (__atomic_load_n(&hdr->magic, __ATOMIC_ACQUIRE) != VITA_TRACE_SHARED_MAGIC) {
        return 0;
    }
    return hdr->abi_version == VITA_TRACY_ABI_VERSION;
}

int vita_trace_shared_validate_layout(const void *mem, size_t size) {
    if (size < sizeof(VitaTraceSharedHeader) || !vita_trace_shared_is_valid(mem)) return 0;
    const VitaTraceSharedHeader *hdr = (const VitaTraceSharedHeader *)mem;
    const size_t required = vita_trace_shared_layout_size(hdr->sample_capacity, hdr->control_capacity);
    if (!required || required > size || hdr->core_count != VITA_TRACE_CORE_COUNT) return 0;
    size_t offset = align_up(sizeof(VitaTraceSharedHeader));
    for (uint32_t i = 0; i <= VITA_TRACE_CORE_COUNT; ++i) {
        const int sample = i < VITA_TRACE_CORE_COUNT;
        const uint32_t declared = sample ? hdr->core_ring_offset[i] : hdr->control_ring_offset;
        const uint32_t capacity = sample ? hdr->sample_capacity : hdr->control_capacity;
        const uint32_t element = sample ? sizeof(VitaTraceSample) : sizeof(VitaTraceControlRecord);
        if (declared != offset) return 0;
        const VitaTraceRingHeader *ring = (const VitaTraceRingHeader *)((const uint8_t *)mem + offset);
        if (ring->magic != VITA_TRACE_RING_MAGIC || ring->capacity != capacity ||
            ring->capacity_mask != capacity - 1u || ring->element_size != element) return 0;
        offset += align_up(vita_trace_ring_layout_size(capacity, element));
    }
    return offset == required;
}

void vita_trace_shared_acknowledge(void *mem) {
    if (!vita_trace_shared_is_valid(mem)) {
        return;
    }
    VitaTraceSharedHeader *hdr = (VitaTraceSharedHeader *)mem;
    __atomic_store_n(&hdr->kernel_ack, VITA_TRACE_KERNEL_ACK, __ATOMIC_RELEASE);
}

int vita_trace_shared_is_acknowledged(const void *mem) {
    if (!vita_trace_shared_is_valid(mem)) {
        return 0;
    }
    const VitaTraceSharedHeader *hdr = (const VitaTraceSharedHeader *)mem;
    return __atomic_load_n(&hdr->kernel_ack, __ATOMIC_ACQUIRE) == VITA_TRACE_KERNEL_ACK;
}

void *vita_trace_shared_core_ring(void *mem, uint32_t cpu) {
    if (!vita_trace_shared_is_valid(mem) || cpu >= VITA_TRACE_CORE_COUNT) {
        return NULL;
    }
    VitaTraceSharedHeader *hdr = (VitaTraceSharedHeader *)mem;
    return (uint8_t *)mem + hdr->core_ring_offset[cpu];
}

void *vita_trace_shared_control_ring(void *mem) {
    if (!vita_trace_shared_is_valid(mem)) {
        return NULL;
    }
    VitaTraceSharedHeader *hdr = (VitaTraceSharedHeader *)mem;
    return (uint8_t *)mem + hdr->control_ring_offset;
}
