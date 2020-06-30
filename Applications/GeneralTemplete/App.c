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
#include <unistd.h>
#include <stdint.h>

#include "HAP.h"

#include "App.h"
#include "DB.h"

#include "HAPPlatform.h"
#include "HAPPlatformFileHandle.h"
#include "HAPPlatformThread.h"
#include "HAPPlatformSync.h"

#include "HAPBase.h"
#include "HAP+Internal.h"
#include "HAPPlatformRunSecondLoop.h"
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


#define MAX_SERVICES	8
#define delay_ms(a)    usleep(a*1000)


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
typedef struct {
    /**
     * The type of the service.
     *
     * - Maximum length 31 (excluding NULL-terminator).
     */
	char 		type[32];

    /**
     * The number of the service.
     *
     * - Maximum 8.
     */
	uint32_t 	number;

}AccessorySerivce;

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

	AccessorySerivce services[MAX_SERVICES];

};

/**
 * Global accessory configuration.
 */
typedef struct {
    struct {
        bool On[8];
    } state;
    HAPAccessoryServerRef* server;
    HAPPlatformKeyValueStoreRef keyValueStore;
	struct HAPAccessoryBase baseInfo;

} AccessoryConfiguration;

static AccessoryConfiguration accessoryConfiguration;
COAP_Session coap_session;


//----------------------------------------------------------------------------------------------------------------------
void AccessoryCoapAgentCreate(void);
void CoapAgentHandleCallback(
        HAPPlatformFileHandleRef fileHandle,
        HAPPlatformFileHandleEvent fileHandleEvents,
        void* _Nullable context);
HAP_RESULT_USE_CHECK
HAPError HandleTemperatureRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPFloatCharacteristicReadRequest* request HAP_UNUSED,
        float* value,
        void* _Nullable context HAP_UNUSED);

HAPError HandleHumidityRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPFloatCharacteristicReadRequest* request HAP_UNUSED,
        float* value,
        void* _Nullable context HAP_UNUSED);

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
                                  .services = (const HAPService* []) { &accessoryInformationService,
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


HAP_RESULT_USE_CHECK
static size_t try_read_float(
        struct util_json_reader* json_reader,
        char* bytes,
        size_t numBytes,
        float* value,
        HAPError* err) 
{

    size_t i, k;
	char * buffer = NULL;

    HAPAssert(json_reader != NULL);
    HAPAssert(bytes != NULL);
    HAPAssert(value != NULL);
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
		buffer = calloc(1, k - i + 1);
		HAPAssert(buffer);
        HAPRawBufferCopyBytes(buffer, &bytes[i], k - i);
		
	    HAPLogBufferDebug(&kHAPLog_Default, buffer, k - i,"%s",__func__);

		*err =  HAPFloatFromString(buffer, value);
		
    } else {
        *err = kHAPError_InvalidData;
		goto EXIT;
    }

EXIT:
	if(buffer != NULL){
		free(buffer);
	}
	return k;

}

HAP_RESULT_USE_CHECK
static size_t read_service(
        struct util_json_reader* json_reader,
        char* bytes,
        size_t numBytes,
        AccessorySerivce* service,
        HAPError* err) 
{

    size_t i, j, k;
	uint32_t hasType = 0;
	uint32_t hasNumber = 0;
	
    HAPAssert(json_reader != NULL);
    HAPAssert(bytes != NULL);
    HAPAssert(service != NULL);
    HAPAssert(err != NULL);

	*err = kHAPError_None;

    k = util_json_reader_read(json_reader, bytes, numBytes);
    if (json_reader->state != util_JSON_READER_STATE_BEGINNING_OBJECT) {
        *err = kHAPError_InvalidData;
		goto EXIT;
    }

    do {
        k += util_json_reader_read(json_reader, &bytes[k], numBytes - k);
        if (json_reader->state != util_JSON_READER_STATE_BEGINNING_STRING) {
            *err = kHAPError_InvalidData;
			goto EXIT;
        }
        HAPAssert(k <= numBytes);
        i = k;
        k += util_json_reader_read(json_reader, &bytes[k], numBytes - k);
        if (json_reader->state != util_JSON_READER_STATE_COMPLETED_STRING) {
            *err = kHAPError_InvalidData;
			goto EXIT;
        }
        HAPAssert(k <= numBytes);
        j = k;
        k += util_json_reader_read(json_reader, &bytes[k], numBytes - k);
        if (json_reader->state != util_JSON_READER_STATE_AFTER_NAME_SEPARATOR) {
            *err = kHAPError_InvalidData;
			goto EXIT;
		}
        HAPAssert(i <= j);
        HAPAssert(j <= k);
        HAPAssert(k <= numBytes);
		
		
		if ((j - i == 8) && HAPRawBufferAreEqual(&bytes[i], "\"number\"", 8)) {
	        uint64_t destNum;

            if (hasNumber) {
                HAPLog(&kHAPLog_Default, "Multiple number entries detected.");
                *err = kHAPError_InvalidData;
				goto EXIT;
            }
			k += try_read_number(json_reader, &bytes[k], numBytes - k, &destNum, err);
			if(*err != kHAPError_None){
                HAPLogError(&kHAPLog_Default, "get item err @ %s:%d.",__FILE__,__LINE__);
				goto EXIT;
			}
			service->number = (uint32_t)destNum;
			hasNumber = true;
        } 		
		else if ((j - i == 6) && HAPRawBufferAreEqual(&bytes[i], "\"type\"", 6)) {
            if (hasType) {
                HAPLog(&kHAPLog_Default, "Multiple type entries detected.");
                *err = kHAPError_InvalidData;
				goto EXIT;

            }
			k += try_read_string(json_reader, &bytes[k], numBytes - k, service->type, err);
			if(*err != kHAPError_None){
                HAPLogError(&kHAPLog_Default, "get item err @ %s:%d.",__FILE__,__LINE__);
				goto EXIT;
			}
			hasType = true;
			
        } 
		else {
            size_t skippedBytes;
            *err = HAPJSONUtilsSkipValue(json_reader, &bytes[k], numBytes - k, &skippedBytes);
            if (*err) {
                HAPAssert((*err == kHAPError_InvalidData) || (*err == kHAPError_OutOfResources));
                goto EXIT;
            }
            k += skippedBytes;
        }
        HAPAssert(k <= numBytes);
        k += util_json_reader_read(json_reader, &bytes[k], numBytes - k);
    } while ((k < numBytes) && (json_reader->state == util_JSON_READER_STATE_AFTER_VALUE_SEPARATOR));

	if(json_reader->state != util_JSON_READER_STATE_COMPLETED_OBJECT){
        *err = kHAPError_InvalidData;
		goto EXIT;
	}

EXIT:

	return k;

}

