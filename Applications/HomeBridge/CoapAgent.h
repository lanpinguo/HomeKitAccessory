#ifndef _IPC_HELPER_H
#define _IPC_HELPER_H

int  CoapAgentCreate(const char* pathname,int *sockId);
void test_coap_response_get(int srcSockFd);

void test_coap(int srcSockFd);
uint32_t CoapAgentRecv(int fd);


#endif /* _IPC_HELPER_H */

