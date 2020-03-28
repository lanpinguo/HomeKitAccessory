#ifndef _SOCKETS_MANAGER_H
#define _SOCKETS_MANAGER_H

int poll_fd_register_with_priority(int fd,
                                      poll_fd_ready_callback_f callback,
                                      void *cookie,
                                      int priority);

void
process_poll_fds(int priority);

int sockets_manager_core(int timeout);


#endif /* _SOCKETS_MANAGER_H */


