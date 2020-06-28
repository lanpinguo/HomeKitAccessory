// Copyright (c) 2015-2019 The HomeKit ADK Contributors
//
// Licensed under the Apache License, Version 2.0 (the â€œLicenseâ€?;
// you may not use this file except in compliance with the License.
// See [CONTRIBUTORS.md] for the list of HomeKit ADK project authors.

// This implementation is based on `select` for maximum portability but may be extended to also support
// `poll`, `epoll` or `kqueue`.

#include "HAPPlatform.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>

#include "HAPPlatform+Init.h"
#include "HAPPlatformFileHandle.h"
#include "HAPPlatformLog+Init.h"
#include "HAPPlatformRunLoop+Init.h"

static const HAPLogObject logObject = { .subsystem = kHAPPlatform_LogSubsystem, .category = "RunLoop" };

/**
 * Internal file handle type, representing the registration of a platform-specific file descriptor.
 */
typedef struct HAPPlatformFileHandle HAPPlatformFileHandle;

/**
 * Internal file handle representation.
 */
struct HAPPlatformFileHandle {
    /**
     * Platform-specific file descriptor.
     */
    int fileDescriptor;

    /**
     * Set of file handle events on which the callback shall be invoked.
     */
    HAPPlatformFileHandleEvent interests;

    /**
     * Function to call when one or more events occur on the given file descriptor.
     */
    HAPPlatformFileHandleCallback callback;

    /**
     * The context parameter given to the HAPPlatformFileHandleRegister function.
     */
    void* _Nullable context;

    /**
     * Previous file handle in linked list.
     */
    HAPPlatformFileHandle* _Nullable prevFileHandle;

    /**
     * Next file handle in linked list.
     */
    HAPPlatformFileHandle* _Nullable nextFileHandle;

    /**
     * Flag indicating whether the platform-specific file descriptor is registered with an I/O multiplexer or not.
     */
    bool isAwaitingEvents;
};

/**
 * Internal timer type.
 */
typedef struct HAPPlatformTimer HAPPlatformTimer;

/**
 * Internal timer representation.
 */
struct HAPPlatformTimer {
    /**
     * Deadline at which the timer expires.
     */
    HAPTime deadline;

    /**
     * Callback that is invoked when the timer expires.
     */
    HAPPlatformTimerCallback callback;

    /**
     * The context parameter given to the HAPPlatformTimerRegister function.
     */
    void* _Nullable context;

    /**
     * Next timer in linked list.
     */
    HAPPlatformTimer* _Nullable nextTimer;
};

/**
 * Run loop state.
 */
HAP_ENUM_BEGIN(uint8_t, HAPPlatformRunLoopState) { /**
                                                    * Idle.
                                                    */
                                                   kHAPPlatformRunLoopState_Idle,

                                                   /**
                                                    * Running.
                                                    */
                                                   kHAPPlatformRunLoopState_Running,

                                                   /**
                                                    * Stopping.
                                                    */
                                                   kHAPPlatformRunLoopState_Stopping
} HAP_ENUM_END(uint8_t, HAPPlatformRunLoopState);

static struct {
    /**
     * Sentinel node of a circular doubly-linked list of file handles
     */
    HAPPlatformFileHandle fileHandleSentinel;

    /**
     * Pointer to sentinel node, representing a circular doubly-linked list of file handles
     */
    HAPPlatformFileHandle* _Nullable fileHandles;

    /**
     * File handle cursor, used to handle reentrant modifications of global file handle list during iteration.
     */
    HAPPlatformFileHandle* _Nullable fileHandleCursor;

    /**
     * Start of linked list of timers, ordered by deadline.
     */
    HAPPlatformTimer* _Nullable timers;

    /**
     * Self-pipe file descriptor to receive data.
     */
    volatile int selfPipeFileDescriptor0;

    /**
     * Self-pipe file descriptor to send data.
     */
    volatile int selfPipeFileDescriptor1;

    /**
     * Self-pipe byte buffer.
     *
     * - Callbacks are serialized into the buffer as:
     *   - 8-byte aligned callback pointer.
     *   - Context size (up to UINT8_MAX).
     *   - Context (unaligned). When invoking the callback, the context is first moved to be 8-byte aligned.
     */
    HAP_ALIGNAS(8)
    char selfPipeBytes[sizeof(HAPPlatformRunLoopCallback) + 1 + UINT8_MAX];

