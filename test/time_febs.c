#include <stdio.h>		       /* for printf() */
#include <stdlib.h>		       /* for strtol() */
#include <assert.h>		       /* for assert() */
#include <qthread/qthread.h>
#include <qthread/qloop.h>
#include <qthread/qtimer.h>
#include "argparsing.h"

size_t TEST_SELECTION = 0xffffffff;
size_t ITERATIONS = 100000;
size_t MAXPARALLELISM = 256;
aligned_t incrementme = 0;
aligned_t *increments = NULL;

static void balanced_readFF(qthread_t * me, const size_t startat,
			  const size_t stopat, const size_t step, void *arg)
{
    size_t i;

    for (i = startat; i < stopat; i++) {
	qthread_readFF(me, NULL, (aligned_t*)arg);
    }
}

static void balanced_syncvar_readFF(qthread_t * me, const size_t startat,
			  const size_t stopat, const size_t step, void *arg)
{
    size_t i;

    for (i = startat; i < stopat; i++) {
	qthread_syncvar_readFF(me, NULL, (syncvar_t*)arg);
    }
}

static void balanced_falseshare_syncreadFF(qthread_t * me, const size_t startat,
				const size_t stopat, const size_t step, void *arg)
{
    size_t i;
    qthread_shepherd_id_t shep = qthread_shep(me);
    syncvar_t *myloc = ((syncvar_t*)arg) + shep;

    for (i = startat; i < stopat; i++) {
	qthread_syncvar_readFF(me, NULL, myloc);
    }
}

static void balanced_falseshare_readFF(qthread_t * me, const size_t startat,
				const size_t stopat, const size_t step, void *arg)
{
    size_t i;
    qthread_shepherd_id_t shep = qthread_shep(me);
    aligned_t *myloc = ((aligned_t*)arg) + shep;

    for (i = startat; i < stopat; i++) {
	qthread_readFF(me, NULL, myloc);
    }
}

static void balanced_noncomp_syncreadFF(qthread_t * me, const size_t startat,
			     const size_t stopat, const size_t step, void *arg)
{
    size_t i;
    syncvar_t myinc = SYNCVAR_STATIC_INITIALIZER;

    for (i = startat; i < stopat; i++) {
	qthread_syncvar_readFF(me, NULL, &myinc);
    }
}

static void balanced_noncomp_readFF(qthread_t * me, const size_t startat,
			     const size_t stopat, const size_t step, void *arg)
{
    size_t i;
    aligned_t myinc = 0;

    for (i = startat; i < stopat; i++) {
	qthread_readFF(me, NULL, &myinc);
    }
}

static char *human_readable_rate(double rate)
{
    static char readable_string[100] = { 0 };
    const double GB = 1024 * 1024 * 1024;
    const double MB = 1024 * 1024;
    const double kB = 1024;

    if (rate > GB) {
	snprintf(readable_string, 100, "(%.1f GB/s)", rate / GB);
    } else if (rate > MB) {
	snprintf(readable_string, 100, "(%.1f MB/s)", rate / MB);
    } else if (rate > kB) {
	snprintf(readable_string, 100, "(%.1f kB/s)", rate / kB);
    } else {
	memset(readable_string, 0, 100 * sizeof(char));
    }
    return readable_string;
}

