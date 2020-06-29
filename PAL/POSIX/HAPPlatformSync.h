// Copyright (c) 2015-2019 The HomeKit ADK Contributors
//
// Licensed under the Apache License, Version 2.0 (the â€œLicenseâ€?;
// you may not use this file except in compliance with the License.
// See [CONTRIBUTORS.md] for the list of HomeKit ADK project authors.

#ifndef HAP_PLATFORM_SYNC_H
#define HAP_PLATFORM_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "HAPPlatform.h"

#if __has_feature(nullability)
#pragma clang assume_nonnull begin
#endif

typedef struct sal_mutex_s{
    char mutex_opaque_type;
} *sal_mutex_t;

typedef struct sal_sem_s{
    char sal_opaque_type;
} *sal_sem_t;

typedef struct sal_spinlock_s {
    char spinlock_opaque_type;
} *sal_spinlock_t;

#define sal_mutex_FOREVER	(-1)
#define sal_mutex_NOWAIT	0

#define sal_sem_FOREVER		(-1)
#define sal_sem_BINARY		1
#define sal_sem_COUNTING	0

extern sal_mutex_t sal_mutex_create(char *desc);
extern void sal_mutex_destroy(sal_mutex_t m);

int sal_mutex_take(sal_mutex_t m, int usec);
int sal_mutex_give(sal_mutex_t m);
sal_sem_t _Nullable sal_sem_create(char *desc, int initial_count);
void sal_sem_destroy(sal_sem_t b);
int sal_sem_take(sal_sem_t b, int usec);
int sal_sem_give(sal_sem_t b);



#if __has_feature(nullability)
#pragma clang assume_nonnull end
#endif

#ifdef __cplusplus
}
#endif

#endif
