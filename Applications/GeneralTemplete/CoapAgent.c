#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <net/ethernet.h>
#include <sys/types.h> 
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "common_types.h"

#include "HAPPlatform.h"
#include "HAPPlatformFileHandle.h"

#include "HAP+Internal.h"
#include "HAPPlatformClock.h"
#include "CoapAgent.h"

uint32_t debug_lvl = DBG_LOG_INFO;

static const HAPLogObject logObject = { .subsystem = kHAP_LogSubsystem, .category = "CoapAgent" };


/** Build-time flag to disable session security. */
#define kHAPIPAccessoryServer_SessionSecurityDisabled ((bool) false)

/** US-ASCII horizontal-tab character. */
#define kHAPIPAccessoryServerCharacter_HorizontalTab ((char) 9)

/** US-ASCII space character. */
#define kHAPIPAccessoryServerCharacter_Space ((char) 32)


static void prepare_reading_response(COAPUnixDomainSessionDescriptor* session) {
    HAPPrecondition(session);

    util_http_reader_init(&session->httpReader, util_HTTP_READER_TYPE_RESPONSE);
    session->httpReaderPosition = 0;
    session->httpParserError = false;
    session->httpMethod.bytes = NULL;
    session->httpURI.bytes = NULL;
    session->httpHeaderFieldName.bytes = NULL;
    session->httpHeaderFieldValue.bytes = NULL;
    session->httpContentLength.isDefined = false;
    session->httpContentType = kHAPIPAccessoryServerContentType_Unknown;
}


static void update_token(struct util_http_reader* r, char** token, size_t* length) {
    HAPAssert(r);
    HAPAssert(token);
    HAPAssert(length);

    if (!*token) {
        *token = r->result_token;
        *length = r->result_length;
    } else if (r->result_token) {
        HAPAssert(&(*token)[*length] == r->result_token);
        *length += r->result_length;
    }
}


static void read_http_content_length(COAPUnixDomainSessionDescriptor * session) {
    HAPPrecondition(session);

    size_t i;
    int overflow;
    unsigned int v;

    HAPAssert(session->inboundBuffer.data);
    HAPAssert(session->inboundBuffer.position <= session->inboundBuffer.limit);
    HAPAssert(session->inboundBuffer.limit <= session->inboundBuffer.capacity);
    HAPAssert(session->httpReaderPosition <= session->inboundBuffer.position);
    HAPAssert(session->httpReader.state == util_HTTP_READER_STATE_COMPLETED_HEADER_VALUE);
    HAPAssert(!session->httpParserError);
    i = 0;
    while ((i < session->httpHeaderFieldValue.numBytes) &&
           ((session->httpHeaderFieldValue.bytes[i] == kHAPIPAccessoryServerCharacter_Space) ||
            (session->httpHeaderFieldValue.bytes[i] == kHAPIPAccessoryServerCharacter_HorizontalTab))) {
        // Skip whitespace.
        i++;
    }
    HAPAssert(
            (i == session->httpHeaderFieldValue.numBytes) ||
            ((i < session->httpHeaderFieldValue.numBytes) &&
             (session->httpHeaderFieldValue.bytes[i] != kHAPIPAccessoryServerCharacter_Space) &&
             (session->httpHeaderFieldValue.bytes[i] != kHAPIPAccessoryServerCharacter_HorizontalTab)));
    if ((i < session->httpHeaderFieldValue.numBytes) && ('0' <= session->httpHeaderFieldValue.bytes[i]) &&
        (session->httpHeaderFieldValue.bytes[i] <= '9') && !session->httpContentLength.isDefined) {
        overflow = 0;
        session->httpContentLength.value = 0;
        do {
            v = (unsigned int) (session->httpHeaderFieldValue.bytes[i] - '0');
            if (session->httpContentLength.value <= (SIZE_MAX - v) / 10) {
                session->httpContentLength.value = session->httpContentLength.value * 10 + v;
                i++;
            } else {
                overflow = 1;
            }
        } while (!overflow && (i < session->httpHeaderFieldValue.numBytes) &&
                 ('0' <= session->httpHeaderFieldValue.bytes[i]) && (session->httpHeaderFieldValue.bytes[i] <= '9'));
        HAPAssert(
                overflow || (i == session->httpHeaderFieldValue.numBytes) ||
                ((i < session->httpHeaderFieldValue.numBytes) &&
                 ((session->httpHeaderFieldValue.bytes[i] < '0') || (session->httpHeaderFieldValue.bytes[i] > '9'))));
        if (!overflow) {
            while ((i < session->httpHeaderFieldValue.numBytes) &&
                   ((session->httpHeaderFieldValue.bytes[i] == kHAPIPAccessoryServerCharacter_Space) ||
                    (session->httpHeaderFieldValue.bytes[i] == kHAPIPAccessoryServerCharacter_HorizontalTab))) {
                i++;
            }
            HAPAssert(
                    (i == session->httpHeaderFieldValue.numBytes) ||
                    ((i < session->httpHeaderFieldValue.numBytes) &&
                     (session->httpHeaderFieldValue.bytes[i] != kHAPIPAccessoryServerCharacter_Space) &&
                     (session->httpHeaderFieldValue.bytes[i] != kHAPIPAccessoryServerCharacter_HorizontalTab)));
            if (i == session->httpHeaderFieldValue.numBytes) {
                session->httpContentLength.isDefined = true;
            } else {
                session->httpParserError = true;
            }
        } else {
            session->httpParserError = true;
        }
    } else {
        session->httpParserError = true;
    }
}