    /**
     * Number of bytes in self-pipe byte buffer.
     */
    size_t numSelfPipeBytes;

    /**
     * File handle for self-pipe.
     */
    HAPPlatformFileHandleRef selfPipeFileHandle;

    /**
     * Current run loop state.
     */
    HAPPlatformRunLoopState state;
} runSecondLoop = { .fileHandleSentinel = { .fileDescriptor = -1,
                                      .interests = { .isReadyForReading = false,
                                                     .isReadyForWriting = false,
                                                     .hasErrorConditionPending = false },
                                      .callback = NULL,
                                      .context = NULL,
                                      .prevFileHandle = &runSecondLoop.fileHandleSentinel,
                                      .nextFileHandle = &runSecondLoop.fileHandleSentinel,
                                      .isAwaitingEvents = false },
              .fileHandles = &runSecondLoop.fileHandleSentinel,
              .fileHandleCursor = &runSecondLoop.fileHandleSentinel,

              .timers = NULL,

              .selfPipeFileDescriptor0 = -1,
              .selfPipeFileDescriptor1 = -1 };

HAP_RESULT_USE_CHECK
HAPError HAPPlatformSecondFileHandleRegister(
        HAPPlatformFileHandleRef* fileHandle_,
        int fileDescriptor,
        HAPPlatformFileHandleEvent interests,
        HAPPlatformFileHandleCallback callback,
        void* _Nullable context) {
    HAPPrecondition(fileHandle_);

    // Prepare fileHandle.
    HAPPlatformFileHandle* fileHandle = calloc(1, sizeof(HAPPlatformFileHandle));
    if (!fileHandle) {
        HAPLog(&logObject, "Cannot allocate more file handles.");
        *fileHandle_ = 0;
        return kHAPError_OutOfResources;
    }
    fileHandle->fileDescriptor = fileDescriptor;
    fileHandle->interests = interests;
    fileHandle->callback = callback;
    fileHandle->context = context;
    fileHandle->prevFileHandle = runSecondLoop.fileHandles->prevFileHandle;
    fileHandle->nextFileHandle = runSecondLoop.fileHandles;
    fileHandle->isAwaitingEvents = false;
    runSecondLoop.fileHandles->prevFileHandle->nextFileHandle = fileHandle;
    runSecondLoop.fileHandles->prevFileHandle = fileHandle;

    *fileHandle_ = (HAPPlatformFileHandleRef) fileHandle;
    return kHAPError_None;
}

void HAPPlatformSecondFileHandleUpdateInterests(
        HAPPlatformFileHandleRef fileHandle_,
        HAPPlatformFileHandleEvent interests,
        HAPPlatformFileHandleCallback callback,
        void* _Nullable context) {
    HAPPrecondition(fileHandle_);
    HAPPlatformFileHandle* fileHandle = (HAPPlatformFileHandle * _Nonnull) fileHandle_;

    fileHandle->interests = interests;
    fileHandle->callback = callback;
    fileHandle->context = context;
}

void HAPPlatformSecondFileHandleDeregister(HAPPlatformFileHandleRef fileHandle_) {
    HAPPrecondition(fileHandle_);
    HAPPlatformFileHandle* fileHandle = (HAPPlatformFileHandle * _Nonnull) fileHandle_;

    HAPPrecondition(fileHandle->prevFileHandle);
    HAPPrecondition(fileHandle->nextFileHandle);

    if (fileHandle == runSecondLoop.fileHandleCursor) {
        runSecondLoop.fileHandleCursor = fileHandle->nextFileHandle;
    }

    fileHandle->prevFileHandle->nextFileHandle = fileHandle->nextFileHandle;
    fileHandle->nextFileHandle->prevFileHandle = fileHandle->prevFileHandle;

    fileHandle->fileDescriptor = -1;
    fileHandle->interests.isReadyForReading = false;
    fileHandle->interests.isReadyForWriting = false;
    fileHandle->interests.hasErrorConditionPending = false;
    fileHandle->callback = NULL;
    fileHandle->context = NULL;
    fileHandle->nextFileHandle = NULL;
    fileHandle->prevFileHandle = NULL;
    fileHandle->isAwaitingEvents = false;
    HAPPlatformFreeSafe(fileHandle);
}