HAPError ParseBaseInfoFromJsonFormat(
		char* bytes,
        size_t numBytes,
        struct HAPAccessoryBase* baseInfo ,
        size_t maxServices,
        size_t *numServices)
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
	uint32_t hasServices = 0;

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
			hasFirmwareVersion = true;
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
		else if ((j - i == 10) && HAPRawBufferAreEqual(&bytes[i], "\"services\"", 10)) {
            if (hasServices) {
                HAPLog(&kHAPLog_Default, "Multiple hardwareVersion entries detected.");
                return kHAPError_InvalidData;
            }
            k += util_json_reader_read(&json_reader, &bytes[k], numBytes - k);
            if (json_reader.state != util_JSON_READER_STATE_BEGINNING_ARRAY) {
                return kHAPError_InvalidData;
            }
            HAPAssert(k <= numBytes);
			*numServices = 0;
            do {
				if(*numServices >= maxServices){
	                HAPLogError(&kHAPLog_Default, " Service number %ld is out of range %ld.",
								*numServices, maxServices);
	                return kHAPError_InvalidData;
				}
                k += read_service(
                        &json_reader, &bytes[k], numBytes - k,
                        &baseInfo->services[*numServices], &err);
                if (err) {
                    return err;
                }
				*numServices += 1;
                HAPAssert(k <= numBytes);
                k += util_json_reader_read(&json_reader, &bytes[k], numBytes - k);
            } while ((k < numBytes) && (json_reader.state == util_JSON_READER_STATE_AFTER_VALUE_SEPARATOR));
            HAPAssert(
                    (k == numBytes) ||
                    ((k < numBytes) && (json_reader.state != util_JSON_READER_STATE_AFTER_VALUE_SEPARATOR)));
            if (json_reader.state != util_JSON_READER_STATE_COMPLETED_ARRAY) {
                return kHAPError_InvalidData;
            }
			hasServices = true;
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




HAPError GetVariableFromHttpResponse(
		char* bytes,
        size_t numBytes,
        char* name,
        size_t nameLen,
        float* value)
{
    size_t i, j, k;
    struct util_json_reader json_reader;

	uint32_t hasVariable = 0;

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
		
		
		if ((j - i == nameLen) && HAPRawBufferAreEqual(&bytes[i], name, nameLen)) {
            if (hasVariable) {
                HAPLog(&kHAPLog_Default, "Multiple %s entries detected.",name);
                return kHAPError_InvalidData;
            }
			k += try_read_float(&json_reader, &bytes[k], numBytes - k, value, &err);
			if(err != kHAPError_None){
                HAPLogError(&kHAPLog_Default, "get item err @ %s:%d.",__FILE__,__LINE__);
				return err;
			}
			hasVariable = true;
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




HAPError handle_http_response(COAPUnixDomainSessionDescriptor* session,
		        char* name,
		        size_t nameLen,
		        float* value)
{
	HAPError err = kHAPError_Unknown;


	HAPPrecondition(session);

    size_t content_length;
    HAPAssert(session->inboundBuffer.data);
    HAPAssert(session->inboundBuffer.position <= session->inboundBuffer.limit);
    HAPAssert(session->inboundBuffer.limit <= session->inboundBuffer.capacity);
    HAPAssert(session->httpReaderPosition <= session->inboundBuffer.position);
    HAPAssert(session->httpReader.state == util_HTTP_READER_STATE_DONE);
    HAPAssert(!session->httpParserError);


    if (session->httpContentLength.isDefined) {
        content_length = session->httpContentLength.value;
    } else {
        content_length = 0;
    }
    if ((content_length <= session->inboundBuffer.position) &&
        (session->httpReaderPosition <= session->inboundBuffer.position - content_length)) {
        HAPLogBufferDebug(
                &kHAPLog_Default,
                session->inboundBuffer.data + session->httpReaderPosition ,
                content_length,
                "session:%p:>",
                (const void*) session);

		err = GetVariableFromHttpResponse(
				session->inboundBuffer.data + session->httpReaderPosition,
		        content_length,
		        name,
		        nameLen,
		        value);
				
    }

	return err;
}



HAPError SwitchServiceAdd(uint64_t *iid, uint64_t localId, AccessorySerivce* input, HAPService** out)
{
	HAPService * service = calloc(1, sizeof(HAPService));
	HAPDataCharacteristic* signature = calloc(1, sizeof(HAPDataCharacteristic));
	HAPStringCharacteristic* name = calloc(1, sizeof(HAPStringCharacteristic)); 
	HAPBoolCharacteristic *switch_on = calloc(1, sizeof(HAPBoolCharacteristic));

	HAPAssert(iid);
	HAPAssert(input);
	HAPAssert(out);

	HAPAssert(service);
	HAPAssert(signature);
	HAPAssert(name);
	HAPAssert(switch_on);

	
	/**
	 * The 'Service Signature' characteristic of the switch service.
	 */
	signature->format = kHAPCharacteristicFormat_Data;
	signature->iid = *iid + 1;
	signature->characteristicType = &kHAPCharacteristicType_ServiceSignature,
	signature->debugDescription = kHAPCharacteristicDebugDescription_ServiceSignature;
	signature->manufacturerDescription = NULL;
	signature->properties.readable = true;
    signature->properties.writable = false;
    signature->properties.supportsEventNotification = false;
    signature->properties.hidden = false;
    signature->properties.requiresTimedWrite = false;
    signature->properties.supportsAuthorizationData = false;
    signature->properties.ip.controlPoint = true;
    signature->properties.ble.supportsBroadcastNotification = false;
    signature->properties.ble.supportsDisconnectedNotification = false;
    signature->properties.ble.readableWithoutSecurity = false;
    signature->properties.ble.writableWithoutSecurity = false;
	signature->constraints.maxLength = 2097152 ;
	signature->callbacks.handleRead = HAPHandleServiceSignatureRead;
	signature->callbacks.handleWrite = NULL ;

	/**
	 * The 'Name' characteristic of the switch service.
	 */
	name->format = kHAPCharacteristicFormat_String;
	name->iid =  *iid + 2;
	name->characteristicType = &kHAPCharacteristicType_Name;
	name->debugDescription = kHAPCharacteristicDebugDescription_Name;
	name->manufacturerDescription = NULL;
	name->properties.readable = true;
	name->properties.writable = false;
	name->properties.supportsEventNotification = false;
	name->properties.hidden = false;
	name->properties.requiresTimedWrite = false;
	name->properties.supportsAuthorizationData = false;
	name->properties.ip.controlPoint = false;
	name->properties.ip.supportsWriteResponse = false ;
	name->properties.ble.supportsBroadcastNotification = false;
	name->properties.ble.supportsDisconnectedNotification = false;
	name->properties.ble.readableWithoutSecurity = false;
	name->properties.ble.writableWithoutSecurity = false ;
	name->constraints.maxLength = 64;
	name->callbacks.handleRead = HAPHandleNameRead;
	name->callbacks.handleWrite = NULL ;

	/**
	 * The 'On' characteristic of the switch service.
	 */
	switch_on->format = kHAPCharacteristicFormat_Bool;
	switch_on->iid = *iid + 3;
	switch_on->characteristicType = &kHAPCharacteristicType_On;
	switch_on->debugDescription = kHAPCharacteristicDebugDescription_On;
	switch_on->manufacturerDescription = NULL;
	switch_on->properties.readable = true;
	switch_on->properties.writable = true;
	switch_on->properties.supportsEventNotification = true;
	switch_on->properties.hidden = false;
	switch_on->properties.requiresTimedWrite = false;
	switch_on->properties.supportsAuthorizationData = false;
	switch_on->properties.ip.controlPoint = false; 
	switch_on->properties.ip.supportsWriteResponse = false;
	switch_on->properties.ble.supportsBroadcastNotification = true;
	switch_on->properties.ble.supportsDisconnectedNotification = true;
	switch_on->properties.ble.readableWithoutSecurity = false;
	switch_on->properties.ble.writableWithoutSecurity = false ;
	switch_on->callbacks.handleRead = HandleLightBulbOnRead;
	switch_on->callbacks.handleWrite = HandleLightBulbOnWrite ;

	/**
	 * The switch service that contains the 'On' characteristic.
	 */
	HAPCharacteristic **characteristics = calloc(4, sizeof(HAPCharacteristic *));
	characteristics[0] = signature;
    characteristics[1] = name;
    characteristics[2] = switch_on;
    characteristics[3] = NULL;

	char *service_name = calloc(1, 32);
	snprintf(service_name,32,"switch-%ld",localId);
	service->iid = *iid ;
	service->serviceType = &kHAPServiceType_Switch;
	service->debugDescription = kHAPServiceDebugDescription_Switch;
	service->name = service_name;
	service->properties.primaryService = true;
	service->properties.hidden = false; 
	service->properties.ble.supportsConfiguration = false;
	service->linkedServices = NULL;
	service->characteristics = (const HAPCharacteristic*  const* )characteristics; 

	*iid += 4;
	*out = service;
	
	return kHAPError_None;
}




HAPError TemperatureSensorServiceAdd(uint64_t *iid, uint64_t localId, AccessorySerivce* input, HAPService** out)
{
	HAPService * service = calloc(1, sizeof(HAPService));
	HAPDataCharacteristic* signature = calloc(1, sizeof(HAPDataCharacteristic));
	HAPStringCharacteristic* name = calloc(1, sizeof(HAPStringCharacteristic)); 
	HAPFloatCharacteristic *current_temperature = calloc(1, sizeof(HAPFloatCharacteristic));

	HAPAssert(iid);
	HAPAssert(input);
	HAPAssert(out);

	HAPAssert(service);
	HAPAssert(signature);
	HAPAssert(name);
	HAPAssert(current_temperature);

	
	/**
	 * The 'Service Signature' characteristic of the switch service.
	 */
	signature->format = kHAPCharacteristicFormat_Data;
	signature->iid = *iid + 1;
	signature->characteristicType = &kHAPCharacteristicType_ServiceSignature,
	signature->debugDescription = kHAPCharacteristicDebugDescription_ServiceSignature;
	signature->manufacturerDescription = NULL;
	signature->properties.readable = true;
    signature->properties.writable = false;
    signature->properties.supportsEventNotification = false;
    signature->properties.hidden = false;
    signature->properties.requiresTimedWrite = false;
    signature->properties.supportsAuthorizationData = false;
    signature->properties.ip.controlPoint = true;
    signature->properties.ble.supportsBroadcastNotification = false;
    signature->properties.ble.supportsDisconnectedNotification = false;
    signature->properties.ble.readableWithoutSecurity = false;
    signature->properties.ble.writableWithoutSecurity = false;
	signature->constraints.maxLength = 2097152 ;
	signature->callbacks.handleRead = HAPHandleServiceSignatureRead;
	signature->callbacks.handleWrite = NULL ;

	/**
	 * The 'Name' characteristic of the switch service.
	 */
	name->format = kHAPCharacteristicFormat_String;
	name->iid =  *iid + 2;
	name->characteristicType = &kHAPCharacteristicType_Name;
	name->debugDescription = kHAPCharacteristicDebugDescription_Name;
	name->manufacturerDescription = NULL;
	name->properties.readable = true;
	name->properties.writable = false;
	name->properties.supportsEventNotification = false;
	name->properties.hidden = false;
	name->properties.requiresTimedWrite = false;
	name->properties.supportsAuthorizationData = false;
	name->properties.ip.controlPoint = false;
	name->properties.ip.supportsWriteResponse = false ;
	name->properties.ble.supportsBroadcastNotification = false;
	name->properties.ble.supportsDisconnectedNotification = false;
	name->properties.ble.readableWithoutSecurity = false;
	name->properties.ble.writableWithoutSecurity = false ;
	name->constraints.maxLength = 64;
	name->callbacks.handleRead = HAPHandleNameRead;
	name->callbacks.handleWrite = NULL ;

	/**
	 * The current temperature characteristic of the sensor service.
	 */
	current_temperature->format = kHAPCharacteristicFormat_Float;
	current_temperature->iid = *iid + 3;
	current_temperature->characteristicType = &kHAPCharacteristicType_CurrentTemperature;
	current_temperature->debugDescription = kHAPCharacteristicDebugDescription_CurrentTemperature;
	current_temperature->manufacturerDescription = NULL;
	current_temperature->properties.readable = true;
	current_temperature->properties.writable = false;
	current_temperature->properties.supportsEventNotification = true;
	current_temperature->properties.hidden = false;
	current_temperature->properties.requiresTimedWrite = false;
	current_temperature->properties.supportsAuthorizationData = false;
	current_temperature->properties.ip.controlPoint = false; 
	current_temperature->properties.ip.supportsWriteResponse = false;
	current_temperature->properties.ble.supportsBroadcastNotification = true;
	current_temperature->properties.ble.supportsDisconnectedNotification = true;
	current_temperature->properties.ble.readableWithoutSecurity = false;
	current_temperature->properties.ble.writableWithoutSecurity = false ;
	
    current_temperature->units = kHAPCharacteristicUnits_Celsius,
    current_temperature->constraints.minimumValue 	= 0.1;
	current_temperature->constraints.maximumValue 	= 100;
	current_temperature->constraints.stepValue 	= 0.1;

	current_temperature->callbacks.handleRead = HandleTemperatureRead;
	current_temperature->callbacks.handleWrite = NULL ;

	/**
	 * The switch service that contains the 'current_temperature' characteristic.
	 */
	HAPCharacteristic **characteristics = calloc(4, sizeof(HAPCharacteristic *));
	characteristics[0] = signature;
    characteristics[1] = name;
    characteristics[2] = current_temperature;
    characteristics[3] = NULL;

	char *service_name = calloc(1, 32);
	snprintf(service_name,32,"temperature-%ld",localId);
	service->iid = *iid ;
	service->serviceType = &kHAPServiceType_TemperatureSensor;
	service->debugDescription = kHAPServiceDebugDescription_TemperatureSensor;
	service->name = service_name;
	service->properties.primaryService = true;
	service->properties.hidden = false; 
	service->properties.ble.supportsConfiguration = false;
	service->linkedServices = NULL;
	service->characteristics = (const HAPCharacteristic*  const* )characteristics; 

	*iid += 4;
	*out = service;
	
	return kHAPError_None;
}






HAPError HumiditySensorServiceAdd(uint64_t *iid, uint64_t localId, AccessorySerivce* input, HAPService** out)
{
	HAPService * service = calloc(1, sizeof(HAPService));
	HAPDataCharacteristic* signature = calloc(1, sizeof(HAPDataCharacteristic));
	HAPStringCharacteristic* name = calloc(1, sizeof(HAPStringCharacteristic)); 
	HAPFloatCharacteristic *current_humidity = calloc(1, sizeof(HAPFloatCharacteristic));

	HAPAssert(iid);
	HAPAssert(input);
	HAPAssert(out);

	HAPAssert(service);
	HAPAssert(signature);
	HAPAssert(name);
	HAPAssert(current_humidity);

	
	/**
	 * The 'Service Signature' characteristic of the switch service.
	 */
	signature->format = kHAPCharacteristicFormat_Data;
	signature->iid = *iid + 1;
	signature->characteristicType = &kHAPCharacteristicType_ServiceSignature,
	signature->debugDescription = kHAPCharacteristicDebugDescription_ServiceSignature;
	signature->manufacturerDescription = NULL;
	signature->properties.readable = true;
    signature->properties.writable = false;
    signature->properties.supportsEventNotification = false;
    signature->properties.hidden = false;
    signature->properties.requiresTimedWrite = false;
    signature->properties.supportsAuthorizationData = false;
    signature->properties.ip.controlPoint = true;
    signature->properties.ble.supportsBroadcastNotification = false;
    signature->properties.ble.supportsDisconnectedNotification = false;
    signature->properties.ble.readableWithoutSecurity = false;
    signature->properties.ble.writableWithoutSecurity = false;
	signature->constraints.maxLength = 2097152 ;
	signature->callbacks.handleRead = HAPHandleServiceSignatureRead;
	signature->callbacks.handleWrite = NULL ;

	/**
	 * The 'Name' characteristic of the switch service.
	 */
	name->format = kHAPCharacteristicFormat_String;
	name->iid =  *iid + 2;
	name->characteristicType = &kHAPCharacteristicType_Name;
	name->debugDescription = kHAPCharacteristicDebugDescription_Name;
	name->manufacturerDescription = NULL;
	name->properties.readable = true;
	name->properties.writable = false;
	name->properties.supportsEventNotification = false;
	name->properties.hidden = false;
	name->properties.requiresTimedWrite = false;
	name->properties.supportsAuthorizationData = false;
	name->properties.ip.controlPoint = false;
	name->properties.ip.supportsWriteResponse = false ;
	name->properties.ble.supportsBroadcastNotification = false;
	name->properties.ble.supportsDisconnectedNotification = false;
	name->properties.ble.readableWithoutSecurity = false;
	name->properties.ble.writableWithoutSecurity = false ;
	name->constraints.maxLength = 64;
	name->callbacks.handleRead = HAPHandleNameRead;
	name->callbacks.handleWrite = NULL ;

	/**
	 * The current temperature characteristic of the sensor service.
	 */
	current_humidity->format = kHAPCharacteristicFormat_Float;
	current_humidity->iid = *iid + 3;
	current_humidity->characteristicType = &kHAPCharacteristicType_CurrentRelativeHumidity;
	current_humidity->debugDescription = kHAPCharacteristicDebugDescription_CurrentRelativeHumidity;
	current_humidity->manufacturerDescription = NULL;
	current_humidity->properties.readable = true;
	current_humidity->properties.writable = false;
	current_humidity->properties.supportsEventNotification = true;
	current_humidity->properties.hidden = false;
	current_humidity->properties.requiresTimedWrite = false;
	current_humidity->properties.supportsAuthorizationData = false;
	current_humidity->properties.ip.controlPoint = false; 
	current_humidity->properties.ip.supportsWriteResponse = false;
	current_humidity->properties.ble.supportsBroadcastNotification = true;
	current_humidity->properties.ble.supportsDisconnectedNotification = true;
	current_humidity->properties.ble.readableWithoutSecurity = false;
	current_humidity->properties.ble.writableWithoutSecurity = false ;
	
    current_humidity->units = kHAPCharacteristicUnits_Percentage,
    current_humidity->constraints.minimumValue 	= 0.1;
	current_humidity->constraints.maximumValue 	= 100;
	current_humidity->constraints.stepValue 	= 0.1;

	current_humidity->callbacks.handleRead = HandleHumidityRead;
	current_humidity->callbacks.handleWrite = NULL ;

	/**
	 * The switch service that contains the 'current_humidity' characteristic.
	 */
	HAPCharacteristic **characteristics = calloc(4, sizeof(HAPCharacteristic *));
	characteristics[0] = signature;
    characteristics[1] = name;
    characteristics[2] = current_humidity;
    characteristics[3] = NULL;

	char *service_name = calloc(1, 32);
	snprintf(service_name,32,"humidity-%ld",localId);
	service->iid = *iid ;
	service->serviceType = &kHAPServiceType_HumiditySensor;
	service->debugDescription = kHAPServiceDebugDescription_HumiditySensor;
	service->name = service_name;
	service->properties.primaryService = true;
	service->properties.hidden = false; 
	service->properties.ble.supportsConfiguration = false;
	service->linkedServices = NULL;
	service->characteristics = (const HAPCharacteristic*  const* )characteristics; 

	*iid += 4;
	*out = service;
	
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
    size_t numServiceType = 0;
    size_t numServices = 0;
	uint64_t iid;

	
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
		&accessoryConfiguration.baseInfo,
		MAX_SERVICES,
		&numServiceType);
	
    HAPLogInfo(&kHAPLog_Default, "baseInfo.aid: %ld", accessoryConfiguration.baseInfo.aid);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.category: %d", accessoryConfiguration.baseInfo.category);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.name: %s", accessoryConfiguration.baseInfo.name);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.manufacturer: %s", accessoryConfiguration.baseInfo.manufacturer);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.model: %s", accessoryConfiguration.baseInfo.model);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.serialNumber: %s", accessoryConfiguration.baseInfo.serialNumber);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.firmwareVersion: %s", accessoryConfiguration.baseInfo.firmwareVersion);
    HAPLogInfo(&kHAPLog_Default, "baseInfo.hardwareVersion: %s", accessoryConfiguration.baseInfo.hardwareVersion);
	for(uint32_t i = 0; i < numServiceType; i++){
	    HAPLogInfo(&kHAPLog_Default, "baseInfo.servcie[%d]: %s, number: %d",
					i, accessoryConfiguration.baseInfo.services[i].type,
					accessoryConfiguration.baseInfo.services[i].number);
		numServices += accessoryConfiguration.baseInfo.services[i].number; 
	}

    HAPAssert(numServices <= MAX_SERVICES);

	
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


    // Prepare HAPService.
    numServices += 4;
    HAPService** services = calloc(numServices, sizeof(HAPService*));
    if (!services) {
        HAPLog(&kHAPLog_Default, "Cannot allocate more services.");
		goto DONE;
    }

	services[0] = (HAPService*)&accessoryInformationService;
	services[1] = (HAPService*)&hapProtocolInformationService;
	services[2] = (HAPService*)&pairingService;


	uint32_t i , j, k ;
	iid = 0x30;
	for(i = 0,j = 3 ; (i < numServiceType) && (j < numServices); i++){
		AccessorySerivce * service;
		service = &accessoryConfiguration.baseInfo.services[i];

	    HAPLogDebug(&kHAPLog_Default, "add new service type: %s", service->type);

		if(HAPRawBufferAreEqual(service->type, "switch", 6)){

			for(k = 0; k < service->number; k++,j++ ){

				err = SwitchServiceAdd(	&iid, k + 1, service,
										(HAPService**)&services[j]);
			    if (err) {
			        HAPAssert(err == kHAPError_Unknown);
			        HAPFatalError();
					goto DONE;
			    }
			    HAPLogDebug(&kHAPLog_Default, "add new switch service: %lu", iid);
			}
		}
		else if(HAPRawBufferAreEqual(service->type, "humidity", 8)){

			for(k = 0; k < service->number; k++,j++ ){

				err = HumiditySensorServiceAdd(	&iid, k + 1, service,
										(HAPService**)&services[j]);
			    if (err) {
			        HAPAssert(err == kHAPError_Unknown);
			        HAPFatalError();
					goto DONE;
			    }
			    HAPLogDebug(&kHAPLog_Default, "add new humidity service: %lu", iid);
			}
		}
		else if(HAPRawBufferAreEqual(service->type, "temperature", 11)){

			for(k = 0; k < service->number; k++,j++ ){

				err = TemperatureSensorServiceAdd(	&iid, k + 1, service,
										(HAPService**)&services[j]);
			    if (err) {
			        HAPAssert(err == kHAPError_Unknown);
			        HAPFatalError();
					goto DONE;
			    }
			    HAPLogDebug(&kHAPLog_Default, "add new temperature service: %lu", iid);
			}
		}

	}
	
	accessory.services = (const HAPService* const* )services;
	
    HAPLog(&kHAPLog_Default, "new iid: %ld",iid);

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
        
	HAPError err;
	HAPTime now;
	uint32_t localId = 0;
	static 	HAPTime last_time = 0;

	localId = (request->characteristic->iid - 0x30) /4;
	*value = accessoryConfiguration.state.On[localId];
    HAPLogInfo(&kHAPLog_Default, "%s: %s", __func__, *value ? "true" : "false");

	/*
	GET /characteristics HTTP/1.1 
	Host: lights.local:12345
	*/
	/* Wait for 5s, prevent from message sent too frequency */
	now = HAPPlatformClockGetCurrent();
	if(now - last_time > 5000){
	    err = HAPIPByteBufferAppendStringWithFormat(
	            &coap_session.session.outboundBuffer,
				"GET /characteristics HTTP/1.1\r\n"
				"Host: %s\r\n",
	            accessoryConfiguration.baseInfo.name);
	    HAPAssert(!err);

		err = WriteMessageToCoapAgent(&coap_session,0);
	    HAPAssert(!err);

		last_time = HAPPlatformClockGetCurrent();

	}
    return kHAPError_None;
}