int main(int argc, char *argv[])
{
    qtimer_t timer = qtimer_create();
    double rate;
    aligned_t *rets;
    unsigned int shepherds = 1;

    /* setup */
    assert(qthread_initialize() == QTHREAD_SUCCESS);

    CHECK_VERBOSE();
    NUMARG(ITERATIONS, "ITERATIONS");
    NUMARG(MAXPARALLELISM, "MAXPARALLELISM");
    NUMARG(TEST_SELECTION, "TEST_SELECTION");
    shepherds = qthread_num_shepherds();
    printf("%u shepherds...\n", shepherds);
    rets = malloc(sizeof(aligned_t) * MAXPARALLELISM);
    assert(rets);

    /* BALANCED SYNCVAR READFF LOOP (strong scaling) */
    if (TEST_SELECTION & 1) {
	syncvar_t *shared;
	printf("\tBalanced competing syncvar readFF: ");
	fflush(stdout);
	shared = (syncvar_t *) calloc(1, sizeof(syncvar_t));
	assert(shared);
	qtimer_start(timer);
	qt_loop_balance(0, MAXPARALLELISM * ITERATIONS, balanced_syncvar_readFF, shared);
	qtimer_stop(timer);
	free(shared);

	printf("%15g secs (%u-threads %u iters)\n", qtimer_secs(timer),
	       shepherds, (unsigned)(ITERATIONS * MAXPARALLELISM));
	iprintf("\t + average read time: %28g secs\n",
		qtimer_secs(timer) / (ITERATIONS * MAXPARALLELISM));
	printf("\t = read throughput: %30f reads/sec\n",
	       (ITERATIONS * MAXPARALLELISM) / qtimer_secs(timer));
	rate =
	    (ITERATIONS * MAXPARALLELISM * sizeof(aligned_t)) /
	    qtimer_secs(timer);
	printf("\t = data throughput: %30g bytes/sec %s\n", rate,
	       human_readable_rate(rate));
    }
    /* BALANCED READFF LOOP (strong scaling) */
    if (TEST_SELECTION & (1<<1)) {
	aligned_t *shared;
	printf("\tBalanced competing readFF: ");
	fflush(stdout);
	shared = (aligned_t *) calloc(1, sizeof(aligned_t));
	qtimer_start(timer);
	qt_loop_balance(0, MAXPARALLELISM * ITERATIONS, balanced_readFF, shared);
	qtimer_stop(timer);
	free(shared);

	printf("%23g secs (%u-threads %u iters)\n", qtimer_secs(timer),
	       shepherds, (unsigned)(ITERATIONS * MAXPARALLELISM));
	iprintf("\t + average read time: %28g secs\n",
		qtimer_secs(timer) / (ITERATIONS * MAXPARALLELISM));
	printf("\t = read throughput: %30f reads/sec\n",
	       (ITERATIONS * MAXPARALLELISM) / qtimer_secs(timer));
	rate =
	    (ITERATIONS * MAXPARALLELISM * sizeof(aligned_t)) /
	    qtimer_secs(timer);
	printf("\t = data throughput: %30g bytes/sec %s\n", rate,
	       human_readable_rate(rate));
    }

    if (TEST_SELECTION & (1 << 2)) {
	syncvar_t *shared;
	printf("\tBalanced false-sharing syncvar readFF: ");
	fflush(stdout);
	shared = (syncvar_t *) calloc(shepherds, sizeof(syncvar_t));
	qtimer_start(timer);
	qt_loop_balance(0, ITERATIONS * 256, balanced_falseshare_syncreadFF, shared);
	qtimer_stop(timer);
	free(shared);

	printf("%11g secs (%u-threads %u iters)\n", qtimer_secs(timer),
	       shepherds, (unsigned)(ITERATIONS * 256));
	iprintf("\t + average read time: %28g secs\n",
		qtimer_secs(timer) / (ITERATIONS * 256));
	printf("\t = read throughput: %30f reads/sec\n",
	       (ITERATIONS * 256) / qtimer_secs(timer));
	rate = (ITERATIONS * 256 * sizeof(aligned_t)) / qtimer_secs(timer);
	printf("\t = data throughput: %30g bytes/sec %s\n", rate,
	       human_readable_rate(rate));
    }

    if (TEST_SELECTION & (1 << 3)) {
	aligned_t *shared;
	printf("\tBalanced false-sharing readFF: ");
	fflush(stdout);
	shared = (aligned_t *) calloc(shepherds, sizeof(aligned_t));
	qtimer_start(timer);
	qt_loop_balance(0, ITERATIONS * 256, balanced_falseshare_readFF, shared);
	qtimer_stop(timer);
	free(shared);

	printf("%19g secs (%u-threads %u iters)\n", qtimer_secs(timer),
	       shepherds, (unsigned)(ITERATIONS * 256));
	iprintf("\t + average read time: %28g secs\n",
		qtimer_secs(timer) / (ITERATIONS * 256));
	printf("\t = read throughput: %30f reads/sec\n",
	       (ITERATIONS * 256) / qtimer_secs(timer));
	rate = (ITERATIONS * 256 * sizeof(aligned_t)) / qtimer_secs(timer);
	printf("\t = data throughput: %30g bytes/sec %s\n", rate,
	       human_readable_rate(rate));
    }

    if (TEST_SELECTION & (1 << 4)) {
	/* BALANCED INDEPENDENT LOOP */
	printf("\tBalanced independent syncvar readFF: ");
	fflush(stdout);
	qtimer_start(timer);
	qt_loop_balance(0, ITERATIONS * 256, balanced_noncomp_syncreadFF, NULL);
	qtimer_stop(timer);

	printf("%13g secs (%u-threads %u iters)\n", qtimer_secs(timer),
	       shepherds, (unsigned)(ITERATIONS * 256));
	iprintf("\t + average read time: %28g secs\n",
		qtimer_secs(timer) / (ITERATIONS * 256));
	printf("\t = read throughput: %30f reads/sec\n",
	       (ITERATIONS * 256) / qtimer_secs(timer));
	rate = (ITERATIONS * 256 * sizeof(aligned_t)) / qtimer_secs(timer);
	printf("\t = data throughput: %30g bytes/sec %s\n", rate,
	       human_readable_rate(rate));
    }

    if (TEST_SELECTION & (1 << 5)) {
	/* BALANCED INDEPENDENT LOOP */
	printf("\tBalanced independent readFF: ");
	fflush(stdout);
	qtimer_start(timer);
	qt_loop_balance(0, ITERATIONS * 256, balanced_noncomp_readFF, NULL);
	qtimer_stop(timer);

	printf("%21g secs (%u-threads %u iters)\n", qtimer_secs(timer),
	       shepherds, (unsigned)(ITERATIONS * 256));
	iprintf("\t + average read time: %28g secs\n",
		qtimer_secs(timer) / (ITERATIONS * 256));
	printf("\t = read throughput: %30f reads/sec\n",
	       (ITERATIONS * 256) / qtimer_secs(timer));
	rate = (ITERATIONS * 256 * sizeof(aligned_t)) / qtimer_secs(timer);
	printf("\t = data throughput: %30g bytes/sec %s\n", rate,
	       human_readable_rate(rate));
    }

    qtimer_destroy(timer);
    free(rets);

    return 0;
}