static void ProcessSelectedFileHandles(
        fd_set* readFileDescriptors,
        fd_set* writeFileDescriptors,
        fd_set* errorFileDescriptors) {
    HAPPrecondition(readFileDescriptors);
    HAPPrecondition(writeFileDescriptors);
    HAPPrecondition(errorFileDescriptors);

    runSecondLoop.fileHandleCursor = runSecondLoop.fileHandles->nextFileHandle;
    while (runSecondLoop.fileHandleCursor != runSecondLoop.fileHandles) {
        HAPPlatformFileHandle* fileHandle = runSecondLoop.fileHandleCursor;
        runSecondLoop.fileHandleCursor = fileHandle->nextFileHandle;

        if (fileHandle->isAwaitingEvents) {
            HAPAssert(fileHandle->fileDescriptor != -1);
            fileHandle->isAwaitingEvents = false;
            if (fileHandle->callback) {
                HAPPlatformFileHandleEvent fileHandleEvents;
                fileHandleEvents.isReadyForReading = fileHandle->interests.isReadyForReading &&
                                                     FD_ISSET(fileHandle->fileDescriptor, readFileDescriptors);
                fileHandleEvents.isReadyForWriting = fileHandle->interests.isReadyForWriting &&
                                                     FD_ISSET(fileHandle->fileDescriptor, writeFileDescriptors);
                fileHandleEvents.hasErrorConditionPending = fileHandle->interests.hasErrorConditionPending &&
                                                            FD_ISSET(fileHandle->fileDescriptor, errorFileDescriptors);

                if (fileHandleEvents.isReadyForReading || fileHandleEvents.isReadyForWriting ||
                    fileHandleEvents.hasErrorConditionPending) {
                    fileHandle->callback((HAPPlatformFileHandleRef) fileHandle, fileHandleEvents, fileHandle->context);
                }
            }
        }
    }
}



