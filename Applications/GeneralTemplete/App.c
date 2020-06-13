// Copyright (c) 2015-2019 The HomeKit ADK Contributors
//
// Licensed under the Apache License, Version 2.0 (the â€œLicenseâ€?;
// you may not use this file except in compliance with the License.
// See [CONTRIBUTORS.md] for the list of HomeKit ADK project authors.

// An example that implements the light bulb HomeKit profile. It can serve as a basic implementation for
// any platform. The accessory logic implementation is reduced to internal state updates and log output.
//
// This implementation is platform-independent.
//
// The code consists of multiple parts:
//
//   1. The definition of the accessory configuration and its internal state.
//
//   2. Helper functions to load and save the state of the accessory.
//
//   3. The definitions for the HomeKit attribute database.
//
//   4. The callbacks that implement the actual behavior of the accessory, in this
//      case here they merely access the global accessory state variable and write
//      to the log to make the behavior easily observable.
//
//   5. The initialization of the accessory state.
//
//   6. Callbacks that notify the server in case their associated value has changed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "HAP.h"

#include "App.h"
#include "DB.h"

#include "HAPPlatform.h"
#include "HAPPlatformFileHandle.h"

#include "HAP+Internal.h"

#include "CoapAgent.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Domain used in the key value store for application data.
 *
 * Purged: On factory reset.
 */
#define kAppKeyValueStoreDomain_Configuration ((HAPPlatformKeyValueStoreDomain) 0x00)

/**
 * Key used in the key value store to store the configuration state.
 *
 * Purged: On factory reset.
 */
#define kAppKeyValueStoreKey_Configuration_State ((HAPPlatformKeyValueStoreKey) 0x00)


 /**
* Key used in the key value store to store the base information.
 *
 * Purged: On factory reset.
 */
#define kAppKeyValueStoreKey_Configuration_Base ((HAPPlatformKeyValueStoreKey) 0x01)


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * HomeKit accessory basic information..
 */
struct HAPAccessoryBase {
    /**
     * Accessory instance ID.
     *
     * For regular accessories (Bluetooth LE / IP):
     * - Must be 1.
     *
     * For bridged accessories:
     * - Must be unique for the bridged accessory and not change across firmware updates or power cycles.
     */
    uint64_t aid;

    /**
     * Category information for the accessory.
     *
     * For regular accessories (Bluetooth LE / IP):
     * - Must match the functionality of the accessory's primary service.
     *
     * For bridged accessories:
     * - Must be kHAPAccessoryCategory_BridgedAccessory.
     */
    HAPAccessoryCategory category;

    /**
     * The display name of the accessory.
     *
     * - Maximum length 64 (excluding NULL-terminator).
     * - ':' and ';' characters should not be used for accessories that support Bluetooth LE.
     * - The user may adjust the name on the controller. Such changes are local only and won't sync to the accessory.
     */
    char name[65];

    /**
     * The manufacturer of the accessory.
     *
     * - Maximum length 64 (excluding NULL-terminator).
     */
    char manufacturer[65];

    /**
     * The model name of the accessory.
     *
     * - Minimum length 1 (excluding NULL-terminator).
     * - Maximum length 64 (excluding NULL-terminator).
     */
    char model[65];

    /**
     * The serial number of the accessory.
     *
     * - Minimum length 2 (excluding NULL-terminator).
     * - Maximum length 64 (excluding NULL-terminator).
     */
    char serialNumber[65];

    /**
     * The firmware version of the accessory.
     *
     * - x[.y[.z]] (e.g. "100.1.1")
     * - Each number must not be greater than UINT32_MAX.
     * - Maximum length 64 (excluding NULL-terminator).
     */
    char firmwareVersion[65];

    /**
     * The hardware version of the accessory.
     *
     * - x[.y[.z]] (e.g. "100.1.1")
     * - Maximum length 64 (excluding NULL-terminator).
     */
    char hardwareVersion[65];

};

/**
 * Global accessory configuration.
 */
typedef struct {
    struct {
        bool lightBulbOn;
    } state;
    HAPAccessoryServerRef* server;
    HAPPlatformKeyValueStoreRef keyValueStore;
	struct HAPAccessoryBase baseInfo;

} AccessoryConfiguration;

static AccessoryConfiguration accessoryConfiguration;
COAP_Session coap_session;
static HAPPlatformFileHandleRef coapAgentFileHandle;
//----------------------------------------------------------------------------------------------------------------------

