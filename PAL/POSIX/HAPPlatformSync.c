// Copyright (c) 2015-2019 The HomeKit ADK Contributors
//
// Licensed under the Apache License, Version 2.0 (the â€œLicenseâ€?;
// you may not use this file except in compliance with the License.
// See [CONTRIBUTORS.md] for the list of HomeKit ADK project authors.

#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>



#include "HAPPlatform.h"
#include "HAPPlatformThread.h"
#include "HAPPlatformSync.h"

/*
 * Constants
 */

#define MILLISECOND_USEC        (1000)
#define SECOND_USEC            (1000000)
#define MINUTE_USEC            (60 * SECOND_USEC)
#define HOUR_USEC            (60 * MINUTE_USEC)

#define SECOND_MSEC            (1000)
#define MINUTE_MSEC            (60 * SECOND_MSEC)

#define SAL_USECS_TIMESTAMP_ROLLOVER    ((uint32)(0xFFFFFFFF))    

#ifndef SECOND_NSEC
#define SECOND_NSEC     (SECOND_USEC * 1000)
#endif


#if defined(BROADCOM_DEBUG) && defined(INCLUDE_BCM_SAL_PROFILE)
#include "string.h"

static unsigned int _sal_sem_count_curr;
static unsigned int _sal_sem_count_max;
static unsigned int _sal_mutex_count_curr;
static unsigned int _sal_mutex_count_max;

#define FILE_LOC_NAME_MAX 128
#define MUTEX_DBG_ARR_MAX 5000
#define DEBUG_SAL_PROFILE 1

#define SAL_SEM_RESOURCE_USAGE_INCR(a_curr, a_max)             \
    do {                                                       \
        a_curr++;                                              \
        a_max = ((a_curr) > (a_max)) ? (a_curr) : (a_max);     \
    } while (0)
    
#define SAL_SEM_RESOURCE_USAGE_DECR(a_curr) \
    do {                                    \
      a_curr--;                             \
    } while(0)

#else
#define SAL_SEM_RESOURCE_USAGE_INCR(a_curr, a_max)  ;
#define SAL_SEM_RESOURCE_USAGE_DECR(a_curr)         ;
#endif



/*
 * recursive_mutex_t
 *
 *   This is an abstract type built on the POSIX mutex that allows a
 *   mutex to be taken recursively by the same thread without deadlock.
 *
 *   The Linux version of pthreads supports recursive mutexes
 *   (a non-portable extension to posix). In this case, we 
 *   use the Linux support instead of our own. 
 */

typedef struct recursive_mutex_s {
    int             val;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    char            *desc;
    sal_thread_t    owner;
    int             recurse_count;

#ifdef BROADCOM_DEBUG_MUTEX
    unsigned int ctrl_c_blk;
    unsigned int take_count;
    unsigned int give_count;
    unsigned int tk_exc_gv_ind;
    char         prev_file_tk_location[FILE_LOC_NAME_MAX];
    char         last_file_tk_location[FILE_LOC_NAME_MAX];

    char         prev_file_gv_location[FILE_LOC_NAME_MAX];
    char         last_file_gv_location[FILE_LOC_NAME_MAX];
#endif

} recursive_mutex_t;


static const HAPLogObject logObject = { .subsystem = kHAPPlatform_LogSubsystem, .category = "sync" };

/*
 * Keyboard interrupt protection
 *
 *   When a thread is running on a console, the user could Control-C
 *   while a mutex is held by the thread.  Control-C results in a signal
 *   that longjmp's somewhere else.  We prevent this from happening by
 *   blocking Control-C signals while any mutex is held.
 */
static void ctrl_c_block(void)
{
}

static void ctrl_c_unblock(void)
{
}

static void _init_ctrl_c(void) 
{
}