void HAPPlatformRunSecondLoopCreate(void) {


    runSecondLoop.state = kHAPPlatformRunLoopState_Idle;

    // Issue memory barrier to ensure visibility of write to runSecondLoop.selfPipeFileDescriptor1 on signal handlers and
    // other threads.
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void HAPPlatformRunSecondLoopRelease(void) {

    runSecondLoop.state = kHAPPlatformRunLoopState_Idle;

    // Issue memory barrier to ensure visibility of write to runSecondLoop.selfPipeFileDescriptor1 on signal handlers and
    // other threads.
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void HAPPlatformRunSecondLoopRun(void) {
    HAPPrecondition(runSecondLoop.state == kHAPPlatformRunLoopState_Idle);

    HAPLogInfo(&logObject, "Entering run second loop.");
    runSecondLoop.state = kHAPPlatformRunLoopState_Running;
    do {
        fd_set readFileDescriptors;
        fd_set writeFileDescriptors;
        fd_set errorFileDescriptors;

        FD_ZERO(&readFileDescriptors);
        FD_ZERO(&writeFileDescriptors);
        FD_ZERO(&errorFileDescriptors);

        int maxFileDescriptor = -1;

        HAPPlatformFileHandle* fileHandle = runSecondLoop.fileHandles->nextFileHandle;
        while (fileHandle != runSecondLoop.fileHandles) {
            fileHandle->isAwaitingEvents = false;
            if (fileHandle->fileDescriptor != -1) {
                if (fileHandle->interests.isReadyForReading) {
                    HAPAssert(fileHandle->fileDescriptor >= 0);
                    HAPAssert(fileHandle->fileDescriptor < FD_SETSIZE);
                    FD_SET(fileHandle->fileDescriptor, &readFileDescriptors);
                    if (fileHandle->fileDescriptor > maxFileDescriptor) {
                        maxFileDescriptor = fileHandle->fileDescriptor;
                    }
                    fileHandle->isAwaitingEvents = true;
                }
                if (fileHandle->interests.isReadyForWriting) {
                    HAPAssert(fileHandle->fileDescriptor >= 0);
                    HAPAssert(fileHandle->fileDescriptor < FD_SETSIZE);
                    FD_SET(fileHandle->fileDescriptor, &writeFileDescriptors);
                    if (fileHandle->fileDescriptor > maxFileDescriptor) {
                        maxFileDescriptor = fileHandle->fileDescriptor;
                    }
                    fileHandle->isAwaitingEvents = true;
                }
                if (fileHandle->interests.hasErrorConditionPending) {
                    HAPAssert(fileHandle->fileDescriptor >= 0);
                    HAPAssert(fileHandle->fileDescriptor < FD_SETSIZE);
                    FD_SET(fileHandle->fileDescriptor, &errorFileDescriptors);
                    if (fileHandle->fileDescriptor > maxFileDescriptor) {
                        maxFileDescriptor = fileHandle->fileDescriptor;
                    }
                    fileHandle->isAwaitingEvents = true;
                }
            }
            fileHandle = fileHandle->nextFileHandle;
        }

        struct timeval timeoutValue;
        struct timeval* timeout = NULL;

        HAPTime nextDeadline = runSecondLoop.timers ? runSecondLoop.timers->deadline : 0;
        if (nextDeadline) {
            HAPTime now = HAPPlatformClockGetCurrent();
            HAPTime delta;
            if (nextDeadline > now) {
                delta = nextDeadline - now;
            } else {
                delta = 0;
            }
            HAPAssert(!timeout);
            timeout = &timeoutValue;
            timeout->tv_sec = (time_t)(delta / 1000);
            timeout->tv_usec = (suseconds_t)((delta % 1000) * 1000);
        }


        HAPAssert(maxFileDescriptor >= -1);
        HAPAssert(maxFileDescriptor < FD_SETSIZE);

        int e = select(
                maxFileDescriptor + 1, &readFileDescriptors, &writeFileDescriptors, &errorFileDescriptors, timeout);
        if (e == -1 && errno == EINTR) {
            continue;
        }
        if (e < 0) {
            int _errno = errno;
            HAPAssert(e == -1);
            HAPPlatformLogPOSIXError(
                    kHAPLogType_Error, "System call 'select' failed.", _errno, __func__, HAP_FILE, __LINE__);
            HAPFatalError();
        }


        ProcessSelectedFileHandles(&readFileDescriptors, &writeFileDescriptors, &errorFileDescriptors);
    } while (runSecondLoop.state == kHAPPlatformRunLoopState_Running);

    HAPLogInfo(&logObject, "Exiting run loop.");
    HAPAssert(runSecondLoop.state == kHAPPlatformRunLoopState_Stopping);
    runSecondLoop.state = kHAPPlatformRunLoopState_Idle;
}

void HAPPlatformRunSecondLoopStop(void) {
    if (runSecondLoop.state == kHAPPlatformRunLoopState_Running) {
        runSecondLoop.state = kHAPPlatformRunLoopState_Stopping;
    }
}

HAPError HAPPlatformRunSecondLoopScheduleCallback(
        HAPPlatformRunLoopCallback callback,
        void* _Nullable const context,
        size_t contextSize) {
    HAPPrecondition(callback);
    HAPPrecondition(!contextSize || context);

    if (contextSize > UINT8_MAX) {
        HAPLogError(&logObject, "Contexts larger than UINT8_MAX are not supported.");
        return kHAPError_OutOfResources;
    }
    if (contextSize + 1 + sizeof callback > PIPE_BUF) {
        HAPLogError(&logObject, "Context too large (PIPE_BUF).");
        return kHAPError_OutOfResources;
    }

    // Issue memory barrier to ensure visibility of writes on signal handlers and other threads.
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // Serialize event context.
    // Format: Callback pointer followed by 1 byte context size and context data.
    // Context is copied to offset 0 when invoking the callback to ensure proper alignment.
    uint8_t bytes[sizeof callback + 1 + UINT8_MAX];
    size_t numBytes = 0;
    HAPRawBufferCopyBytes(&bytes[numBytes], &callback, sizeof callback);
    numBytes += sizeof callback;
    bytes[numBytes] = (uint8_t) contextSize;
    numBytes++;
    if (context) {
        HAPRawBufferCopyBytes(&bytes[numBytes], context, contextSize);
        numBytes += contextSize;
    }
    HAPAssert(numBytes <= sizeof bytes);

    ssize_t n;
    do {
        n = write(runSecondLoop.selfPipeFileDescriptor1, bytes, numBytes);
    } while (n == -1 && errno == EINTR);
    if (n == -1) {
        HAPLogError(&logObject, "write failed: %ld.", (long) n);
        return kHAPError_Unknown;
    }

    return kHAPError_None;
}