static void read_http_content_type(COAPUnixDomainSessionDescriptor* session) {
    HAPPrecondition(session);

    HAPAssert(session->inboundBuffer.data);
    HAPAssert(session->inboundBuffer.position <= session->inboundBuffer.limit);
    HAPAssert(session->inboundBuffer.limit <= session->inboundBuffer.capacity);
    HAPAssert(session->httpReaderPosition <= session->inboundBuffer.position);
    HAPAssert(session->httpReader.state == util_HTTP_READER_STATE_COMPLETED_HEADER_VALUE);
    HAPAssert(!session->httpParserError);

    size_t i = 0;
    while ((i < session->httpHeaderFieldValue.numBytes) &&
           ((session->httpHeaderFieldValue.bytes[i] == kHAPIPAccessoryServerCharacter_Space) ||
            (session->httpHeaderFieldValue.bytes[i] == kHAPIPAccessoryServerCharacter_HorizontalTab))) {
        // Skip whitespace.
        i++;
    }
    HAPAssert(
            (i == session->httpHeaderFieldValue.numBytes) ||
            ((i < session->httpHeaderFieldValue.numBytes) &&
             (session->httpHeaderFieldValue.bytes[i] != kHAPIPAccessoryServerCharacter_Space) &&
             (session->httpHeaderFieldValue.bytes[i] != kHAPIPAccessoryServerCharacter_HorizontalTab)));
    if ((i < session->httpHeaderFieldValue.numBytes)) {
        session->httpContentType = kHAPIPAccessoryServerContentType_Unknown;

#define TryAssignContentType(contentType, contentTypeString) \
    do { \
        size_t numContentTypeStringBytes = sizeof(contentTypeString) - 1; \
        if (session->httpHeaderFieldValue.numBytes - i >= numContentTypeStringBytes && \
            HAPRawBufferAreEqual( \
                    &session->httpHeaderFieldValue.bytes[i], (contentTypeString), numContentTypeStringBytes)) { \
            session->httpContentType = (contentType); \
            i += numContentTypeStringBytes; \
        } \
    } while (0)

        // Check longer header values first if multiple have the same prefix.
        TryAssignContentType(kHAPIPAccessoryServerContentType_Application_HAPJSON, "application/hap+json");
        TryAssignContentType(kHAPIPAccessoryServerContentType_Application_OctetStream, "application/octet-stream");
        TryAssignContentType(kHAPIPAccessoryServerContentType_Application_PairingTLV8, "application/pairing+tlv8");

#undef TryAssignContentType

        while ((i < session->httpHeaderFieldValue.numBytes) &&
               ((session->httpHeaderFieldValue.bytes[i] == kHAPIPAccessoryServerCharacter_Space) ||
                (session->httpHeaderFieldValue.bytes[i] == kHAPIPAccessoryServerCharacter_HorizontalTab))) {
            i++;
        }
        HAPAssert(
                (i == session->httpHeaderFieldValue.numBytes) ||
                ((i < session->httpHeaderFieldValue.numBytes) &&
                 (session->httpHeaderFieldValue.bytes[i] != kHAPIPAccessoryServerCharacter_Space) &&
                 (session->httpHeaderFieldValue.bytes[i] != kHAPIPAccessoryServerCharacter_HorizontalTab)));
        if (i != session->httpHeaderFieldValue.numBytes) {
            HAPLogBuffer(
                    &logObject,
                    session->httpHeaderFieldValue.bytes,
                    session->httpHeaderFieldValue.numBytes,
                    "Unknown Content-Type.");
            session->httpContentType = kHAPIPAccessoryServerContentType_Unknown;
        }
    } else {
        session->httpParserError = true;
    }
}


