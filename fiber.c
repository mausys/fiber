#include "fiber.h"

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ucontext.h>


#include <sys/queue.h>

#define NUM_THREADS 16

typedef enum {
    STATE_NEW,
    STATE_RUNNABLE,
    STATE_WAITING,
    STATE_TERMINATED,
} state_t;


typedef struct fiber_main fiber_main_t;
typedef struct fiber fiber_t;


struct fiber {
    CIRCLEQ_ENTRY(fiber) qe;
    ucontext_t ctx;
    state_t state;
    fiber_main_t *main;
    fid_t id;
    fiber_start_fn start;
    void *user_data;
};


struct fiber_main {
    ucontext_t ctx;
    unsigned n;
    uint32_t uid;
    fid_t current;
    fiber_t **list;
    CIRCLEQ_HEAD(active_q, fiber) active;
    struct {
        unsigned n;
        unsigned *indices;
    } empty;
    struct {
        fiber_next_fn next;
        void *user_data;
    } sched;
};


static fiber_main_t* fiber_main_new(void);

 static fiber_main_t *g_main_fiber = NULL;

static fiber_main_t *fiber_main_instance(void)
{
    if (!g_main_fiber)
        g_main_fiber = fiber_main_new();

    return g_main_fiber;
}



static fid_t round_robin(fid_t current, void *user_data)
{
    fiber_main_t *main = fiber_main_instance();

    if (!main)
        return FIBER_ID_INVAL;

    if (!CIRCLEQ_FIRST(&main->active))
        return FIBER_ID_INVAL;

    if (current == FIBER_ID_INVAL)
        return CIRCLEQ_FIRST(&main->active)->id;

    unsigned index = current & FIBER_INDEX_MASK;

    if (index >= main->n)
        return FIBER_ID_INVAL;

    fiber_t *fiber = main->list[index];

    if (!fiber)
        return FIBER_ID_INVAL;

    fiber_t *next = CIRCLEQ_NEXT(fiber, qe);

    return next->id;
}



static int resize_fiber_list(fiber_main_t *main)
{
    /* double the size of the fiber vector */
    unsigned extend = main->n;
    fiber_t **list = realloc(main->list, (main->n + extend) * sizeof(fiber_t*));

    if (!list)
        return -ENOMEM;

    main->list = list;

    unsigned *indices = realloc(main->empty.indices, (main->n + extend) * sizeof(unsigned));

    if (!indices)
        return -ENOMEM;

    main->empty.indices = indices;

    main->n += extend;
    main->empty.n = extend;

    for (unsigned i = 0; i < extend; i++)
        main->empty.indices[i] = main->n - 1 - i;

    return extend;
}

static int add_fiber(fiber_main_t *main, fiber_t *fiber)
{
    if (main->empty.n == 0) {
        int r = resize_fiber_list(main);

        if (r < 0)
            return r;
    }

    fid_t idx = main->empty.indices[main->empty.n - 1];
    main->empty.n--;

    main->list[idx] = fiber;

    fiber->main = main;
    fiber->id = idx | ((fid_t)main->uid << FIBER_ID_SHIFT)  ;
    main->uid++;

    CIRCLEQ_INSERT_HEAD(&main->active, fiber, qe);

    return idx;
}

static fiber_main_t* fiber_main_new(void)
{
    fiber_main_t *main = malloc(sizeof(fiber_main_t));

    if (!main)
        goto fail_alloc;

    *main = (fiber_main_t) {
        .current = FIBER_ID_INVAL,
        .sched.next = round_robin,
    };

    CIRCLEQ_INIT(&main->active);

    main->list = calloc(NUM_THREADS, sizeof(fiber_t*));

    if (!main->list)
        goto fail_alloc_list;

    main->n = NUM_THREADS;

    main->empty.indices = malloc(main->n * sizeof(unsigned));

    if (!main->empty.indices)
        goto fail_alloc_empty;

    main->empty.n = main->n;

    for (unsigned i = 0; i < main->empty.n; i++)
        main->empty.indices[i] = main->empty.n - 1 - i;

    return main;

fail_alloc_empty:
    free(main->list);
fail_alloc_list:
    free(main);
fail_alloc:
    return NULL;
}