static int
_sal_compute_timeout(struct timespec *ts, int usec)
{
    int sec;
    uint32_t nsecs;

    if (clock_gettime(CLOCK_MONOTONIC, ts)) {
        if (clock_gettime(CLOCK_REALTIME, ts)) {
            struct timeval  ltv;

            /* Fall back to RTC if realtime clock unavailable */
            gettimeofday(&ltv, 0);
            ts->tv_sec = ltv.tv_sec;
            ts->tv_nsec = ltv.tv_usec * 1000;
        }
    }

    /* Add in the delay */
    ts->tv_sec += usec / SECOND_USEC;

    /* compute new nsecs */
    nsecs = ts->tv_nsec + (usec % SECOND_USEC) * 1000;

    /* detect and handle rollover */
    if (nsecs < ts->tv_nsec) {
        ts->tv_sec += 1;
        nsecs -= SECOND_NSEC;
    }
    ts->tv_nsec = nsecs;

    /* Normalize if needed */
    sec = ts->tv_nsec / SECOND_NSEC;
    if (sec) {
        ts->tv_sec += sec;
        ts->tv_nsec = ts->tv_nsec % SECOND_NSEC;
    }

    /* indicate that we successfully got the time */
    return 1;
}



static sal_mutex_t
_sal_mutex_create(char *desc)
{
    recursive_mutex_t   *rm;
    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t  cond_attr;
    struct timespec     ts;

    _init_ctrl_c();

    if ((rm = malloc(sizeof (recursive_mutex_t))) == NULL) {
	    HAPLogError(&logObject, "ERROR in %s", __func__) ;
        return NULL;
    }

    rm->desc          = desc;
    rm->owner         = 0;
    rm->recurse_count = 0;
    rm->val           = 1;

    pthread_mutexattr_init(&mutex_attr);
    pthread_mutex_init(&rm->mutex, &mutex_attr);

    pthread_condattr_init(&cond_attr);
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    }
    pthread_cond_init(&rm->cond, &cond_attr);

#ifdef BROADCOM_DEBUG_MUTEX
    {
        int i;

        rm->ctrl_c_blk = 0;
        rm->take_count = 0;
        rm->give_count = 0;
        rm->tk_exc_gv_ind = 0;

        sal_memset(rm->prev_file_tk_location, FILE_LOC_NAME_MAX, 0);
        sal_memset(rm->last_file_tk_location, FILE_LOC_NAME_MAX, 0);
        sal_memset(rm->prev_file_gv_location, FILE_LOC_NAME_MAX, 0);
        sal_memset(rm->last_file_gv_location, FILE_LOC_NAME_MAX, 0);

        for (i = 0; i < MUTEX_DBG_ARR_MAX; i++) {
            /* Find an empty slot */
            if (mutex_dbg_arr_ptr[i] == 0) {
                mutex_dbg_arr_ptr[i] = rm;
                break;
            }
        }
    }
#endif /* BROADCOM_DEBUG_MUTEX */

    SAL_SEM_RESOURCE_USAGE_INCR(_sal_mutex_count_curr, _sal_mutex_count_max);

    return (sal_mutex_t) rm;
}

/*
 * Mutex and semaphore abstraction
 */
sal_mutex_t
sal_mutex_create(char *desc)
{
#ifdef SAL_GLOBAL_MUTEX
    static sal_mutex_t _m = NULL;
    if (!_m) {
        _m = _sal_mutex_create("sal_global_mutex");
        HAPAssert(_m);
    }
    if (strcmp(desc, "spl mutex")) {
        return _m;
    }
#endif

    return _sal_mutex_create(desc);
}

void
sal_mutex_destroy(sal_mutex_t m)
{
    recursive_mutex_t   *rm = (recursive_mutex_t *) m;

    HAPAssert(rm);

    pthread_mutex_destroy(&rm->mutex);
    pthread_cond_destroy(&rm->cond);

    free(rm);

    SAL_SEM_RESOURCE_USAGE_DECR(_sal_mutex_count_curr);
}

/*
 * Both semaphores and mutexes are secretly mutexes, so the core of the take
 * logic can be shared between them
 */