static void read_http(COAPUnixDomainSessionDescriptor* session) {
    HAPPrecondition(session);

    struct util_http_reader* r;

    HAPAssert(session->inboundBuffer.data);
    HAPAssert(session->inboundBuffer.position <= session->inboundBuffer.limit);
    HAPAssert(session->inboundBuffer.limit <= session->inboundBuffer.capacity);
    HAPAssert(session->httpReaderPosition <= session->inboundBuffer.position);
    HAPAssert(!session->httpParserError);
    r = &session->httpReader;
    bool hasContentLength = false;
    bool hasContentType = false;
    do {
        session->httpReaderPosition += util_http_reader_read(
                r,
                &session->inboundBuffer.data[session->httpReaderPosition],
                session->inboundBuffer.position - session->httpReaderPosition);

        switch (r->state) {
            case util_HTTP_READER_STATE_READING_METHOD:
            case util_HTTP_READER_STATE_COMPLETED_METHOD: {
                update_token(r, &session->httpMethod.bytes, &session->httpMethod.numBytes);
            } break;
            case util_HTTP_READER_STATE_READING_URI:
            case util_HTTP_READER_STATE_COMPLETED_URI: {
                update_token(r, &session->httpURI.bytes, &session->httpURI.numBytes);
            } break;
            case util_HTTP_READER_STATE_READING_HEADER_NAME:
            case util_HTTP_READER_STATE_COMPLETED_HEADER_NAME: {
                update_token(r, &session->httpHeaderFieldName.bytes, &session->httpHeaderFieldName.numBytes);
            } break;
            case util_HTTP_READER_STATE_READING_HEADER_VALUE: {
                update_token(r, &session->httpHeaderFieldValue.bytes, &session->httpHeaderFieldValue.numBytes);
            } break;
            case util_HTTP_READER_STATE_COMPLETED_HEADER_VALUE: {
                update_token(r, &session->httpHeaderFieldValue.bytes, &session->httpHeaderFieldValue.numBytes);
                HAPAssert(session->httpHeaderFieldName.bytes);
                if ((session->httpHeaderFieldName.numBytes == 14) &&
                    (session->httpHeaderFieldName.bytes[0] == 'C' || session->httpHeaderFieldName.bytes[0] == 'c') &&
                    (session->httpHeaderFieldName.bytes[1] == 'O' || session->httpHeaderFieldName.bytes[1] == 'o') &&
                    (session->httpHeaderFieldName.bytes[2] == 'N' || session->httpHeaderFieldName.bytes[2] == 'n') &&
                    (session->httpHeaderFieldName.bytes[3] == 'T' || session->httpHeaderFieldName.bytes[3] == 't') &&
                    (session->httpHeaderFieldName.bytes[4] == 'E' || session->httpHeaderFieldName.bytes[4] == 'e') &&
                    (session->httpHeaderFieldName.bytes[5] == 'N' || session->httpHeaderFieldName.bytes[5] == 'n') &&
                    (session->httpHeaderFieldName.bytes[6] == 'T' || session->httpHeaderFieldName.bytes[6] == 't') &&
                    (session->httpHeaderFieldName.bytes[7] == '-') &&
                    (session->httpHeaderFieldName.bytes[8] == 'L' || session->httpHeaderFieldName.bytes[8] == 'l') &&
                    (session->httpHeaderFieldName.bytes[9] == 'E' || session->httpHeaderFieldName.bytes[9] == 'e') &&
                    (session->httpHeaderFieldName.bytes[10] == 'N' || session->httpHeaderFieldName.bytes[10] == 'n') &&
                    (session->httpHeaderFieldName.bytes[11] == 'G' || session->httpHeaderFieldName.bytes[11] == 'g') &&
                    (session->httpHeaderFieldName.bytes[12] == 'T' || session->httpHeaderFieldName.bytes[12] == 't') &&
                    (session->httpHeaderFieldName.bytes[13] == 'H' || session->httpHeaderFieldName.bytes[13] == 'h')) {
                    if (hasContentLength) {
                        HAPLog(&logObject, "Request has multiple Content-Length headers.");
                        session->httpParserError = true;
                    } else {
                        hasContentLength = true;
                        read_http_content_length(session);
                    }
                } else if (
                        (session->httpHeaderFieldName.numBytes == 12) &&
                        (session->httpHeaderFieldName.bytes[0] == 'C' ||
                         session->httpHeaderFieldName.bytes[0] == 'c') &&
                        (session->httpHeaderFieldName.bytes[1] == 'O' ||
                         session->httpHeaderFieldName.bytes[1] == 'o') &&
                        (session->httpHeaderFieldName.bytes[2] == 'N' ||
                         session->httpHeaderFieldName.bytes[2] == 'n') &&
                        (session->httpHeaderFieldName.bytes[3] == 'T' ||
                         session->httpHeaderFieldName.bytes[3] == 't') &&
                        (session->httpHeaderFieldName.bytes[4] == 'E' ||
                         session->httpHeaderFieldName.bytes[4] == 'e') &&
                        (session->httpHeaderFieldName.bytes[5] == 'N' ||
                         session->httpHeaderFieldName.bytes[5] == 'n') &&
                        (session->httpHeaderFieldName.bytes[6] == 'T' ||
                         session->httpHeaderFieldName.bytes[6] == 't') &&
                        (session->httpHeaderFieldName.bytes[7] == '-') &&
                        (session->httpHeaderFieldName.bytes[8] == 'T' ||
                         session->httpHeaderFieldName.bytes[8] == 't') &&
                        (session->httpHeaderFieldName.bytes[9] == 'Y' ||
                         session->httpHeaderFieldName.bytes[9] == 'y') &&
                        (session->httpHeaderFieldName.bytes[10] == 'P' ||
                         session->httpHeaderFieldName.bytes[10] == 'p') &&
                        (session->httpHeaderFieldName.bytes[11] == 'E' ||
                         session->httpHeaderFieldName.bytes[11] == 'e')) {
                    if (hasContentType) {
                        HAPLog(&logObject, "Request has multiple Content-Type headers.");
                        session->httpParserError = true;
                    } else {
                        hasContentType = true;
                        read_http_content_type(session);
                    }
                }
                session->httpHeaderFieldName.bytes = NULL;
                session->httpHeaderFieldValue.bytes = NULL;
            } break;
            default: {
            } break;
        }
    } while ((session->httpReaderPosition < session->inboundBuffer.position) &&
             (r->state != util_HTTP_READER_STATE_DONE) && (r->state != util_HTTP_READER_STATE_ERROR) &&
             !session->httpParserError);
    HAPAssert(
            (session->httpReaderPosition == session->inboundBuffer.position) ||
            ((session->httpReaderPosition < session->inboundBuffer.position) &&
             ((r->state == util_HTTP_READER_STATE_DONE) || (r->state == util_HTTP_READER_STATE_ERROR) ||
              session->httpParserError)));
}









