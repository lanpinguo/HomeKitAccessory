#ifndef _IPC_HELPER_H
#define _IPC_HELPER_H

int udsAddrGenerate(struct sockaddr_un * sockaddr,const char * path);
int rcPipeServerCreate(const char* pathname,int *sockId);
int rcPipeMsgRecv(int fd, uint8_t *buf, uint32_t *buf_len,struct timeval *timeout);
int rcPipeMsgSend(int srcSockFd, struct sockaddr *dest, uint32_t addrlen,uint8_t *msg,ssize_t len);


#endif /* _IPC_HELPER_H */

