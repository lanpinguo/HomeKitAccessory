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


typedef struct sal_mutex_s{
    char mutex_opaque_type;
} *sal_mutex_t;

typedef struct sal_sem_s{
    char sal_opaque_type;
} *sal_sem_t;

typedef struct sal_spinlock_s {
    char spinlock_opaque_type;
} *sal_spinlock_t;



typedef struct thread_info_s {
    void		(*f)(void *);
    char		*name;
    pthread_t		id;
    void		*arg;
    int                 ss;
    sal_sem_t           sem;
    struct thread_info_s *next;
} thread_info_t;


#if defined (__STRICT_ANSI__)
#define NO_CONTROL_C
#endif

#ifndef SAL_THREAD_RT_PRIO_HIGHEST
#define SAL_THREAD_RT_PRIO_HIGHEST  90
#endif


#define SAL_THREAD_ERROR	((sal_thread_t) -1)
#ifndef SAL_THREAD_STKSZ
#define	SAL_THREAD_STKSZ	16384	/* Default Stack Size */
#endif

#define SAL_THREAD_NAME_MAX_LEN     80 
#define SAL_THREAD_PRIO_NO_PREEMPT  -1


static const HAPLogObject logObject = { .subsystem = kHAPPlatform_LogSubsystem, .category = "Thread" };



 /* Function:
 *	sal_thread_self
 * Purpose:
 *	Return thread ID of caller
 * Parameters:
 *	None
 * Returns:
 *	Thread ID
 */

sal_thread_t
sal_thread_self(void)
{
    return (sal_thread_t) pthread_self();
}


/*
 * Function:
 *	HAPPlatformThreadCreate
 * Purpose:
 *	Abstraction for task creation
 * Parameters:
 *	name - name of task
 *	ss - stack size requested
 *	prio - scheduling prio (0 = highest, 255 = lowest)
 *	func - address of function to call
 *	arg - argument passed to func.
 * Returns:
 *	Thread ID
 */

sal_thread_t
HAPPlatformThreadCreate(char *name, int ss, int prio,
									void *(f)(void *), void * _Nullable arg)
{
    pthread_attr_t	attribs;
    struct sched_param param;
    pthread_t		id;

    if (pthread_attr_init(&attribs)) {
	    HAPLogError(&logObject, "pthread_attr_init %s fail",name) ;
        return(SAL_THREAD_ERROR);
    }

    ss += PTHREAD_STACK_MIN;
    pthread_attr_setstacksize(&attribs, ss);

    if (prio == SAL_THREAD_PRIO_NO_PREEMPT) {
        pthread_attr_setinheritsched(&attribs, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attribs, SCHED_FIFO);
        param.sched_priority = SAL_THREAD_RT_PRIO_HIGHEST;
        pthread_attr_setschedparam(&attribs, &param);
    }
    else
    {
      	if(prio < 99)
        {

          /*policy = RR*/
          pthread_attr_setdetachstate(&attribs, PTHREAD_CREATE_DETACHED);	/*tbd*/
          pthread_attr_setinheritsched(&attribs, PTHREAD_EXPLICIT_SCHED);
          pthread_attr_setschedpolicy(&attribs, SCHED_RR);
          param.sched_priority = prio;
          pthread_attr_setschedparam(&attribs, &param);
        }

    }



    if (pthread_create(&id, &attribs, f, (void *)arg)) {
	    HAPLogError(&logObject, "pthread_create %s fail",name) ;
    	return(SAL_THREAD_ERROR);
    }



    return ((sal_thread_t)id);
}

/*
 * Function:
 *	sal_thread_destroy
 * Purpose:
 *	Abstraction for task deletion
 * Parameters:
 *	thread - thread ID
 * Returns:
 *	0 on success, -1 on failure
 * Notes:
 *	This routine is not generally used by Broadcom drivers because
 *	it's unsafe.  If a task is destroyed while holding a mutex or
 *	other resource, system operation becomes unpredictable.  Also,
 *	some RTOS's do not include kill routines.
 *
 *	Instead, Broadcom tasks are written so they can be notified via
 *	semaphore when it is time to exit, at which time they call
 *	sal_thread_exit().
 */

int
HAPPlatformThreadDestroy(sal_thread_t thread)
{
#ifdef netbsd
    /* not supported */
    return -1;
#else
    pthread_t		id = (pthread_t) thread;

    if (pthread_cancel(id)) {
    	return -1;
    }

    return 0;
#endif
}