int udsAddrGenerate(struct sockaddr_un * sockaddr,const char * path)
{
	uint32_t  addrlen;

	memset(sockaddr, 0, sizeof(struct sockaddr_un));
	sockaddr->sun_family = AF_UNIX;
	snprintf(sockaddr->sun_path, sizeof(sockaddr->sun_path) - 1,"%s", path);
	addrlen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr->sun_path);

	return addrlen;

}


/* socket is created as a blocking socket */
int  CoapAgentCreate(const char* pathname,int *sockId)
{
	struct sockaddr_un sockaddr;
	uint32_t  addrlen;
	int sockfd;
	int rc;
	unsigned int rcvSize = (1024 * 256); /* bytes */



	addrlen = udsAddrGenerate(&sockaddr,pathname);

  
	sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (sockfd < 0)
	{
		return RC_E_UNAVAIL;
	}

  
	unlink((const char *)sockaddr.sun_path);     /* remove old socket file if it exists */

	rc = bind(sockfd, (const struct sockaddr *)&sockaddr, addrlen);
	if (rc < 0)
	{
		close(sockfd);
		return RC_E_INTERNAL;
	}


	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvSize, sizeof(rcvSize)) == -1)
	{
		close(sockfd);
		return RC_E_INTERNAL;
	}

	*sockId = sockfd;

	return RC_E_NONE;
}






