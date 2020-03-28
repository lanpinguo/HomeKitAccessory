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
#include "sockets_manager.h"


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
int  rcPipeServerCreate(const char* pathname,int *sockId)
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
int rcPipeMsgSend(int srcSockFd, struct sockaddr *dest, uint32_t addrlen,uint8_t *msg,ssize_t len)
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




int rcPipeMsgRecv(int fd, uint8_t *buf, uint32_t *buf_len,struct timeval *timeout)
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

  recvBytes = recvfrom(fd, buf, *buf_len, flags, 0, 0);

  if (recvBytes < 0)
  {
    if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
    {
      /* Normal if no packets waiting to be received and caller didn't block. */
      return RC_E_TIMEOUT;
    }
    debug_log(DBG_LOG_ERR,"Failed to receive packet. recvfrom() returned %d. errno %s.\r\n",
                      (int)recvBytes, strerror(errno));
    return RC_E_FAIL;
  }


	*buf_len = recvBytes;
  return RC_E_NONE;
}




