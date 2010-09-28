#ifdef HAVE_ACCEPT4
#  define A4_SOCK_CLOEXEC SOCK_CLOEXEC
#  define A4_SOCK_NONBLOCK SOCK_NONBLOCK
#else
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif
#  include <sys/types.h>
#  include <sys/socket.h>
#  ifndef SOCK_CLOEXEC
#    if (FD_CLOEXEC == O_NONBLOCK)
#      define A4_SOCK_CLOEXEC 1
#      define A4_SOCK_NONBLOCK 2
#    else
#      define A4_SOCK_CLOEXEC FD_CLOEXEC
#      define A4_SOCK_NONBLOCK O_NONBLOCK
#    endif
#  else
#    define A4_SOCK_CLOEXEC SOCK_CLOEXEC
#    define A4_SOCK_NONBLOCK SOCK_NONBLOCK
#  endif

/* accept4() is currently a Linux-only goodie */
static int
accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
	int fd = accept(sockfd, addr, addrlen);

	if (fd >= 0) {
		if ((flags & A4_SOCK_CLOEXEC) == A4_SOCK_CLOEXEC)
			(void)fcntl(fd, F_SETFD, FD_CLOEXEC);

		/*
		 * Some systems inherit O_NONBLOCK across accept().
		 * We also expect our users to use MSG_DONTWAIT under
		 * Linux, so fcntl() is completely unnecessary
		 * in most cases...
		 */
		if ((flags & A4_SOCK_NONBLOCK) == A4_SOCK_NONBLOCK) {
			int fl = fcntl(fd, F_GETFL);

			if ((fl & O_NONBLOCK) == 0)
				(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		}

		/*
		 * nothing we can do about fcntl() errors in this wrapper
		 * function, let the user (Ruby) code figure it out
		 */
		errno = 0;
	}
	return fd;
}
#endif /* !HAVE_ACCEPT4 */