static fiber_t* current_fiber(void)
{
    fiber_main_t *main = fiber_main_instance();

    if (!main)
        return NULL;

    unsigned index = main->current & FIBER_INDEX_MASK;

    if (index >= main->n)
        return NULL;

    return main->list[index];
}

static void fiber_entry(void)
{
    fiber_t* self = current_fiber();

    self->start(self->user_data);

    self->state = STATE_TERMINATED;
}


static void fiber_delete(fiber_t *fiber)
{
    fiber_main_t *main = fiber->main;

    unsigned idx = fiber->id & FIBER_INDEX_MASK;

    main->list[idx] = NULL;

    main->empty.n++;
    main->empty.indices[main->empty.n - 1] = idx;

    CIRCLEQ_REMOVE(&main->active, fiber, qe);

    free(fiber->ctx.uc_stack.ss_sp);
    free(fiber);
}


static void exec_fiber(fiber_t *fiber)
{
    fiber->main->current = fiber->id;
    swapcontext(&fiber->main->ctx, &fiber->ctx);
}


void fiber_reset(void)
{
    fiber_main_t *main = fiber_main_instance();

    if (!main)
        return;

    for (unsigned i = 0; i < main->n; i++) {
        if (!main->list[i])
            continue;

        fiber_delete(main->list[i]);
    }

    free(main->list);

    free(main->empty.indices);

    free(main);

    g_main_fiber = NULL;
}

void fiber_set_sched(fiber_next_fn next, void *user_data)
{
    fiber_main_t *main = fiber_main_instance();

    if (!main)
        return;

    main->sched.next = next;
    main->sched.user_data = user_data;
}


int fiber_run(void)
{
    fiber_main_t *main = fiber_main_instance();

    if (!main)
        return -ENOMEM;

    int r = -EINVAL;

    if (main->current != FIBER_ID_INVAL)
        return -EALREADY;

    fiber_t *fiber = NULL;

    for (;;) {
        if (!main->sched.next)
            break;

        fid_t fid = main->sched.next(main->current, main->sched.user_data);

        if (fiber) {
            if (fiber->state == STATE_TERMINATED)
                fiber_delete(fiber);
        }

        if (fid == FIBER_ID_INVAL) {
            r = 0;
            break;
        }

        unsigned index = fid & FIBER_INDEX_MASK;

        if (index >= main->n)
            break;

        fiber = main->list[index];

        if (!fiber)
            break;

        if (fid != fiber->id)
            break;

        exec_fiber(fiber);
    }

    return r;
}


fid_t fiber_new(size_t stack_size, fiber_start_fn start, void* user_data)
{
    fiber_main_t *main = fiber_main_instance();

    if (!main)
        return FIBER_ID_INVAL;

    fiber_t *fiber = malloc(sizeof(fiber_t));

    if (!fiber)
        goto fail_alloc;

    *fiber = (fiber_t) {
        .state = STATE_NEW,
        .start = start,
        .user_data = user_data,
    };

    int r = getcontext(&fiber->ctx);

    if (r < 0)
        goto fail_getcontext;

    void *stack = malloc(stack_size);

    if (!stack)
        goto fail_stack;

    fiber->ctx.uc_link = &main->ctx;
    fiber->ctx.uc_stack.ss_size = stack_size;
    fiber->ctx.uc_stack.ss_sp = stack;

    makecontext(&fiber->ctx, fiber_entry, 0);

    r = add_fiber(main, fiber);

    if (r < 0)
        goto fail_add;

    return fiber->id;

fail_add:
    free(stack);
fail_stack:
fail_getcontext:
    free(fiber);
fail_alloc:
    return FIBER_ID_INVAL;
}



fid_t fiber_self(void)
{
    fiber_t *fiber = current_fiber();

    if (!fiber)
        return FIBER_ID_INVAL;

    return fiber->id;
}


fid_t fiber_get_id(const fiber_t *fiber)
{
    return fiber->id;
}


void fiber_yield(void)
{
    fiber_t *fiber = current_fiber();

    if (!fiber)
        return;

    swapcontext(&fiber->ctx, &fiber->main->ctx);
}