static int 
sal_mutex_sem_take(pthread_mutex_t *mutex, pthread_cond_t *cond, 
                   int *val, int forever, int usec) 
{
    struct timespec ts;
    int             err = 0;

    if ((!forever) && (!_sal_compute_timeout(&ts, usec))) {
        err = -1; 
    }

    if (err == 0) {
        err = pthread_mutex_lock(mutex);

        while ((*val == 0) && (err == 0)) {
            if (forever) {
                err = pthread_cond_wait(cond, mutex);
            } else {
                err = pthread_cond_timedwait(cond, mutex, &ts);
            }
            
        }

        if (err == 0) {
            *val -= 1;
        }

        /* even if there's an error, try to unlock this... */
        pthread_mutex_unlock(mutex);
    }

    return err ? -1 : 0;
}

int
sal_mutex_take(sal_mutex_t m, int usec)
{
    recursive_mutex_t *rm = (recursive_mutex_t *)m;
    sal_thread_t      myself = sal_thread_self();
    int               err = 0;

    HAPAssert(rm);

    if (rm->owner == myself) {
        rm->recurse_count++;
        return 0;
    }

    ctrl_c_block();

#ifdef BROADCOM_DEBUG_MUTEX
    rm->ctrl_c_blk++;
#endif

    err = sal_mutex_sem_take(&rm->mutex, &rm->cond, &rm->val, 
                             usec == sal_mutex_FOREVER, usec);

    rm->owner = myself;

    if (err) {
        ctrl_c_unblock();

#ifdef BROADCOM_DEBUG_MUTEX
	    HAPLogError(&logObject, "ERROR in TAKING MUTEX") ;
        rm->ctrl_c_blk--;
#endif
        
        return -1;
    }
 
    return err ? -1 : 0;
}


int
sal_mutex_give(sal_mutex_t m)
{
    recursive_mutex_t  *rm = (recursive_mutex_t *) m;
    int                err = 0;
    sal_thread_t       myself = sal_thread_self();

    HAPAssert(rm);

    HAPAssert(rm->owner == myself);

    if (rm->recurse_count > 0) {
        rm->recurse_count--;
        return 0;
    }
    rm->owner = 0;
    pthread_mutex_lock(&rm->mutex);
    rm->val++;
    err = pthread_cond_broadcast(&rm->cond);
    /* even if there's an error, try to unlock this... */
    pthread_mutex_unlock(&rm->mutex);
    ctrl_c_unblock();

#ifdef BROADCOM_DEBUG_MUTEX
    rm->ctrl_c_blk--;
#endif

    return err ? -1 : 0;
}


/*
 * Wrapper class to hold additional info
 * along with the semaphore.
 */
typedef struct {
    int             val;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    char            *desc;
} wrapped_sem_t;

sal_sem_t _Nullable
sal_sem_create(char *desc, int initial_count)
{
    wrapped_sem_t       *s = NULL;
    struct timespec     ts;
    pthread_condattr_t  cond_attr;
    pthread_mutexattr_t mutex_attr;

    /* Ignore binary for now */

    if ((s = malloc(sizeof (wrapped_sem_t))) == NULL) {
        return NULL;
    }

    s->val = initial_count;
    s->desc = desc;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutex_init(&s->mutex, &mutex_attr);

    pthread_condattr_init(&cond_attr);
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    }
    pthread_cond_init(&s->cond, &cond_attr);

    SAL_SEM_RESOURCE_USAGE_INCR(_sal_sem_count_curr, _sal_sem_count_max);

    return (sal_sem_t) s;
}

void
sal_sem_destroy(sal_sem_t b)
{
    wrapped_sem_t *s = (wrapped_sem_t *) b;

    HAPAssert(s);

    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->cond);

    free(s);

    SAL_SEM_RESOURCE_USAGE_DECR(_sal_sem_count_curr);
}

int
sal_sem_take(sal_sem_t b, int usec)
{
    wrapped_sem_t    *s = (wrapped_sem_t *) b;
    int              err = 0;

    err = sal_mutex_sem_take(&s->mutex, &s->cond, &s->val, 
                             usec == sal_sem_FOREVER, usec);

    return err ? -1 : 0;
}

int
sal_sem_give(sal_sem_t b)
{
    wrapped_sem_t *s = (wrapped_sem_t *) b;
    int           err;

    pthread_mutex_lock(&s->mutex);
    s->val++;
    err = pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mutex);

    return err ? -1 : 0;
}