/*********************************************************************
* @purpose  Send an message to the server.
*
* @returns  RC_E_NONE if message sent.
*           RC_E_FAIL if message not sent or partially sent.
*
* @notes    The socket is a nonblocking socket.
*
* @end
*********************************************************************/
int CoapAgentMsgSend(int srcSockFd, struct sockaddr *dest, uint32_t addrlen,uint8_t *msg,ssize_t len)
{
	ssize_t   bytesSent;
	int flags = 0;

	ssize_t msgLen = len ;




	if(srcSockFd == 0)
	{
		HAPLogError(&logObject,
				"Failed to send pkt cb info to port ft on socket with fd %d. \r\n",
				srcSockFd);
		
		return RC_E_FAIL;

	}

	bytesSent = sendto(srcSockFd, msg, msgLen, flags, dest, addrlen);

	if (bytesSent != msgLen)
	{
		HAPLogError(&logObject,
			"Failed to send pkt cb msg to client on "
			"socket with fd %d. Error (%d, %s).",
			srcSockFd, errno, strerror(errno));
		return RC_E_FAIL;
	}

	return RC_E_NONE;
}

static uint64_t  xid_counter = 0;

HAPError CoapAgentSend(COAP_Session* _Nonnull coap_session, uint64_t * _Nullable  xid)
{
    HAPPrecondition(coap_session);

	struct sockaddr_un server;	
	uint32_t addrlen;


	xid_counter++;
	if(xid){
		if(coap_session->session.semResponse  != NULL){
			sal_sem_destroy(coap_session->session.semResponse);
		}
		coap_session->session.semResponse = sal_sem_create("coapAgent",0);
		HAPAssert(coap_session->session.semResponse);
		*xid = xid_counter;
	}
	
	HAPLogBufferDebug(&logObject,
		coap_session->session.outboundBuffer.data,
		coap_session->session.outboundBuffer.limit,"%s",__func__);
	addrlen = udsAddrGenerate(&server,"/tmp/borderAgent");
	CoapAgentMsgSend(coap_session->sockFd , 
		(struct sockaddr*)&server, 
		addrlen,
		(uint8_t*)coap_session->session.outboundBuffer.data,
		coap_session->session.outboundBuffer.limit);
	HAPIPByteBufferClear(&coap_session->session.outboundBuffer);
	return kHAPError_None;
}


uint32_t CoapAgentRecv(COAP_Session* coap_session )
{
	ssize_t recvBytes;
	//int rc;
	int flags = 0;

    HAPAssert(coap_session);


	/* set socket to non-blocking for this read */
	flags |= MSG_DONTWAIT;

	recvBytes = recvfrom(coap_session->sockFd,
		coap_session->session.inboundBuffer.data,
		coap_session->session.inboundBuffer.capacity,
		flags, 0, 0);

	if (recvBytes < 0)
	{
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
		{
			/* Normal if no packets waiting to be received and caller didn't block. */
			return 0;
		}
		HAPLogError(&logObject,
					"Failed to receive packet. recvfrom() returned %ld. errno %s.\r\n",
					recvBytes, strerror(errno));
		return 0;
	}


	coap_session->session.inboundBuffer.limit = recvBytes;
	coap_session->session.inboundBuffer.position = recvBytes;
	
	HAPLogBufferDebug(&logObject,
		coap_session->session.inboundBuffer.data,
		coap_session->session.inboundBuffer.limit,"%s",__func__);

    prepare_reading_response(&coap_session->session);

	read_http(&coap_session->session);

    HAPLogBufferDebug(
            &logObject,
            coap_session->session.inboundBuffer.data + coap_session->session.httpReaderPosition ,
            coap_session->session.httpContentLength.value,"%s",__func__);

    if ((coap_session->session.httpReader.state == util_HTTP_READER_STATE_ERROR) 
			|| coap_session->session.httpParserError) {
		HAPLogError(&logObject,"Unexpected request.");
		goto EXIT;
    } 
			
	if(coap_session->session.semResponse  != NULL){
		sal_sem_give(coap_session->session.semResponse);
	}
	
EXIT:
	return recvBytes;
}




