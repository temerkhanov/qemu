/*
 * QEMU coroutines
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Stefan Hajnoczi    <stefanha@linux.vnet.ibm.com>
 *  Kevin Wolf         <kwolf@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "trace.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "qemu/coroutine.h"
#include "qemu/coroutine_int.h"
#include "block/aio.h"

enum {
    COROUTINE_POOL_SIZE = 16,
};

// TODO: implement a lockless stack here
typedef struct {
    uint8_t total;
    uint8_t top;
    Coroutine *pool[COROUTINE_POOL_SIZE];
} Coroutine_pool;

Coroutine_pool coroutine_pool = { .total = 0, .top = 0 };

Coroutine *qemu_coroutine_create(CoroutineEntry *entry, void *opaque)
{
    Coroutine *co = NULL;

    // Check for possible race
    if (atomic_read(&coroutine_pool.top) > 0) {
        co = coroutine_pool.pool[atomic_dec_fetch(&coroutine_pool.top)];
    } else {
        co = qemu_coroutine_new();
        atomic_inc(&coroutine_pool.total);
    }

    co->entry = entry;
    co->entry_arg = opaque;
    QSIMPLEQ_INIT(&co->co_queue_wakeup);
    return co;
}

static void coroutine_delete(Coroutine *co)
{
    co->caller = NULL;

    if (atomic_read(&coroutine_pool.total) >= COROUTINE_POOL_SIZE) {
        qemu_coroutine_delete(co);
        atomic_dec(&coroutine_pool.total);
    } else {
        coroutine_pool.pool[atomic_fetch_inc(&coroutine_pool.top)] = co;
    }
}

void qemu_aio_coroutine_enter(AioContext *ctx, Coroutine *co)
{
    QSIMPLEQ_HEAD(, Coroutine) pending = QSIMPLEQ_HEAD_INITIALIZER(pending);
    Coroutine *from = qemu_coroutine_self();

    QSIMPLEQ_INSERT_TAIL(&pending, co, co_queue_next);

    /* Run co and any queued coroutines */
    while (!QSIMPLEQ_EMPTY(&pending)) {
        Coroutine *to = QSIMPLEQ_FIRST(&pending);
        CoroutineAction ret;

        /* Cannot rely on the read barrier for to in aio_co_wake(), as there are
         * callers outside of aio_co_wake() */
        const char *scheduled = atomic_mb_read(&to->scheduled);

        QSIMPLEQ_REMOVE_HEAD(&pending, co_queue_next);

        trace_qemu_aio_coroutine_enter(ctx, from, to, to->entry_arg);

        /* if the Coroutine has already been scheduled, entering it again will
         * cause us to enter it twice, potentially even after the coroutine has
         * been deleted */
        if (scheduled) {
            fprintf(stderr,
                    "%s: Co-routine was already scheduled in '%s'\n",
                    __func__, scheduled);
            abort();
        }

        if (to->caller) {
            fprintf(stderr, "Co-routine re-entered recursively\n");
            abort();
        }

        to->caller = from;
        to->ctx = ctx;

        /* Store to->ctx before anything that stores to.  Matches
         * barrier in aio_co_wake and qemu_co_mutex_wake.
         */
        smp_wmb();

        ret = qemu_coroutine_switch(from, to, COROUTINE_ENTER);

        /* Queued coroutines are run depth-first; previously pending coroutines
         * run after those queued more recently.
         */
        QSIMPLEQ_PREPEND(&pending, &to->co_queue_wakeup);

        switch (ret) {
        case COROUTINE_YIELD:
            break;
        case COROUTINE_TERMINATE:
            assert(!to->locks_held);
            trace_qemu_coroutine_terminate(to);
            coroutine_delete(to);
            break;
        default:
            abort();
        }
    }
}

void qemu_coroutine_enter(Coroutine *co)
{
    qemu_aio_coroutine_enter(qemu_get_current_aio_context(), co);
}

void qemu_coroutine_enter_if_inactive(Coroutine *co)
{
    if (!qemu_coroutine_entered(co)) {
        qemu_coroutine_enter(co);
    }
}

void coroutine_fn qemu_coroutine_yield(void)
{
    Coroutine *self = qemu_coroutine_self();
    Coroutine *to = self->caller;

    trace_qemu_coroutine_yield(self, to);

    if (!to) {
        fprintf(stderr, "Co-routine is yielding to no one\n");
        abort();
    }

    self->caller = NULL;
    qemu_coroutine_switch(self, to, COROUTINE_YIELD);
}

bool qemu_coroutine_entered(Coroutine *co)
{
    return co->caller;
}

AioContext *coroutine_fn qemu_coroutine_get_aio_context(Coroutine *co)
{
    return co->ctx;
}
