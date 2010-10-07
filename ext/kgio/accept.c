#include "kgio.h"
#include "missing/accept4.h"
#include "sock_for_fd.h"

static VALUE localhost;
static VALUE cClientSocket;
static VALUE cKgio_Socket;
static VALUE mSocketMethods;
static VALUE iv_kgio_addr;

#if defined(__linux__)
static int accept4_flags = SOCK_CLOEXEC;
#else /* ! linux */
static int accept4_flags = SOCK_CLOEXEC | SOCK_NONBLOCK;
#endif /* ! linux */

struct accept_args {
	int fd;
	struct sockaddr *addr;
	socklen_t *addrlen;
};

static VALUE set_accepted(VALUE klass, VALUE aclass)
{
	VALUE tmp;

	if (NIL_P(aclass))
		aclass = cKgio_Socket;

	tmp = rb_funcall(aclass, rb_intern("included_modules"), 0, 0);
	tmp = rb_funcall(tmp, rb_intern("include?"), 1, mSocketMethods);

	if (tmp != Qtrue)
		rb_raise(rb_eTypeError,
		         "class must include Kgio::SocketMethods");

	cClientSocket = aclass;

	return aclass;
}

static VALUE get_accepted(VALUE klass)
{
	return cClientSocket;
}

static VALUE xaccept(void *ptr)
{
	struct accept_args *a = ptr;

	return (VALUE)accept4(a->fd, a->addr, a->addrlen, accept4_flags);
}

#ifdef HAVE_RB_THREAD_BLOCKING_REGION
#  include <time.h>
/*
 * Try to use a (real) blocking accept() since that can prevent
 * thundering herds under Linux:
 * http://www.citi.umich.edu/projects/linux-scalability/reports/accept.html
 *
 * So we periodically disable non-blocking, but not too frequently
 * because other processes may set non-blocking (especially during
 * a process upgrade) with Rainbows! concurrency model changes.
 */
static int thread_accept(struct accept_args *a, int force_nonblock)
{
	if (force_nonblock)
		set_nonblocking(a->fd);
	return (int)rb_thread_blocking_region(xaccept, a, RUBY_UBF_IO, 0);
}

static void set_blocking_or_block(int fd)
{
	static time_t last_set_blocking;
	time_t now = time(NULL);

	if (last_set_blocking == 0) {
		last_set_blocking = now;
		(void)rb_io_wait_readable(fd);
	} else if ((now - last_set_blocking) <= 5) {
		(void)rb_io_wait_readable(fd);
	} else {
		int flags = fcntl(fd, F_GETFL);
		if (flags == -1)
			rb_sys_fail("fcntl(F_GETFL)");
		if (flags & O_NONBLOCK) {
			flags = fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
			if (flags == -1)
				rb_sys_fail("fcntl(F_SETFL)");
		}
		last_set_blocking = now;
	}
}
#else /* ! HAVE_RB_THREAD_BLOCKING_REGION */
#  include <rubysig.h>
static int thread_accept(struct accept_args *a, int force_nonblock)
{
	int rv;

	/* always use non-blocking accept() under 1.8 for green threads */
	set_nonblocking(a->fd);
	TRAP_BEG;
	rv = (int)xaccept(a);
	TRAP_END;
	return rv;
}
#define set_blocking_or_block(fd) (void)rb_io_wait_readable(fd)
#endif /* ! HAVE_RB_THREAD_BLOCKING_REGION */

static VALUE
my_accept(VALUE io, struct sockaddr *addr, socklen_t *addrlen, int nonblock)
{
	int client;
	struct accept_args a;

	a.fd = my_fileno(io);
	a.addr = addr;
	a.addrlen = addrlen;
retry:
	client = thread_accept(&a, nonblock);
	if (client == -1) {
		switch (errno) {
		case EAGAIN:
			if (nonblock)
				return Qnil;
			set_blocking_or_block(a.fd);
#ifdef ECONNABORTED
		case ECONNABORTED:
#endif /* ECONNABORTED */
#ifdef EPROTO
		case EPROTO:
#endif /* EPROTO */
		case EINTR:
			goto retry;
		case ENOMEM:
		case EMFILE:
		case ENFILE:
#ifdef ENOBUFS
		case ENOBUFS:
#endif /* ENOBUFS */
			errno = 0;
			rb_gc();
			client = thread_accept(&a, nonblock);
		}
		if (client == -1) {
			if (errno == EINTR)
				goto retry;
			rb_sys_fail("accept");
		}
	}
	return sock_for_fd(cClientSocket, client);
}