HAPError WriteMessageToCoapAgent(	
			COAP_Session* coap_session,
	        uint64_t* xid){

	HAPError err;
	HAPPrecondition(coap_session);




	coap_session->session.outboundBuffer.limit = coap_session->session.outboundBuffer.position;


	err = CoapAgentSend(coap_session,xid);


    return err;

}


HAPError WaitResponseFromCoapAgent( 			COAP_Session* coap_session,
										        uint64_t xid, uint64_t timeout){

	HAPError err = kHAPError_None;
	int rc;
	HAPPrecondition(coap_session);


	coap_session->session.waitedTransactionId = xid;

	rc = sal_sem_take(coap_session->session.semResponse,timeout * 1000);

	if(rc != 0){
		err = kHAPError_Busy ;
	}
    return err;

}



int CoapMsgRecvWithTimeout(int fd, uint8_t *buf, 
	uint32_t bufLen, uint32_t *recvLen,struct timeval *timeout)
{
	ssize_t recvBytes;
	int rv;
	int flags = 0;



	if (fd < 0)
	{
		return RC_E_FAIL;
	}

	if (timeout)
	{
		if ((timeout->tv_sec == 0) && (timeout->tv_usec == 0))
		{
			/* set socket to non-blocking for this read */
			flags |= MSG_DONTWAIT;
		}
		else
		{
			/* blocking socket with a timeout */
			rv = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)timeout,
			  sizeof(struct timeval));
			if (rv < 0)
			{
				HAPLogError(&logObject,"Failed to set packet receive timeout. Error %d.\r\n", rv);
				return RC_E_FAIL;
			}
		}
	}
	else
	{
		/* blocking socket with no timeout. Make sure there is no timeout configured
		* on the socket from previous call. */
		rv = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, NULL, 0);
	}

	recvBytes = recvfrom(fd, buf, bufLen, flags, 0, 0);

	if (recvBytes < 0)
	{
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
		{
			/* Normal if no packets waiting to be received and caller didn't block. */
			return RC_E_TIMEOUT;
		}
		HAPLogError(&logObject,"Failed to receive packet. recvfrom() returned %ld. errno %s.\r\n",
		  recvBytes, strerror(errno));
		return RC_E_FAIL;
	}


	*recvLen = recvBytes;
	return RC_E_NONE;
}












#define RECV_TIMEOUT	2000

void test_coap_response_get(int srcSockFd)
{
	RC_ERROR_t rc;
	struct timeval timeout;
	HAPTime start,now;
	uint32_t msgLen = 0;
	uint8_t msg[200];



	memset(msg,0,200);
	start = HAPPlatformClockGetCurrent();
	now = start;
	timeout.tv_sec = 2;
	for(;(now - start) < RECV_TIMEOUT;){
		rc = CoapMsgRecvWithTimeout(srcSockFd, msg, 200, &msgLen,&timeout);
		now = HAPPlatformClockGetCurrent();
		if(rc != RC_E_TIMEOUT){
			int32_t tmp;
			debug_log(DBG_LOG_INFO,"sync recv(%d): %s",msgLen,msg);
			tmp = timeout.tv_sec * 1000 + timeout.tv_usec / 1000;
			tmp -= (now - start);
			timeout.tv_sec = tmp / 1000;
			timeout.tv_usec = (tmp % 1000) * 1000;
			continue;
		}
		else{
			/* Wait timeout */
			debug_log(DBG_LOG_INFO,"sync recv timeout");
			return ;
		}
	}
}



void test_coap(int srcSockFd)
{
	struct sockaddr_un server;	
	uint32_t addrlen;
	uint32_t msgLen;
	char msg[200];
	char res[]	= "relay-sw";
	char payload[] = "&state=0xFF&mask=0xF0";
	char ip[] = "fd00::212:4b00:1940:c16c";


	
	msgLen = snprintf(msg,200,"post://[%ld]/[%s]/%s%s",strlen(res),ip,res,payload);

	debug_log(DBG_LOG_INFO,"send(%d): %s",msgLen,msg);
	addrlen = udsAddrGenerate(&server,"/tmp/borderAgent");
	CoapAgentMsgSend(srcSockFd, (struct sockaddr*)&server, addrlen,(uint8_t*)msg,msgLen);


}