HAP_RESULT_USE_CHECK
HAPError HandleLightBulbOnWrite(
        HAPAccessoryServerRef* server,
        const HAPBoolCharacteristicWriteRequest* request,
        bool value,
        void* _Nullable context HAP_UNUSED) {

	HAPError err;
	char json_body[256];
	unsigned long content_length;
	uint64_t localId = 0;


	localId = (request->characteristic->iid - 0x30) /4;
	
	HAPAssert(localId < 8);

    HAPLogInfo(&kHAPLog_Default, "%s,request iid: %ld, local id:%ld",
				__func__,
				request->characteristic->iid,
				localId);

	
    HAPLogInfo(&kHAPLog_Default, "%s: %s", __func__, value ? "true" : "false");
	
    if (accessoryConfiguration.state.On[localId] != value) {
        accessoryConfiguration.state.On[localId] = value;

        SaveAccessoryState();

		content_length = snprintf(json_body,256,
							"{\"characteristics\" : "
							"[{\"aid\" : 2,\"iid\" : %ld, \"localId\" : %ld,\"value\" : %s}]}",
							request->characteristic->iid,
							localId,
							value ? "true" : "false");	

        err = HAPIPByteBufferAppendStringWithFormat(
                &coap_session.session.outboundBuffer,
				"PUT /switchs HTTP/1.1\r\n"
				"Host: %s\r\n"
				"Content-Type: application/hap+json\r\n"
                "Content-Length: %lu\r\n\r\n",
                accessoryConfiguration.baseInfo.name,
                (unsigned long) content_length);
        HAPAssert(!err);

        err = HAPIPByteBufferAppendStringWithFormat(
                &coap_session.session.outboundBuffer,
				"%s", json_body);
        HAPAssert(!err);

		err = WriteMessageToCoapAgent(&coap_session,0);
        HAPAssert(!err);
		
        HAPAccessoryServerRaiseEvent(server, request->characteristic, request->service, request->accessory);
    }

    return kHAPError_None;
}

