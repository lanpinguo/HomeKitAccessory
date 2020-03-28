#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include "common_types.h"



#define POLL_COUNT_MAX	1024
/* Indexed by socket descriptor */
static soc_map_t soc_map[POLL_COUNT_MAX];

/* Dense array passed to poll(2) */
static struct pollfd pollfds[POLL_COUNT_MAX];
static int num_pollfds = 0;



#define IS_ACTIVE_SOCKET_ID(_id) (soc_map[_id].fd == (_id))
#define IS_LEGAL_SOCKET_ID(_id) (((_id) >= 0) && ((_id) < POLL_COUNT_MAX))
#define POLLFD_INDEX(_id) soc_map[(_id)].pollfd_index



int poll_fd_register_with_priority(int fd,
                                      poll_fd_ready_callback_f callback,
                                      void *cookie,
                                      int priority)
{
    struct pollfd *pfd;

    debug_log(DBG_LOG_INFO,"Register socket %d", fd);
    if (!IS_LEGAL_SOCKET_ID(fd)) {
        debug_log(DBG_LOG_ERR,"Socket ID out of range: id %d", fd);
        return -1;
    }

    if (callback == NULL) {
        debug_log(DBG_LOG_ERR,"No callback specified");
        return -1;
    }

    if (IS_ACTIVE_SOCKET_ID(fd)) {
        debug_log(DBG_LOG_INFO,"Socket %d exists", fd);
        return -2;
    }

    if(num_pollfds >= POLL_COUNT_MAX){
		return -2;
	}
	
    soc_map[fd].fd = fd;
    soc_map[fd].pollfd_index = num_pollfds;
    soc_map[fd].callback = callback;
    soc_map[fd].cookie = cookie;
    soc_map[fd].priority = priority;

    pfd = &pollfds[num_pollfds++];
    pfd->fd = fd;
    pfd->events = POLLIN;
    pfd->revents = 0;

	
    return 0;
}


/*
 * Run callbacks for each ready socket.
 */
static void
process_poll_fds(int priority)
{
    int i;
    for (i = 0; i < num_pollfds; i++) {
        struct pollfd *pfd = &pollfds[i];
        int read_ready, write_ready, error_seen;

        if (soc_map[pfd->fd].priority != priority) {
            continue;
        }

        read_ready = (pfd->revents & POLLIN) != 0;
        write_ready = (pfd->revents & POLLOUT) != 0;
        error_seen = (pfd->revents & POLLERR) != 0;
        if (read_ready || write_ready || error_seen) {
            //before_callback();
            soc_map[pfd->fd].callback(pfd->fd, soc_map[pfd->fd].cookie,
                    read_ready, write_ready, error_seen);
            //after_callback();
        }
    }
}


int sockets_manager_core(int timeout)
{
	int rc = 0;

    rc = poll(pollfds, num_pollfds, timeout);

    if (rc < 0 && errno != EINTR) {
        debug_log(DBG_LOG_ERR,"Error in poll: %s", strerror(errno));
        return rc;
    }

    process_poll_fds(0);

	return rc;
}