/**
 * HomeKit accessory that provides the Light Bulb service.
 *
 * Note: Not constant to enable BCT Manual Name Change.
 */
static HAPAccessory accessory = { .aid = 1,
                                  .category = kHAPAccessoryCategory_Lighting,
                                  .name = "Acme Light Bulb",
                                  .manufacturer = "Acme",
                                  .model = "LightBulb1,1",
                                  .serialNumber = "099DB48E9E28",
                                  .firmwareVersion = "1",
                                  .hardwareVersion = "1",
                                  .services = (const HAPService* const[]) { &accessoryInformationService,
                                                                            &hapProtocolInformationService,
                                                                            &pairingService,
                                                                            &lightBulbService,
                                                                            NULL },
                                  .callbacks = { .identify = IdentifyAccessory } };

//----------------------------------------------------------------------------------------------------------------------

//----------------------------------------------------------------------------------------------------------------------
HAP_RESULT_USE_CHECK
static size_t try_read_uint64(const char* buffer, size_t length, uint64_t* r) {
    size_t k;
    HAPAssert(buffer != NULL);
    HAPAssert(r != NULL);
    *r = 0;
    k = 0;
    HAPAssert(k <= length);
    while ((k < length) && ('0' <= buffer[k]) && (buffer[k] <= '9') &&
           (*r <= (UINT64_MAX - (uint64_t)(buffer[k] - '0')) / 10)) {
        *r = *r * 10 + (uint64_t)(buffer[k] - '0');
        k++;
    }
    HAPAssert(
            (k == length) || ((k < length) && ((buffer[k] < '0') || (buffer[k] > '9') ||
                                               (*r > (UINT64_MAX - (uint64_t)(buffer[k] - '0')) / 10))));
    return k;
}

HAP_RESULT_USE_CHECK
static size_t try_read_number(
        struct util_json_reader* json_reader,
        char* bytes,
        size_t numBytes,
        uint64_t* destNum,
        HAPError* err) 
{

    size_t i, k, n;
    uint64_t x;


    HAPAssert(json_reader != NULL);
    HAPAssert(bytes != NULL);
    HAPAssert(destNum != NULL);
    HAPAssert(err != NULL);

	k = 0;
	*err = kHAPError_None;
    k += util_json_reader_read(json_reader, &bytes[k], numBytes - k);
    if (json_reader->state == util_JSON_READER_STATE_BEGINNING_NUMBER) {
        HAPAssert(k <= numBytes);
        i = k;
        k += util_json_reader_read(json_reader, &bytes[k], numBytes - k);
        if (json_reader->state != util_JSON_READER_STATE_COMPLETED_NUMBER) {
            *err = kHAPError_InvalidData;
			goto EXIT;
        }
        HAPAssert(i <= k);
        HAPAssert(k <= numBytes);
        n = try_read_uint64(&bytes[i], k - i, &x);
        if (n == k - i) {
            *destNum = x;
        } else {
            HAPLogBuffer(&kHAPLog_Default, &bytes[i], k - i, "Invalid number.");
	        *err = kHAPError_InvalidData;
			goto EXIT;
        }
    } else {
        *err = kHAPError_InvalidData;
		goto EXIT;
    }

EXIT:

	return k;

}

HAP_RESULT_USE_CHECK
static size_t try_read_string(
        struct util_json_reader* json_reader,
        char* bytes,
        size_t numBytes,
        char* destStr,
        HAPError* err) 
{

    size_t i, k;


    HAPAssert(json_reader != NULL);
    HAPAssert(bytes != NULL);
    HAPAssert(destStr != NULL);
    HAPAssert(err != NULL);

	k = 0;
	*err = kHAPError_None;
    k += util_json_reader_read(json_reader, &bytes[k], numBytes - k);
    if (json_reader->state == util_JSON_READER_STATE_BEGINNING_STRING) {
        HAPAssert(k <= numBytes);
        i = k;
        k += util_json_reader_read(json_reader, &bytes[k], numBytes - k);
        if (json_reader->state != util_JSON_READER_STATE_COMPLETED_STRING) {
            *err = kHAPError_InvalidData;
			goto EXIT;
        }
        HAPAssert(i <= k);
        HAPAssert(k <= numBytes);
		/* Do not copy '"' char */
        HAPRawBufferCopyBytes(destStr,&bytes[i + 1],k - i - 2);
    } else {
        *err = kHAPError_InvalidData;
		goto EXIT;
    }

EXIT:

	return k;

}

