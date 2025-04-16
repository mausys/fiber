#pragma once

#include <stdint.h>
#include <stddef.h>

typedef uint64_t fid_t;
#define FIBER_INDEX_MASK UINT32_MAX
#define FIBER_ID_INVAL UINT64_MAX
#define FIBER_ID_SHIFT 32

typedef struct fiber fiber_t;
typedef struct fiber_main fiber_main_t;
typedef void (*fiber_start_fn)(void *);
typedef fid_t (*fiber_next_fn)(fiber_main_t* main, fid_t current, void *user_data);


fiber_main_t* fiber_main_instance(void);
void fiber_main_set_sched(fiber_main_t* main, fiber_next_fn next, void *user_data);
int fiber_main_run(fiber_main_t* main);

fiber_t* fiber_self(void);

fiber_t* fiber_new(fiber_main_t *main, size_t stack_sice, fiber_start_fn start, void* user_data);
fid_t fiber_get_id(const fiber_t *fiber);


void fiber_yield(void);



