#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#if (HAVE_MEMALIGN && HAVE_MALLOC_H)
# include <malloc.h>                   /* for memalign() */
#endif

#include "qthread/qthread.h"
#include "qthread/cacheline.h"
#include "qthread_asserts.h"
#include "qt_shepherd_innards.h"
#include "qthread_expect.h"
#include "qt_visibility.h"

#include "qthread/qt_sinc.h"

typedef aligned_t qt_sinc_count_t;

typedef struct qt_sinc_reduction_ {
    // Value-related info
    void *restrict values;
    qt_sinc_op_f   op;
    void *restrict result;
    void *restrict initial_value;
    size_t         sizeof_value;
    size_t         sizeof_shep_value_part;
    size_t         sizeof_shep_count_part;
} qt_sinc_reduction_t;

typedef struct qt_sinc_s {
    qt_sinc_count_t      counter;
    aligned_t            ready;
    qt_sinc_reduction_t *rdata;
} qt_internal_sinc_t;

static size_t       num_sheps;
static size_t       num_workers;
static size_t       num_wps;
static unsigned int cacheline;

#ifdef HAVE_MEMALIGN
# define ALIGNED_ALLOC(val, size, align) (val) = memalign((align), (size))
#elif defined(HAVE_POSIX_MEMALIGN)
# define ALIGNED_ALLOC(val, size, align) posix_memalign((void **)&(val), (align), (size))
#elif defined(HAVE_WORKING_VALLOC)
# define ALIGNED_ALLOC(val, size, align) (val) = valloc((size))
#elif defined(HAVE_PAGE_ALIGNED_MALLOC)
# define ALIGNED_ALLOC(val, size, align) (val) = malloc((size))
#else
# define ALIGNED_ALLOC(val, size, align) (val) = valloc((size)) /* cross your fingers! */
#endif

void qt_sinc_init(qt_sinc_t *restrict  sinc_,
                  size_t               sizeof_value,
                  const void *restrict initial_value,
                  qt_sinc_op_f         op,
                  size_t               expect)
{
    assert((0 == sizeof_value && NULL == initial_value) ||
           (0 != sizeof_value && NULL != initial_value));
    qt_internal_sinc_t *const restrict sinc = (struct qt_sinc_s *)sinc_;
    assert(sinc);

    if (QTHREAD_EXPECT((num_sheps == 0), 0)) {
        num_sheps   = qthread_readstate(TOTAL_SHEPHERDS);
        num_workers = qthread_readstate(TOTAL_WORKERS);
        num_wps     = num_workers / num_sheps;
        cacheline   = qthread_cacheline();
    }

    if (sizeof_value == 0) {
        sinc->rdata = NULL;
    } else {
        const size_t                        sizeof_shep_values     = num_wps * sizeof_value;
        const size_t                        num_lines_per_shep     = ceil(sizeof_shep_values * 1.0 / cacheline);
        const size_t                        num_lines              = num_sheps * num_lines_per_shep;
        const size_t                        sizeof_shep_value_part = num_lines_per_shep * cacheline;
        qt_sinc_reduction_t *const restrict rdata                  = sinc->rdata = malloc(sizeof(qt_sinc_reduction_t));
        assert(rdata);
        rdata->op            = op;
        rdata->sizeof_value  = sizeof_value;
        rdata->initial_value = malloc(2 * sizeof_value);
        assert(rdata->initial_value);
        memcpy(rdata->initial_value, initial_value, sizeof_value);
        rdata->result = ((uint8_t *)rdata->initial_value) + sizeof_value;
        assert(rdata->result);

        rdata->sizeof_shep_value_part = sizeof_shep_value_part;

        ALIGNED_ALLOC(rdata->values, num_lines * cacheline, cacheline);
        assert(rdata->values);

        // Initialize values
        for (size_t s = 0; s < num_sheps; s++) {
            const size_t shep_offset = s * sizeof_shep_value_part;
            for (size_t w = 0; w < num_wps; w++) {
                const size_t worker_offset = w * rdata->sizeof_value;
                memcpy((uint8_t *)rdata->values + shep_offset + worker_offset,
                       initial_value,
                       sizeof_value);
            }
        }
    }
    sinc->counter = expect;
    if (sinc->counter != 0) {
        qthread_empty(&sinc->ready);
    } else {
        qthread_fill(&sinc->ready);
    }
    assert(NULL == sinc->rdata || 
           ((sinc->rdata->result && sinc->rdata->initial_value) || 
            (!sinc->rdata->result && !sinc->rdata->initial_value)));
}

qt_sinc_t *qt_sinc_create(const size_t sizeof_value,
                          const void  *initial_value,
                          qt_sinc_op_f op,
                          const size_t will_spawn)
{
    qt_sinc_t *const restrict sinc = malloc(sizeof(qt_sinc_t));

    assert(sinc);

    qt_sinc_init(sinc, sizeof_value, initial_value, op, will_spawn);

    return sinc;
}

void qt_sinc_reset(qt_sinc_t   *sinc_,
                   const size_t will_spawn)
{
    qt_internal_sinc_t *const restrict  sinc  = (qt_internal_sinc_t *)sinc_;
    assert(sinc && (0 == sinc->counter));
    
    qt_sinc_reduction_t *const restrict rdata = sinc->rdata;

    // Reset values
    if (NULL != rdata) {
        const size_t sizeof_shep_value_part = rdata->sizeof_shep_value_part;
        const size_t sizeof_value           = rdata->sizeof_value;
        for (size_t s = 0; s < num_sheps; s++) {
            const size_t shep_offset = s * sizeof_shep_value_part;
            for (size_t w = 0; w < num_wps; w++) {
                const size_t worker_offset = w * sizeof_value;
                memcpy((uint8_t *)rdata->values + shep_offset + worker_offset,
                       rdata->initial_value,
                       sizeof_value);
            }
        }
    }

    // Reset termination detection
    sinc->counter = will_spawn;
    if (sinc->counter != 0) {
        qthread_empty(&sinc->ready);
    } else {
        qthread_fill(&sinc->ready);
    }
}