HAPError ParseBaseInfoFromJsonFormat(
		char* bytes,
        size_t numBytes,
        struct HAPAccessoryBase* baseInfo )
{
    size_t i, j, k;
    struct util_json_reader json_reader;

	uint32_t hasAID = 0;
	uint32_t hasCategory = 0;
	uint32_t hasName = 0;
	uint32_t hasManufacturer = 0;
	uint32_t hasModel = 0;
	uint32_t hasSerialNumber = 0;
	uint32_t hasFirmwareVersion = 0;
	uint32_t hasHardwareVersion = 0;

    HAPError err;


    util_json_reader_init(&json_reader);
    k = util_json_reader_read(&json_reader, bytes, numBytes);
    if (json_reader.state != util_JSON_READER_STATE_BEGINNING_OBJECT) {
        return kHAPError_InvalidData;
    }

    HAPAssert(k <= numBytes);
	
    do {
        k += util_json_reader_read(&json_reader, &bytes[k], numBytes - k);
        if (json_reader.state != util_JSON_READER_STATE_BEGINNING_STRING) {
            return kHAPError_InvalidData;
        }
        HAPAssert(k <= numBytes);
        i = k;
        k += util_json_reader_read(&json_reader, &bytes[k], numBytes - k);
        if (json_reader.state != util_JSON_READER_STATE_COMPLETED_STRING) {
            return kHAPError_InvalidData;
        }
        HAPAssert(k <= numBytes);
        j = k;
        k += util_json_reader_read(&json_reader, &bytes[k], numBytes - k);
        if (json_reader.state != util_JSON_READER_STATE_AFTER_NAME_SEPARATOR) {
            return kHAPError_InvalidData;
        }
        HAPAssert(i <= j);
        HAPAssert(j <= k);
        HAPAssert(k <= numBytes);
		
		
		if ((j - i == 5) && HAPRawBufferAreEqual(&bytes[i], "\"aid\"", 5)) {
            if (hasAID) {
                HAPLog(&kHAPLog_Default, "Multiple AID entries detected.");
                return kHAPError_InvalidData;
            }
			k += try_read_number(&json_reader, &bytes[k], numBytes - k, &baseInfo->aid, &err);
			if(err != kHAPError_None){
                HAPLogError(&kHAPLog_Default, "get item err @ %s:%d.",__FILE__,__LINE__);
				return err;
			}
			hasAID = true;
        } 		
		else if ((j - i == 10) && HAPRawBufferAreEqual(&bytes[i], "\"category\"", 10)) {
		    uint64_t tmp;

            if (hasCategory) {
                HAPLog(&kHAPLog_Default, "Multiple AID entries detected.");
                return kHAPError_InvalidData;
            }
			k += try_read_number(&json_reader, &bytes[k], numBytes - k, &tmp, &err);
			if(err != kHAPError_None){
                HAPLogError(&kHAPLog_Default, "get item err @ %s:%d.",__FILE__,__LINE__);
				return err;
			}
			baseInfo->category = (HAPAccessoryCategory)tmp;
			hasCategory = true;
        } 
		else if ((j - i == 6) && HAPRawBufferAreEqual(&bytes[i], "\"name\"", 6)) {
            if (hasName) {
                HAPLog(&kHAPLog_Default, "Multiple name entries detected.");
                return kHAPError_InvalidData;
            }
			k += try_read_string(&json_reader, &bytes[k], numBytes - k, baseInfo->name, &err);
			if(err != kHAPError_None){
                HAPLogError(&kHAPLog_Default, "get item err @ %s:%d.",__FILE__,__LINE__);
				return err;
			}
			hasName = true;
			
        } 
		else if ((j - i == 14) && HAPRawBufferAreEqual(&bytes[i], "\"manufacturer\"", 14)) {
            if (hasManufacturer) {
                HAPLog(&kHAPLog_Default, "Multiple AID entries detected.");
                return kHAPError_InvalidData;
            }
			k += try_read_string(&json_reader, &bytes[k], numBytes - k, baseInfo->manufacturer, &err);
			if(err != kHAPError_None){
                HAPLogError(&kHAPLog_Default, "get item err @ %s:%d.",__FILE__,__LINE__);
				return err;
			}
			hasManufacturer = true;

        } 
		else if ((j - i == 7) && HAPRawBufferAreEqual(&bytes[i], "\"model\"", 7)) {
            if (hasModel) {
                HAPLog(&kHAPLog_Default, "Multiple model entries detected.");
                return kHAPError_InvalidData;
            }
			k += try_read_string(&json_reader, &bytes[k], numBytes - k, baseInfo->model, &err);
			if(err != kHAPError_None){
                HAPLogError(&kHAPLog_Default, "get item err @ %s:%d.",__FILE__,__LINE__);
				return err;
			}
			hasModel = true;
        } 
		else if ((j - i == 14) && HAPRawBufferAreEqual(&bytes[i], "\"serialNumber\"", 14)) {
            if (hasSerialNumber) {
                HAPLog(&kHAPLog_Default, "Multiple serialNumber entries detected.");
                return kHAPError_InvalidData;
            }
			k += try_read_string(&json_reader, &bytes[k], numBytes - k, baseInfo->serialNumber, &err);
			if(err != kHAPError_None){
                HAPLogError(&kHAPLog_Default, "get item err @ %s:%d.",__FILE__,__LINE__);
				return err;
			}
			hasSerialNumber = true;
        } 
		else if ((j - i == 17) && HAPRawBufferAreEqual(&bytes[i], "\"firmwareVersion\"", 17)) {
            if (hasFirmwareVersion) {
                HAPLog(&kHAPLog_Default, "Multiple firmwareVersion entries detected.");
                return kHAPError_InvalidData;
            }
			k += try_read_string(&json_reader, &bytes[k], numBytes - k, baseInfo->firmwareVersion, &err);
			if(err != kHAPError_None){
                HAPLogError(&kHAPLog_Default, "get item err @ %s:%d.",__FILE__,__LINE__);
				return err;
			}
			hasSerialNumber = true;
        } 
		else if ((j - i == 17) && HAPRawBufferAreEqual(&bytes[i], "\"hardwareVersion\"", 17)) {
            if (hasHardwareVersion) {
                HAPLog(&kHAPLog_Default, "Multiple hardwareVersion entries detected.");
                return kHAPError_InvalidData;
            }
			k += try_read_string(&json_reader, &bytes[k], numBytes - k, baseInfo->hardwareVersion, &err);
			if(err != kHAPError_None){
                HAPLogError(&kHAPLog_Default, "get item err @ %s:%d.",__FILE__,__LINE__);
				return err;
			}
			hasHardwareVersion = true;
        } 
		else {
            size_t skippedBytes;
            err = HAPJSONUtilsSkipValue(&json_reader, &bytes[k], numBytes - k, &skippedBytes);
            if (err) {
                HAPAssert((err == kHAPError_InvalidData) || (err == kHAPError_OutOfResources));
                return kHAPError_InvalidData;
            }
            k += skippedBytes;
        }
        HAPAssert(k <= numBytes);
        k += util_json_reader_read(&json_reader, &bytes[k], numBytes - k);
    } while ((k < numBytes) && (json_reader.state == util_JSON_READER_STATE_AFTER_VALUE_SEPARATOR));
    HAPAssert(
            (k == numBytes) || ((k < numBytes) && (json_reader.state != util_JSON_READER_STATE_AFTER_VALUE_SEPARATOR)));