static void in_addr_set(VALUE io, struct sockaddr_in *addr)
{
	VALUE host = rb_str_new(0, INET_ADDRSTRLEN);
	socklen_t addrlen = (socklen_t)INET_ADDRSTRLEN;
	const char *name;

	name = inet_ntop(AF_INET, &addr->sin_addr, RSTRING_PTR(host), addrlen);
	if (name == NULL)
		rb_sys_fail("inet_ntop");
	rb_str_set_len(host, strlen(name));
	rb_ivar_set(io, iv_kgio_addr, host);
}

/*
 * call-seq:
 *
 *	server = Kgio::TCPServer.new('0.0.0.0', 80)
 *	server.kgio_tryaccept -> Kgio::Socket or nil
 *
 * Initiates a non-blocking accept and returns a generic Kgio::Socket
 * object with the kgio_addr attribute set to the IP address of the
 * connected client on success.
 *
 * Returns nil on EAGAIN, and raises on other errors.
 */
static VALUE tcp_tryaccept(VALUE io)
{
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	VALUE rv = my_accept(io, (struct sockaddr *)&addr, &addrlen, 1);

	if (!NIL_P(rv))
		in_addr_set(rv, &addr);
	return rv;
}

/*
 * call-seq:
 *
 *	server = Kgio::TCPServer.new('0.0.0.0', 80)
 *	server.kgio_accept -> Kgio::Socket or nil
 *
 * Initiates a blocking accept and returns a generic Kgio::Socket
 * object with the kgio_addr attribute set to the IP address of
 * the client on success.
 *
 * On Ruby implementations using native threads, this can use a blocking
 * accept(2) (or accept4(2)) system call to avoid thundering herds.
 */
static VALUE tcp_accept(VALUE io)
{
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	VALUE rv = my_accept(io, (struct sockaddr *)&addr, &addrlen, 0);

	in_addr_set(rv, &addr);
	return rv;
}

/*
 * call-seq:
 *
 *	server = Kgio::UNIXServer.new("/path/to/unix/socket")
 *	server.kgio_tryaccept -> Kgio::Socket or nil
 *
 * Initiates a non-blocking accept and returns a generic Kgio::Socket
 * object with the kgio_addr attribute set (to the value of
 * Kgio::LOCALHOST) on success.
 *
 * Returns nil on EAGAIN, and raises on other errors.
 */
static VALUE unix_tryaccept(VALUE io)
{
	VALUE rv = my_accept(io, NULL, NULL, 1);

	if (!NIL_P(rv))
		rb_ivar_set(rv, iv_kgio_addr, localhost);
	return rv;
}

/*
 * call-seq:
 *
 *	server = Kgio::UNIXServer.new("/path/to/unix/socket")
 *	server.kgio_accept -> Kgio::Socket or nil
 *
 * Initiates a blocking accept and returns a generic Kgio::Socket
 * object with the kgio_addr attribute set (to the value of
 * Kgio::LOCALHOST) on success.
 *
 * On Ruby implementations using native threads, this can use a blocking
 * accept(2) (or accept4(2)) system call to avoid thundering herds.
 */
static VALUE unix_accept(VALUE io)
{
	VALUE rv = my_accept(io, NULL, NULL, 0);

	rb_ivar_set(rv, iv_kgio_addr, localhost);
	return rv;
}

/*
 * call-seq:
 *
 *	Kgio.accept_cloexec? -> true or false
 *
 * Returns true if newly accepted Kgio::Sockets are created with the
 * FD_CLOEXEC file descriptor flag, false if not.
 */
static VALUE get_cloexec(VALUE mod)
{
	return (accept4_flags & SOCK_CLOEXEC) == SOCK_CLOEXEC ? Qtrue : Qfalse;
}

/*
 *
 * call-seq:
 *
 *	Kgio.accept_nonblock? -> true or false
 *
 * Returns true if newly accepted Kgio::Sockets are created with the
 * O_NONBLOCK file status flag, false if not.
 */
