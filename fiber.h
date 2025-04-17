#pragma once

#include <stdint.h>
#include <stddef.h>

typedef uint64_t fid_t;
#define FIBER_INDEX_MASK UINT32_MAX
#define FIBER_ID_INVAL UINT64_MAX
#define FIBER_ID_SHIFT 32

typedef void (*fiber_start_fn)(void *);
typedef fid_t (*fiber_next_fn)(fid_t current, void *user_data);


void fiber_main_set_sched(fiber_next_fn next, void *user_data);

int fiber_run(void);

fid_t fiber_self(void);

fid_t fiber_new(size_t stack_sice, fiber_start_fn start, void* user_data);


void fiber_yield(void);