	return kHAPError_None;
}

/**
 * Load the accessory base info from persistent memory.
 */
static void LoadAccessoryBaseInfo(void) {
    HAPPrecondition(accessoryConfiguration.keyValueStore);
	struct HAPAccessoryBase* baseInfo = NULL;
    HAPError err;

    // Load persistent state if available
    bool found;
    size_t numBytes;

	baseInfo = calloc(1, sizeof(accessoryConfiguration.baseInfo));
	if(baseInfo == NULL){
		goto DONE;
	}

    err = HAPPlatformKeyValueStoreGet(
            accessoryConfiguration.keyValueStore,
            kAppKeyValueStoreDomain_Configuration,
            kAppKeyValueStoreKey_Configuration_Base,
            baseInfo,
            sizeof accessoryConfiguration.baseInfo,
            &numBytes,
            &found);

    if (err) {
        HAPAssert(err == kHAPError_Unknown);
        HAPFatalError();
    }
    if (!found ) {
        HAPLogError(&kHAPLog_Default, "No app baseInfo found in key-value store. Using default.");
        goto DONE;
    }

    // Verify basic info
    HAPLogBufferDebug(
            &kHAPLog_Default,
            baseInfo,
            sizeof accessoryConfiguration.baseInfo,
            "Accessory base info");

	err = ParseBaseInfoFromJsonFormat(
		(char*)baseInfo,
		strlen((char*)baseInfo),
		&accessoryConfiguration.baseInfo);
	
    HAPLogInfo(&kHAPLog_Default, "baseInfo.aid: %ld", accessoryConfiguration.baseInfo.aid);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.category: %d", accessoryConfiguration.baseInfo.category);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.name: %s", accessoryConfiguration.baseInfo.name);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.manufacturer: %s", accessoryConfiguration.baseInfo.manufacturer);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.model: %s", accessoryConfiguration.baseInfo.model);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.serialNumber: %s", accessoryConfiguration.baseInfo.serialNumber);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.firmwareVersion: %s", accessoryConfiguration.baseInfo.firmwareVersion);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.hardwareVersion: %s", accessoryConfiguration.baseInfo.hardwareVersion);
	
    if (err) {
        HAPAssert(err == kHAPError_Unknown);
        HAPFatalError();
		goto DONE;
    }

	accessory.aid				= accessoryConfiguration.baseInfo.aid;
	accessory.category 			= accessoryConfiguration.baseInfo.category;
	accessory.name				= accessoryConfiguration.baseInfo.name;
	accessory.manufacturer  	= accessoryConfiguration.baseInfo.manufacturer;
	accessory.model				= accessoryConfiguration.baseInfo.model;
	accessory.serialNumber		= accessoryConfiguration.baseInfo.serialNumber;
	accessory.firmwareVersion 	= accessoryConfiguration.baseInfo.firmwareVersion;
	accessory.hardwareVersion 	= accessoryConfiguration.baseInfo.hardwareVersion;