void qt_sinc_fini(qt_sinc_t *sinc_)
{
    qt_internal_sinc_t *const restrict sinc = (qt_internal_sinc_t *)sinc_;
    assert(sinc);

    if (sinc->rdata) {
        qt_sinc_reduction_t *const restrict rdata = sinc->rdata;
        assert(rdata->result);
        assert(rdata->initial_value);
        free(rdata->initial_value);
        assert(rdata->values);
        free(rdata->values);
    }
}

void qt_sinc_destroy(qt_sinc_t *sinc_)
{
    qt_sinc_fini(sinc_);
    free(sinc_);
}

/* Adds a new participant to the sinc.
 * Pre:  sinc was created
 * Post: aggregate count is positive
 */
void qt_sinc_willspawn(qt_sinc_t *sinc_,
                       size_t     count)
{
    qt_internal_sinc_t *const restrict sinc = (qt_internal_sinc_t *)sinc_;
    assert(sinc);

    if (count != 0) {
        if (qthread_incr(&sinc->counter, count) == 0) {
            qthread_empty(&sinc->ready);
        }
    }

    assert(sinc && (0 < sinc->counter));

    return;
}

void *qt_sinc_tmpdata(qt_sinc_t *sinc_)
{
    qt_internal_sinc_t *const restrict sinc = (qt_internal_sinc_t *)sinc_;
    assert(sinc);

    if (NULL != sinc->rdata) {
        qt_sinc_reduction_t *const restrict rdata         = sinc->rdata;
        const size_t                        shep_offset   = qthread_shep() * rdata->sizeof_shep_value_part;
        const size_t                        worker_offset = qthread_readstate(CURRENT_WORKER) * rdata->sizeof_value;
        return (uint8_t *)rdata->values + shep_offset + worker_offset;
    } else {
        return NULL;
    }
}

static void qt_sinc_internal_collate(qt_sinc_t *sinc_)
{
    qt_internal_sinc_t *const restrict sinc = (qt_internal_sinc_t *)sinc_;
    assert(sinc);

    if (sinc->rdata) {
        qt_sinc_reduction_t *const restrict rdata = sinc->rdata;

        // step 1: collate results
        const size_t sizeof_value           = rdata->sizeof_value;
        const size_t sizeof_shep_value_part = rdata->sizeof_shep_value_part;

        memcpy(rdata->result, rdata->initial_value, sizeof_value);
        for (qthread_shepherd_id_t s = 0; s < num_sheps; ++s) {
            const size_t shep_offset = s * sizeof_shep_value_part;
            for (size_t w = 0; w < num_wps; ++w) {
                rdata->op(rdata->result,
                          (uint8_t *)rdata->values + shep_offset + (w * sizeof_value));
            }
        }
    }

    // step 2: release waiters
    qthread_fill(&sinc->ready);
}

void qt_sinc_submit(qt_sinc_t *restrict sinc_,
                    void *restrict      value)
{
    qt_internal_sinc_t *const restrict sinc = (qt_internal_sinc_t *)sinc_;
    assert(sinc);

    if (value) {
        // Update value
        qt_sinc_reduction_t *const restrict rdata = sinc->rdata;
        assert(sinc->rdata);
        assert((rdata->result && rdata->initial_value) || (!rdata->result && !rdata->initial_value));

        const size_t sizeof_shep_value_part = rdata->sizeof_shep_value_part;
        const size_t sizeof_value           = rdata->sizeof_value;

        const qthread_shepherd_id_t shep_id   = qthread_shep();
        const qthread_worker_id_t   worker_id = qthread_readstate(CURRENT_WORKER);

        if (NULL != value) {
            const size_t shep_offset   = shep_id * sizeof_shep_value_part;
            const size_t worker_offset = worker_id * sizeof_value;
            void        *values        = (uint8_t *)rdata->values + shep_offset + worker_offset;

            rdata->op(values, value);
        }
    }

    // Update counter
    qt_sinc_count_t count = qthread_incr(&sinc->counter, -1);
    assert(count > 0);
    if (1 == count) { // This is the final submit
        qt_sinc_internal_collate(sinc_);
    }

    return;
}

void qt_sinc_wait(qt_sinc_t *restrict sinc_,
                  void *restrict      target)
{
    qt_internal_sinc_t *const restrict sinc = (qt_internal_sinc_t *)sinc_;
    assert(sinc);
    assert(NULL == sinc->rdata ||
           (((NULL != sinc->rdata->values) && (0 < sinc->rdata->sizeof_value) && sinc->rdata->op) || (NULL == target)));

    qthread_readFF(NULL, &sinc->ready);

    // XXX: race with many waiters, few cores - first waiter to finish hits `qt_sinc_destroy()`.
    // XXX: need a count of waiters and barrier to protect access to sinc members.
    if (target) {
        assert(sinc->rdata->sizeof_value > 0);
        memcpy(target, sinc->rdata->result, sinc->rdata->sizeof_value);
    }
}

/* vim:set expandtab: */