static VALUE get_nonblock(VALUE mod)
{
	return (accept4_flags & SOCK_NONBLOCK)==SOCK_NONBLOCK ? Qtrue : Qfalse;
}

/*
 * call-seq:
 *
 *	Kgio.accept_cloexec = true
 *	Kgio.accept_cloexec = false
 *
 * Sets whether or not Kgio::Socket objects created by
 * TCPServer#kgio_accept,
 * TCPServer#kgio_tryaccept,
 * UNIXServer#kgio_accept,
 * and UNIXServer#kgio_tryaccept
 * are created with the FD_CLOEXEC file descriptor flag.
 *
 * This is on by default, as there is little reason to deal to enable
 * it for client sockets on a socket server.
 */
static VALUE set_cloexec(VALUE mod, VALUE boolean)
{
	switch (TYPE(boolean)) {
	case T_TRUE:
		accept4_flags |= SOCK_CLOEXEC;
		return boolean;
	case T_FALSE:
		accept4_flags &= ~SOCK_CLOEXEC;
		return boolean;
	}
	rb_raise(rb_eTypeError, "not true or false");
	return Qnil;
}

/*
 * call-seq:
 *
 *	Kgio.accept_nonblock = true
 *	Kgio.accept_nonblock = false
 *
 * Sets whether or not Kgio::Socket objects created by
 * TCPServer#kgio_accept,
 * TCPServer#kgio_tryaccept,
 * UNIXServer#kgio_accept,
 * and UNIXServer#kgio_tryaccept
 * are created with the O_NONBLOCK file status flag.
 *
 * This defaults to +false+ for GNU/Linux where MSG_DONTWAIT is
 * available (and on newer GNU/Linux, accept4() may also set
 * the non-blocking flag.  This defaults to +true+ on non-GNU/Linux
 * systems.
 */
static VALUE set_nonblock(VALUE mod, VALUE boolean)
{
	switch (TYPE(boolean)) {
	case T_TRUE:
		accept4_flags |= SOCK_NONBLOCK;
		return boolean;
	case T_FALSE:
		accept4_flags &= ~SOCK_NONBLOCK;
		return boolean;
	}
	rb_raise(rb_eTypeError, "not true or false");
	return Qnil;
}

void init_kgio_accept(void)
{
	VALUE cUNIXServer, cTCPServer;
	VALUE mKgio = rb_define_module("Kgio");

	localhost = rb_const_get(mKgio, rb_intern("LOCALHOST"));
	cKgio_Socket = rb_const_get(mKgio, rb_intern("Socket"));
	cClientSocket = cKgio_Socket;
	mSocketMethods = rb_const_get(mKgio, rb_intern("SocketMethods"));

	rb_define_singleton_method(mKgio, "accept_cloexec?", get_cloexec, 0);
	rb_define_singleton_method(mKgio, "accept_cloexec=", set_cloexec, 1);
	rb_define_singleton_method(mKgio, "accept_nonblock?", get_nonblock, 0);
	rb_define_singleton_method(mKgio, "accept_nonblock=", set_nonblock, 1);
	rb_define_singleton_method(mKgio, "accept_class=", set_accepted, 1);
	rb_define_singleton_method(mKgio, "accept_class", get_accepted, 0);

	cUNIXServer = rb_const_get(rb_cObject, rb_intern("UNIXServer"));
	cUNIXServer = rb_define_class_under(mKgio, "UNIXServer", cUNIXServer);
	rb_define_method(cUNIXServer, "kgio_tryaccept", unix_tryaccept, 0);
	rb_define_method(cUNIXServer, "kgio_accept", unix_accept, 0);

	cTCPServer = rb_const_get(rb_cObject, rb_intern("TCPServer"));
	cTCPServer = rb_define_class_under(mKgio, "TCPServer", cTCPServer);
	rb_define_method(cTCPServer, "kgio_tryaccept", tcp_tryaccept, 0);
	rb_define_method(cTCPServer, "kgio_accept", tcp_accept, 0);
	init_sock_for_fd();
	iv_kgio_addr = rb_intern("@kgio_addr");
}