HAP_RESULT_USE_CHECK
HAPError HandleTemperatureRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPFloatCharacteristicReadRequest* request HAP_UNUSED,
        float* value,
        void* _Nullable context HAP_UNUSED) {
	HAPError err;
	uint64_t xid;
	int temp ;
	HAPTime now;
	static 	HAPTime last_time = 0;
	static 	float historyValue = 0.1;


	/*
	GET /characteristics HTTP/1.1 
	Host: lights.local:12345
	*/
	/* Wait for 2s, prevent from message sent too frequency */
	now = HAPPlatformClockGetCurrent();
	if(now - last_time > 2000){
	    err = HAPIPByteBufferAppendStringWithFormat(
	            &coap_session.session.outboundBuffer,
				"GET /temperature HTTP/1.1\r\n"
				"Host: %s\r\n",
	            accessoryConfiguration.baseInfo.name);
	    HAPAssert(!err);

		err = WriteMessageToCoapAgent(&coap_session,&xid);
	    HAPAssert(!err);

		err = WaitResponseFromCoapAgent(&coap_session,xid,2000);

		if(err == kHAPError_None){
			HAPLogInfo(&kHAPLog_Default, "Get response from coap-agent");
			err = handle_http_response(&coap_session.session,
			        "\"temperature\"",
			        13,
			        value);
			HAPIPByteBufferClear(&coap_session.session.inboundBuffer);
		}
		else{
			HAPLogInfo(&kHAPLog_Default, "Wait response timeout");
		}

	    temp = (int)((*value) * 10 + 0.5);
	    historyValue = temp / 10.0;	

		last_time = HAPPlatformClockGetCurrent();

	}

	*value = historyValue;
	
    HAPLogInfo(&kHAPLog_Default, "%g", *value);

    return kHAPError_None;
}