DONE:
	if(baseInfo != NULL){
		free(baseInfo);
	}
	return;	
}

/**
 * Load the accessory state from persistent memory.
 */
static void LoadAccessoryState(void) {
    HAPPrecondition(accessoryConfiguration.keyValueStore);

    HAPError err;

    // Load persistent state if available
    bool found;
    size_t numBytes;

    err = HAPPlatformKeyValueStoreGet(
            accessoryConfiguration.keyValueStore,
            kAppKeyValueStoreDomain_Configuration,
            kAppKeyValueStoreKey_Configuration_State,
            &accessoryConfiguration.state,
            sizeof accessoryConfiguration.state,
            &numBytes,
            &found);

    if (err) {
        HAPAssert(err == kHAPError_Unknown);
        HAPFatalError();
    }
    if (!found || numBytes != sizeof accessoryConfiguration.state) {
        if (found) {
            HAPLogError(&kHAPLog_Default, "Unexpected app state found in key-value store. Resetting to default.");
        }
        HAPRawBufferZero(&accessoryConfiguration.state, sizeof accessoryConfiguration.state);
    }
}

/**
 * Save the accessory state to persistent memory.
 */
static void SaveAccessoryState(void) {
    HAPPrecondition(accessoryConfiguration.keyValueStore);

    HAPError err;
    err = HAPPlatformKeyValueStoreSet(
            accessoryConfiguration.keyValueStore,
            kAppKeyValueStoreDomain_Configuration,
            kAppKeyValueStoreKey_Configuration_State,
            &accessoryConfiguration.state,
            sizeof accessoryConfiguration.state);
    if (err) {
        HAPAssert(err == kHAPError_Unknown);
        HAPFatalError();
    }
}


//----------------------------------------------------------------------------------------------------------------------

HAP_RESULT_USE_CHECK
HAPError IdentifyAccessory(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPAccessoryIdentifyRequest* request HAP_UNUSED,
        void* _Nullable context HAP_UNUSED) {
    HAPLogInfo(&kHAPLog_Default, "%s", __func__);
    return kHAPError_None;
}

HAP_RESULT_USE_CHECK
HAPError HandleLightBulbOnRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPBoolCharacteristicReadRequest* request HAP_UNUSED,
        bool* value,
        void* _Nullable context HAP_UNUSED) {
    *value = accessoryConfiguration.state.lightBulbOn;
    HAPLogInfo(&kHAPLog_Default, "%s: %s", __func__, *value ? "true" : "false");

    return kHAPError_None;
}

