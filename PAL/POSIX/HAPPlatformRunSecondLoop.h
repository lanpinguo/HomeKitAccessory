// Copyright (c) 2015-2019 The HomeKit ADK Contributors
//
// Licensed under the Apache License, Version 2.0 (the â€œLicenseâ€?;
// you may not use this file except in compliance with the License.
// See [CONTRIBUTORS.md] for the list of HomeKit ADK project authors.

#ifndef HAP_PLATFORM_RUN_SECOND_LOOP_INIT_H
#define HAP_PLATFORM_RUN_SECOND_LOOP_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "HAPPlatform.h"

#if __has_feature(nullability)
#pragma clang assume_nonnull begin
#endif


/**
 * Create run loop.
 */
void HAPPlatformRunSecondLoopCreate(void);

/**
 * Release run loop.
 */
void HAPPlatformRunSecondLoopRelease(void);

void HAPPlatformRunSecondLoopRun(void);

HAPError HAPPlatformSecondFileHandleRegister(
        HAPPlatformFileHandleRef* fileHandle_,
        int fileDescriptor,
        HAPPlatformFileHandleEvent interests,
        HAPPlatformFileHandleCallback callback,
        void* _Nullable context);

void HAPPlatformSecondFileHandleUpdateInterests(
        HAPPlatformFileHandleRef fileHandle_,
        HAPPlatformFileHandleEvent interests,
        HAPPlatformFileHandleCallback callback,
        void* _Nullable context);


#if __has_feature(nullability)
#pragma clang assume_nonnull end
#endif

#ifdef __cplusplus
}
#endif

#endif