HAP_RESULT_USE_CHECK
HAPError HandleHumidityRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPFloatCharacteristicReadRequest* request HAP_UNUSED,
        float* value,
        void* _Nullable context HAP_UNUSED) {
	HAPError err;
	int temp ;
	uint64_t xid;
	HAPTime now;
	static 	HAPTime last_time = 0;
	static 	float historyValue = 0.1;
	
	/*
	GET /characteristics HTTP/1.1 
	Host: lights.local:12345
	*/
	/* Wait for 2s, prevent from message sent too frequency */
	now = HAPPlatformClockGetCurrent();
	if(now - last_time > 2000){
	    err = HAPIPByteBufferAppendStringWithFormat(
	            &coap_session.session.outboundBuffer,
				"GET /humidity HTTP/1.1\r\n"
				"Host: %s\r\n",
	            accessoryConfiguration.baseInfo.name);
	    HAPAssert(!err);

		err = WriteMessageToCoapAgent(&coap_session,&xid);
	    HAPAssert(!err);

		err = WaitResponseFromCoapAgent(&coap_session,xid,2000);

		if(err == kHAPError_None){
			HAPLogInfo(&kHAPLog_Default, "Get response from coap-agent");
			err = handle_http_response(&coap_session.session,
			        "\"humidity\"",
			        10,
			        value);
			HAPIPByteBufferClear(&coap_session.session.inboundBuffer);
		}
		else{
			HAPLogInfo(&kHAPLog_Default, "Wait response timeout");
		}

		//Rounding while rounding
	    temp = (int)((*value) * 10 + 0.5);
	    historyValue = temp / 10.0;	

		last_time = HAPPlatformClockGetCurrent();

	}

	*value = historyValue;
    HAPLogInfo(&kHAPLog_Default, "%g", *value);

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

	AccessoryCoapAgentCreate();
	
}

