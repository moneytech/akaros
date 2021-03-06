/* 
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */
/* posix */
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

/* bsd extensions */
#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>

#include <sys/plan9_helpers.h>

/* Put the address of the peer connected to socket FD into *ADDR
   (which is *LEN bytes long), and its actual length into *LEN.  */
int __getpeername(int fd, __SOCKADDR_ARG addr, socklen_t * __restrict alen)
{
	Rock *r;
	int i;
	struct sockaddr_in *rip;
	struct sockaddr_un *runix;

	r = _sock_findrock(fd, 0);
	if (r == 0) {
		errno = ENOTSOCK;
		return -1;
	}

	switch (r->domain) {
	case PF_INET:
		rip = (struct sockaddr_in *)&r->raddr;
		memmove(addr.__sockaddr_in__, rip, sizeof(struct sockaddr_in));
		*alen = sizeof(struct sockaddr_in);
		break;
	case PF_UNIX:
		runix = (struct sockaddr_un *)&r->raddr;
		i = &runix->sun_path[strlen(runix->sun_path)] - (char *)runix;
		memmove(addr.__sockaddr_un__, runix, i);
		*alen = i;
		break;
	default:
		errno = EAFNOSUPPORT;
		return -1;
	}
	return 0;
}

weak_alias(__getpeername, getpeername)
