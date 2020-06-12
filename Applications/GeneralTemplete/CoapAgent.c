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
#include "HAPPlatformClock.h"


uint32_t debug_lvl = DBG_LOG_INFO;

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
	  debug_log(DBG_LOG_ERR,
						 "Failed to send pkt cb info to port ft on "
						 "socket with fd %d. \r\n",
						 srcSockFd);
	  return RC_E_FAIL;

  }
  
  bytesSent = sendto(srcSockFd, msg, msgLen, flags, dest, addrlen);

  if (bytesSent != msgLen)
  {
    debug_log(DBG_LOG_ERR,
                       "Failed to send pkt cb msg to client on "
                       "socket with fd %d. Error (%d, %s).",
                       srcSockFd, errno, strerror(errno));
    return RC_E_FAIL;
  }

  return RC_E_NONE;
}

uint32_t CoapAgentRecv(int fd)
{
	ssize_t recvBytes;
	//int rc;
	int flags = 0;
	uint8_t *buf;



	buf = (uint8_t *)calloc(1,1024);
	/* set socket to non-blocking for this read */
	flags |= MSG_DONTWAIT;

	recvBytes = recvfrom(fd, buf, 1024, flags, 0, 0);

	if (recvBytes < 0)
	{
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
		{
			/* Normal if no packets waiting to be received and caller didn't block. */
			return 0;
		}
		debug_log(DBG_LOG_ERR,"Failed to receive packet. recvfrom() returned %ld. errno %s.\r\n",
		      recvBytes, strerror(errno));
		return 0;
	}

	debug_log(DBG_LOG_INFO,"callback recv(%ld): %s",recvBytes,buf);

	free(buf);
	return recvBytes;
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
				debug_log(DBG_LOG_ERR,"Failed to set packet receive timeout. Error %d.\r\n", rv);
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
		debug_log(DBG_LOG_ERR,"Failed to receive packet. recvfrom() returned %ld. errno %s.\r\n",
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