void AppRelease(void) {
    HAPPlatformFileHandleDeregister(coap_session.fileHandle);
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
    HAPAssert(context);
    HAPAssert(fileHandleEvents.isReadyForReading || fileHandleEvents.isReadyForWriting);

	COAP_Session * coap_session = (COAP_Session *)context;


	if(fileHandleEvents.isReadyForReading){
		CoapAgentRecv(coap_session);
	}

}

HAP_RESULT_USE_CHECK
HAPError COAP_SocketNameFormat(char * bytes, size_t numBytes)
{
	size_t i;

	/* Replace char ' ' with '_'*/
	for(i = 0; i < numBytes; i++){
		if(bytes[i] == ' '){
			bytes[i] = '_';
		}
	}
	return kHAPError_None;
}


void * RunSecondLoop(void * args){

	delay_ms(2000);

	HAPPlatformRunSecondLoopCreate();

	HAPPlatformRunSecondLoopRun();

	HAPLogDebug(&kHAPLog_Default, "%s: second loop exit", __func__);

	return NULL;
}


void AccessoryCoapAgentCreate(void)
{
	HAPError err;


    static uint8_t ipInboundBuffers[2048];
    static uint8_t ipOutboundBuffers[2048];

	snprintf(coap_session.uds_sock_name, 
		sizeof(coap_session.uds_sock_name) - 1,
		"/tmp/coap_%s", 
		accessoryConfiguration.baseInfo.name);

	(void)COAP_SocketNameFormat(coap_session.uds_sock_name,sizeof(coap_session.uds_sock_name));	
	
	if(CoapAgentCreate(coap_session.uds_sock_name,&coap_session.sockFd)){
        HAPLogError(&kHAPLog_Default, "%s: CoapAgentCreate failed.", __func__);
	}


    coap_session.session.inboundBuffer.position = 0;
    coap_session.session.inboundBuffer.limit = sizeof ipInboundBuffers;
    coap_session.session.inboundBuffer.capacity = sizeof ipInboundBuffers;
    coap_session.session.inboundBuffer.data = (char*)ipInboundBuffers;
    coap_session.session.inboundBufferMark = 0;
    coap_session.session.outboundBuffer.position = 0;
    coap_session.session.outboundBuffer.limit = sizeof ipOutboundBuffers;
    coap_session.session.outboundBuffer.capacity = sizeof ipOutboundBuffers;
    coap_session.session.outboundBuffer.data = (char*)ipOutboundBuffers;
    coap_session.session.eventNotifications = NULL;
    coap_session.session.maxEventNotifications = 10;
    coap_session.session.numEventNotifications = 0;
    coap_session.session.numEventNotificationFlags = 0;
    coap_session.session.eventNotificationStamp = 0;
    coap_session.session.timedWriteExpirationTime = 0;
    coap_session.session.timedWritePID = 0;
    coap_session.session.semResponse = 0;
	coap_session.session.waitedTransactionId = 0;
	
    err = HAPPlatformSecondFileHandleRegister(
            &coap_session.fileHandle,
            coap_session.sockFd,
            (HAPPlatformFileHandleEvent) {
                    .isReadyForReading = true, .isReadyForWriting = false, .hasErrorConditionPending = false },
            CoapAgentHandleCallback,
            &coap_session);
    if (err) {
        HAPLogError(&kHAPLog_Default, "%s: HAPPlatformFileHandleRegister failed: %u.", __func__, err);
        HAPFatalError();
    }


	HAPPlatformThreadCreate("coap-runloop", 1024*8, 100, RunSecondLoop, NULL);

}


void AppInitialize(
        HAPAccessoryServerOptions* hapAccessoryServerOptions,
        HAPPlatform* hapPlatform,
        HAPAccessoryServerCallbacks* hapAccessoryServerCallbacks)
{
	/* no-op */
}

void AppDeinitialize() {
}