HAP_RESULT_USE_CHECK
HAPError HandleLightBulbOnWrite(
        HAPAccessoryServerRef* server,
        const HAPBoolCharacteristicWriteRequest* request,
        bool value,
        void* _Nullable context HAP_UNUSED) {
    HAPLogInfo(&kHAPLog_Default, "%s: %s", __func__, value ? "true" : "false");
    if (accessoryConfiguration.state.lightBulbOn != value) {
        accessoryConfiguration.state.lightBulbOn = value;

        SaveAccessoryState();

        HAPAccessoryServerRaiseEvent(server, request->characteristic, request->service, request->accessory);
    }

    return kHAPError_None;
}

//----------------------------------------------------------------------------------------------------------------------

void AccessoryNotification(
        const HAPAccessory* accessory,
        const HAPService* service,
        const HAPCharacteristic* characteristic,
        void* ctx) {
    HAPLogInfo(&kHAPLog_Default, "Accessory Notification");

    HAPAccessoryServerRaiseEvent(accessoryConfiguration.server, characteristic, service, accessory);
}

void AppCreate(HAPAccessoryServerRef* server, HAPPlatformKeyValueStoreRef keyValueStore) {
    HAPPrecondition(server);
    HAPPrecondition(keyValueStore);

    HAPLogInfo(&kHAPLog_Default, "%s", __func__);

    HAPRawBufferZero(&accessoryConfiguration, sizeof accessoryConfiguration);
    accessoryConfiguration.server = server;
    accessoryConfiguration.keyValueStore = keyValueStore;

	LoadAccessoryBaseInfo();
	
    LoadAccessoryState();
}

void AppRelease(void) {
}

void AppAccessoryServerStart(void) {
    HAPAccessoryServerStart(accessoryConfiguration.server, &accessory);
}

//----------------------------------------------------------------------------------------------------------------------

void AccessoryServerHandleUpdatedState(HAPAccessoryServerRef* server, void* _Nullable context) {
    HAPPrecondition(server);
    HAPPrecondition(!context);

    switch (HAPAccessoryServerGetState(server)) {
        case kHAPAccessoryServerState_Idle: {
            HAPLogInfo(&kHAPLog_Default, "Accessory Server State did update: Idle.");
            return;
        }
        case kHAPAccessoryServerState_Running: {
            HAPLogInfo(&kHAPLog_Default, "Accessory Server State did update: Running.");
            return;
        }
        case kHAPAccessoryServerState_Stopping: {
            HAPLogInfo(&kHAPLog_Default, "Accessory Server State did update: Stopping.");
            return;
        }
    }
    HAPFatalError();
}

const HAPAccessory* AppGetAccessoryInfo() {
    return &accessory;
}


void CoapAgentHandleCallback(
        HAPPlatformFileHandleRef fileHandle,
        HAPPlatformFileHandleEvent fileHandleEvents,
        void* _Nullable context) {
    HAPAssert(fileHandle);
    HAPAssert(fileHandleEvents.isReadyForReading);
    HAPAssert(context);



	CoapAgentRecv(((COAP_Session *)context)->sockFd);

}


void AppInitialize(
        HAPAccessoryServerOptions* hapAccessoryServerOptions,
        HAPPlatform* hapPlatform,
        HAPAccessoryServerCallbacks* hapAccessoryServerCallbacks) {
	if(CoapAgentCreate("/tmp/coapClient",&coap_session.sockFd)){
        HAPLogError(&kHAPLog_Default, "%s: CoapAgentCreate failed.", __func__);
	}

	HAPError err;


    err = HAPPlatformFileHandleRegister(
            &coapAgentFileHandle,
            coap_session.sockFd,
            (HAPPlatformFileHandleEvent) {
                    .isReadyForReading = true, .isReadyForWriting = false, .hasErrorConditionPending = false },
            CoapAgentHandleCallback,
            &coap_session);
    if (err) {
        HAPLogError(&kHAPLog_Default, "%s: HAPPlatformFileHandleRegister failed: %u.", __func__, err);
        HAPFatalError();
    }
}

void AppDeinitialize() {
    HAPPlatformFileHandleDeregister(coapAgentFileHandle);
}
