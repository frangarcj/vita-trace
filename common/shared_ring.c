#include "vita_tracy/shared_ring.h"

#include <string.h>

static VitaTraceRingHeader *header_of(void *mem) {
    return (VitaTraceRingHeader *)mem;
}

static const VitaTraceRingHeader *const_header_of(const void *mem) {
    return (const VitaTraceRingHeader *)mem;
}

static uint8_t *records_of(void *mem) {
    return (uint8_t *)mem + sizeof(VitaTraceRingHeader);
}

int vita_trace_is_pow2(uint32_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

size_t vita_trace_ring_layout_size(uint32_t capacity, uint32_t element_size) {
    return sizeof(VitaTraceRingHeader) + (size_t)capacity * (size_t)element_size;
}

int vita_trace_ring_init(void *mem, size_t mem_size, uint32_t capacity, uint32_t element_size) {
    if (mem == NULL || capacity == 0 || element_size == 0) {
        return 0;
    }
    if (!vita_trace_is_pow2(capacity)) {
        return 0;
    }
    if (mem_size < vita_trace_ring_layout_size(capacity, element_size)) {
        return 0;
    }

    VitaTraceRingHeader *hdr = header_of(mem);
    hdr->magic = VITA_TRACE_RING_MAGIC;
    hdr->element_size = element_size;
    hdr->capacity = capacity;
    hdr->capacity_mask = capacity - 1u;
    __atomic_store_n(&hdr->write_pos, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&hdr->read_pos, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&hdr->dropped, 0u, __ATOMIC_RELAXED);
    return 1;
}

int vita_trace_ring_try_push(void *mem, const void *element) {
    VitaTraceRingHeader *hdr = header_of(mem);
    uint32_t write_pos = __atomic_load_n(&hdr->write_pos, __ATOMIC_RELAXED);
    uint32_t read_pos = __atomic_load_n(&hdr->read_pos, __ATOMIC_ACQUIRE);

    if (write_pos - read_pos >= hdr->capacity) {
        __atomic_fetch_add(&hdr->dropped, 1u, __ATOMIC_RELAXED);
        return 0;
    }

    uint32_t index = write_pos & hdr->capacity_mask;
    memcpy(records_of(mem) + (size_t)index * hdr->element_size, element, hdr->element_size);
    __atomic_store_n(&hdr->write_pos, write_pos + 1u, __ATOMIC_RELEASE);
    return 1;
}

int vita_trace_ring_try_pop(void *mem, void *out_element) {
    VitaTraceRingHeader *hdr = header_of(mem);
    uint32_t read_pos = __atomic_load_n(&hdr->read_pos, __ATOMIC_RELAXED);
    uint32_t write_pos = __atomic_load_n(&hdr->write_pos, __ATOMIC_ACQUIRE);

    if (read_pos == write_pos) {
        return 0;
    }

    uint32_t index = read_pos & hdr->capacity_mask;
    memcpy(out_element, records_of(mem) + (size_t)index * hdr->element_size, hdr->element_size);
    __atomic_store_n(&hdr->read_pos, read_pos + 1u, __ATOMIC_RELEASE);
    return 1;
}

uint32_t vita_trace_ring_dropped(const void *mem) {
    const VitaTraceRingHeader *hdr = const_header_of(mem);
    return __atomic_load_n(&hdr->dropped, __ATOMIC_RELAXED);
}

uint32_t vita_trace_ring_pending(const void *mem) {
    const VitaTraceRingHeader *hdr = const_header_of(mem);
    uint32_t write_pos = __atomic_load_n(&hdr->write_pos, __ATOMIC_ACQUIRE);
    uint32_t read_pos = __atomic_load_n(&hdr->read_pos, __ATOMIC_ACQUIRE);
    return write_pos - read_pos;
}
