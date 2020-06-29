#ifndef _IPC_HELPER_H
#define _IPC_HELPER_H

#include "HAPPlatformSync.h"

/**
 * Unix Domain session descriptor.
 */
typedef struct {

	sal_mutex_t _Nullable mutex_recive;

	uint64_t 	 waited_xid;

    /** IP session state. */
    HAPIPSessionState state;

    /** Time stamp of last activity on this session. */
    HAPTime stamp;


    /** Inbound buffer. */
    HAPIPByteBuffer inboundBuffer;

    /** Marked inbound buffer position indicating the position until which the buffer has been decrypted. */
    size_t inboundBufferMark;

    /** Outbound buffer. */
    HAPIPByteBuffer outboundBuffer;

    /**
     * Marked outbound buffer position indicating the position until which the buffer has not yet been encrypted
     * (starting from outboundBuffer->limit).
     */
    size_t outboundBufferMark;

    /** HTTP reader. */
    struct util_http_reader httpReader;

    /** Current position of the HTTP reader in the inbound buffer. */
    size_t httpReaderPosition;

    /** Flag indication whether an error has been encountered while parsing a HTTP message. */
    bool httpParserError;

    /**
     * HTTP/1.1 Method.
     */
    struct {
        /**
         * Pointer to the HTTP/1.1 method in the inbound buffer.
         */
        char* _Nullable bytes;

        /**
         * Length of the HTTP/1.1 method in the inbound buffer.
         */
        size_t numBytes;
    } httpMethod;

    /**
     * HTTP/1.1 URI.
     */
    struct {
        /**
         * Pointer to the HTTP/1.1 URI in the inbound buffer.
         */
        char* _Nullable bytes;

        /**
         * Length of the HTTP/1.1 URI in the inbound buffer.
         */
        size_t numBytes;
    } httpURI;

    /**
     * HTTP/1.1 Header Field Name.
     */
    struct {
        /**
         * Pointer to the current HTTP/1.1 header field name in the inbound buffer.
         */
        char* _Nullable bytes;

        /**
         * Length of the current HTTP/1.1 header field name in the inbound buffer.
         */
        size_t numBytes;
    } httpHeaderFieldName;

    /**
     * HTTP/1.1 Header Field Value.
     */
    struct {
        /**
         * Pointer to the current HTTP/1.1 header value in the inbound buffer.
         */
        char* _Nullable bytes;

        /**
         * Length of the current HTTP/1.1 header value in the inbound buffer.
         */
        size_t numBytes;
    } httpHeaderFieldValue;

    /**
     * HTTP/1.1 Content Length.
     */
    struct {
        /**
         * Flag indicating whether a HTTP/1.1 content length is defined.
         */
        bool isDefined;

        /**
         * HTTP/1.1 content length.
         */
        size_t value;
    } httpContentLength;

    /**
     * HTTP/1.1 Content Type.
     */
    HAPIPAccessoryServerContentType httpContentType;

    /**
     * Array of event notification contexts on this session.
     */
    HAPIPEventNotificationRef* _Nullable eventNotifications;

    /**
     * The maximum number of events this session can handle.
     */
    size_t maxEventNotifications;

    /**
     * The number of subscribed events on this session.
     */
    size_t numEventNotifications;

    /**
     * The number of raised events on this session.
     */
    size_t numEventNotificationFlags;

    /**
     * Time stamp of last event notification on this session.
     */
    HAPTime eventNotificationStamp;

    /**
     * Time when the request expires. 0 if no timed write in progress.
     */
    HAPTime timedWriteExpirationTime;

    /**
     * PID of timed write. Must match "pid" of next PUT /characteristics.
     */
    uint64_t timedWritePID;

    /**
     * Serialization context for incremental accessory attribute database serialization.
     */
    HAPIPAccessorySerializationContext accessorySerializationContext;

    /**
     * Flag indicating whether incremental serialization of accessory attribute database is in progress.
     */
    bool accessorySerializationIsInProgress;
} COAPUnixDomainSessionDescriptor;


typedef struct  
{
	int sockFd;	
	char uds_sock_name[128];

	HAPPlatformFileHandleRef fileHandle;
	COAPUnixDomainSessionDescriptor session;
} COAP_Session;
  
int  CoapAgentCreate(const char* _Nonnull pathname,int * _Nullable sockId);

uint32_t CoapAgentRecv(COAP_Session* _Nonnull coap_session );
HAPError CoapAgentSend(COAP_Session* _Nonnull coap_session, uint64_t * _Nullable  xid);


HAPError WriteMessageToCoapAgent(			COAP_Session* _Nonnull coap_session,
												uint64_t* _Nullable xid);
HAPError WaitResponseFromCoapAgent( 			COAP_Session* _Nonnull coap_session,
										        uint64_t xid, uint64_t timeout);


#endif /* _IPC_HELPER_H */

