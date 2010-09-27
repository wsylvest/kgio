#include <ruby.h>
#ifdef HAVE_RUBY_IO_H
#  include <ruby/io.h>
#else
#  include <rubyio.h>
#endif
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <assert.h>

#include "missing/accept4.h"
#include "nonblock.h"
#include "my_fileno.h"
#include "sock_for_fd.h"

#if defined(__linux__)
/*
 * we know MSG_DONTWAIT works properly on all stream sockets under Linux
 * we can define this macro for other platforms as people care and
 * notice.
 */
#  define USE_MSG_DONTWAIT
static int accept4_flags = A4_SOCK_CLOEXEC;
#else /* ! linux */
static int accept4_flags = A4_SOCK_CLOEXEC | A4_SOCK_NONBLOCK;
#endif /* ! linux */

static VALUE cSocket;
static VALUE localhost;
static VALUE mKgio_WaitReadable, mKgio_WaitWritable;
static ID io_wait_rd, io_wait_wr;
static ID iv_kgio_addr;

struct io_args {
	VALUE io;
	VALUE buf;
	char *ptr;
	long len;
	int fd;
};

struct accept_args {
	int fd;
	struct sockaddr *addr;
	socklen_t *addrlen;
};

static void wait_readable(VALUE io)
{
	if (io_wait_rd) {
		(void)rb_funcall(io, io_wait_rd, 0, 0);
	} else {
		int fd = my_fileno(io);

		if (!rb_io_wait_readable(fd))
			rb_sys_fail("wait readable");
	}
}

static void wait_writable(VALUE io)
{
	if (io_wait_wr) {
		(void)rb_funcall(io, io_wait_wr, 0, 0);
	} else {
		int fd = my_fileno(io);

		if (!rb_io_wait_writable(fd))
			rb_sys_fail("wait writable");
	}
}

static void prepare_read(struct io_args *a, int argc, VALUE *argv, VALUE io)
{
	VALUE length;

	a->io = io;
	a->fd = my_fileno(io);
	rb_scan_args(argc, argv, "11", &length, &a->buf);
	a->len = NUM2LONG(length);
	if (NIL_P(a->buf)) {
		a->buf = rb_str_new(NULL, a->len);
	} else {
		StringValue(a->buf);
		rb_str_resize(a->buf, a->len);
	}
	a->ptr = RSTRING_PTR(a->buf);
}

static int read_check(struct io_args *a, long n, const char *msg, int io_wait)
{
	if (n == -1) {
		if (errno == EINTR)
			return -1;
		rb_str_set_len(a->buf, 0);
		if (errno == EAGAIN) {
			if (io_wait) {
				wait_readable(a->io);
				return -1;
			} else {
				a->buf = mKgio_WaitReadable;
				return 0;
			}
		}
		rb_sys_fail(msg);
	}
	rb_str_set_len(a->buf, n);
	if (n == 0)
		a->buf = Qnil;
	return 0;
}

/*
 * Document-method: Kgio::PipeMethods#kgio_read
 *
 * call-seq:
 *
 *	socket.kgio_read(maxlen)  ->  buffer
 *	socket.kgio_read(maxlen, buffer)  ->  buffer
 *
 * Reads at most maxlen bytes from the stream socket.  Returns with a
 * newly allocated buffer, or may reuse an existing buffer.  This
 * calls the method identified by Kgio.wait_readable, or uses
 * the normal, thread-safe Ruby function to wait for readability.
 * This returns nil on EOF.
 *
 * This behaves like read(2) and IO#readpartial, NOT fread(3) or
 * IO#read which possess read-in-full behavior.
 */
static VALUE my_read(int io_wait, int argc, VALUE *argv, VALUE io)
{
	struct io_args a;
	long n;

	prepare_read(&a, argc, argv, io);
	set_nonblocking(a.fd);
retry:
	n = (long)read(a.fd, a.ptr, a.len);
	if (read_check(&a, n, "read", io_wait) != 0)
		goto retry;
	return a.buf;
}

static VALUE kgio_read(int argc, VALUE *argv, VALUE io)
{
	return my_read(1, argc, argv, io);
}

static VALUE kgio_tryread(int argc, VALUE *argv, VALUE io)
{
	return my_read(0, argc, argv, io);
}

#ifdef USE_MSG_DONTWAIT
static VALUE my_recv(int io_wait, int argc, VALUE *argv, VALUE io)
{
	struct io_args a;
	long n;

	prepare_read(&a, argc, argv, io);
retry:
	n = (long)recv(a.fd, a.ptr, a.len, MSG_DONTWAIT);
	if (read_check(&a, n, "recv", io_wait) != 0)
		goto retry;
	return a.buf;
}

static VALUE kgio_recv(int argc, VALUE *argv, VALUE io)
{
	return my_recv(1, argc, argv, io);
}

static VALUE kgio_tryrecv(int argc, VALUE *argv, VALUE io)
{
	return my_recv(0, argc, argv, io);
}
#else /* ! USE_MSG_DONTWAIT */
#  define kgio_recv kgio_read
#  define kgio_tryrecv kgio_tryread
#endif /* USE_MSG_DONTWAIT */

static void prepare_write(struct io_args *a, VALUE io, VALUE str)
{
	a->buf = (TYPE(str) == T_STRING) ? str : rb_obj_as_string(str);
	a->ptr = RSTRING_PTR(a->buf);
	a->len = RSTRING_LEN(a->buf);
	a->io = io;
	a->fd = my_fileno(io);
}

static int write_check(struct io_args *a, long n, const char *msg, int io_wait)
{
	if (a->len == n) {
		a->buf = Qnil;
	} else if (n == -1) {
		if (errno == EINTR)
			return -1;
		if (errno == EAGAIN) {
			if (io_wait) {
				wait_writable(a->io);
				return -1;
			} else {
				a->buf = mKgio_WaitWritable;
				return 0;
			}
		}
		rb_sys_fail(msg);
	} else {
		assert(n >= 0 && n < a->len && "write/send syscall broken?");
		if (io_wait) {
			a->ptr += n;
			a->len -= n;
			return -1;
		}
		a->buf = rb_str_new(a->ptr + n, a->len - n);
	}
	return 0;
}

static VALUE my_write(VALUE io, VALUE str, int io_wait)
{
	struct io_args a;
	long n;

	prepare_write(&a, io, str);
	set_nonblocking(a.fd);
retry:
	n = (long)write(a.fd, a.ptr, a.len);
	if (write_check(&a, n, "write", io_wait) != 0)
		goto retry;
	return a.buf;
}

/*
 * Returns true if the write was completed.
 *
 * Calls the method Kgio.wait_writable is not set
 */
static VALUE kgio_write(VALUE io, VALUE str)
{
	return my_write(io, str, 1);
}

/*
 * Returns a String containing the unwritten portion if there was a
 * partial write.  Will return Kgio::WaitReadable if EAGAIN is
 * encountered.
 *
 * Returns true if the write completed in full.
 */
static VALUE kgio_trywrite(VALUE io, VALUE str)
{
	return my_write(io, str, 0);
}

#ifdef USE_MSG_DONTWAIT
/*
 * This method behaves like Kgio::PipeMethods#kgio_write, except
 * it will use send(2) with the MSG_DONTWAIT flag on sockets to
 * avoid unnecessary calls to fcntl(2).
 */
static VALUE my_send(VALUE io, VALUE str, int io_wait)
{
	struct io_args a;
	long n;

	prepare_write(&a, io, str);
retry:
	n = (long)send(a.fd, a.ptr, a.len, MSG_DONTWAIT);
	if (write_check(&a, n, "send", io_wait) != 0)
		goto retry;
	return a.buf;
}

static VALUE kgio_send(VALUE io, VALUE str)
{
	return my_send(io, str, 1);
}

static VALUE kgio_trysend(VALUE io, VALUE str)
{
	return my_send(io, str, 0);
}
#else /* ! USE_MSG_DONTWAIT */
#  define kgio_send kgio_write
#  define kgio_trysend kgio_trywrite
#endif /* ! USE_MSG_DONTWAIT */

/*
 * call-seq:
 *
 *	Kgio.wait_readable = :method_name
 *
 * Sets a method for kgio_read to call when a read would block.
 * This is useful for non-blocking frameworks that use Fibers,
 * as the method referred to this may cause the current Fiber
 * to yield execution.
 *
 * A special value of nil will cause Ruby to wait using the
 * rb_io_wait_readable() function, giving kgio_read similar semantics to
 * IO#readpartial.
 */
static VALUE set_wait_rd(VALUE mod, VALUE sym)
{
	switch (TYPE(sym)) {
	case T_SYMBOL:
		io_wait_rd = SYM2ID(sym);
		return sym;
	case T_NIL:
		io_wait_rd = 0;
		return sym;
	}
	rb_raise(rb_eTypeError, "must be a symbol or nil");
	return sym;
}

static VALUE set_wait_wr(VALUE mod, VALUE sym)
{
	switch (TYPE(sym)) {
	case T_SYMBOL:
		io_wait_wr = SYM2ID(sym);
		return sym;
	case T_NIL:
		io_wait_wr = 0;
		return sym;
	}
	rb_raise(rb_eTypeError, "must be a symbol or nil");
	return sym;
}

static VALUE wait_wr(VALUE mod)
{
	return io_wait_wr ? ID2SYM(io_wait_wr) : Qnil;
}

static VALUE wait_rd(VALUE mod)
{
	return io_wait_rd ? ID2SYM(io_wait_rd) : Qnil;
}

static VALUE xaccept(void *ptr)
{
	struct accept_args *a = ptr;

	return (VALUE)accept4(a->fd, a->addr, a->addrlen, accept4_flags);
}

#ifdef HAVE_RB_THREAD_BLOCKING_REGION
#  include <time.h>
static int thread_accept(struct accept_args *a, int force_nonblock)
{
	if (force_nonblock)
		set_nonblocking(a->fd);
	return (int)rb_thread_blocking_region(xaccept, a, RUBY_UBF_IO, 0);
}

/*
 * Try to use a (real) blocking accept() since that can prevent
 * thundering herds under Linux:
 * http://www.citi.umich.edu/projects/linux-scalability/reports/accept.html
 *
 * So we periodically disable non-blocking, but not too frequently
 * because other processes may set non-blocking (especially during
 * a process upgrade) with Rainbows! concurrency model changes.
 */
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
	return sock_for_fd(cSocket, client);
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

static VALUE tcp_tryaccept(VALUE io)
{
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	VALUE rv = my_accept(io, (struct sockaddr *)&addr, &addrlen, 1);

	if (!NIL_P(rv))
		in_addr_set(rv, &addr);
	return rv;
}

static VALUE tcp_accept(VALUE io)
{
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	VALUE rv = my_accept(io, (struct sockaddr *)&addr, &addrlen, 0);

	in_addr_set(rv, &addr);
	return rv;
}

static VALUE unix_tryaccept(VALUE io)
{
	VALUE rv = my_accept(io, NULL, NULL, 1);

	if (!NIL_P(rv))
		rb_ivar_set(rv, iv_kgio_addr, localhost);
	return rv;
}

static VALUE unix_accept(VALUE io)
{
	VALUE rv = my_accept(io, NULL, NULL, 0);

	rb_ivar_set(rv, iv_kgio_addr, localhost);
	return rv;
}

static VALUE get_cloexec(VALUE mod)
{
	return (accept4_flags & A4_SOCK_CLOEXEC) ==
	    A4_SOCK_CLOEXEC ? Qtrue : Qfalse;
}

static VALUE get_nonblock(VALUE mod)
{
	return (accept4_flags & A4_SOCK_NONBLOCK) ==
	    A4_SOCK_NONBLOCK ? Qtrue : Qfalse;
}

static VALUE set_cloexec(VALUE mod, VALUE boolean)
{
	switch (TYPE(boolean)) {
	case T_TRUE:
		accept4_flags |= A4_SOCK_CLOEXEC;
		return boolean;
	case T_FALSE:
		accept4_flags &= ~A4_SOCK_CLOEXEC;
		return boolean;
	}
	rb_raise(rb_eTypeError, "not true or false");
	return Qnil;
}

static VALUE set_nonblock(VALUE mod, VALUE boolean)
{
	switch (TYPE(boolean)) {
	case T_TRUE:
		accept4_flags |= A4_SOCK_NONBLOCK;
		return boolean;
	case T_FALSE:
		accept4_flags &= ~A4_SOCK_NONBLOCK;
		return boolean;
	}
	rb_raise(rb_eTypeError, "not true or false");
	return Qnil;
}

static void close_fail(int fd, const char *msg)
{
	int saved_errno = errno;
	(void)close(fd);
	errno = saved_errno;
	rb_sys_fail(msg);
}

#ifdef SOCK_NONBLOCK
#  define MY_SOCK_STREAM (SOCK_STREAM|SOCK_NONBLOCK)
#else
#  define MY_SOCK_STREAM SOCK_STREAM
#endif /* ! SOCK_NONBLOCK */

static VALUE
my_connect(VALUE klass, int io_wait, int domain, void *addr, socklen_t addrlen)
{
	int fd = socket(domain, MY_SOCK_STREAM, 0);

	if (fd == -1) {
		switch (errno) {
		case EMFILE:
		case ENFILE:
#ifdef ENOBUFS
		case ENOBUFS:
#endif /* ENOBUFS */
			errno = 0;
			rb_gc();
			fd = socket(domain, MY_SOCK_STREAM, 0);
		}
		if (fd == -1)
			rb_sys_fail("socket");
	}

#ifndef SOCK_NONBLOCK
	if (fcntl(fd, F_SETFL, O_RDWR | O_NONBLOCK) == -1)
		close_fail(fd, "fcntl(F_SETFL, O_RDWR | O_NONBLOCK)");
#endif /* SOCK_NONBLOCK */

	if (connect(fd, addr, addrlen) == -1) {
		if (errno == EINPROGRESS) {
			VALUE io = sock_for_fd(klass, fd);

			if (io_wait) {
				errno = EAGAIN;
				wait_writable(io);
			}
			return io;
		}
		close_fail(fd, "connect");
	}
	return sock_for_fd(klass, fd);
}

static VALUE tcp_connect(VALUE klass, VALUE ip, VALUE port, int io_wait)
{
	struct sockaddr_in addr = { 0 };

	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)NUM2INT(port));

	switch (inet_pton(AF_INET, StringValuePtr(ip), &addr.sin_addr)) {
	case 1:
		return my_connect(klass, io_wait, PF_INET, &addr, sizeof(addr));
	case -1:
		rb_sys_fail("inet_pton");
	}
	rb_raise(rb_eArgError, "invalid address: %s", StringValuePtr(ip));

	return Qnil;
}

/*
 * call-seq:
 *
 *	Kgio::TCPSocket.new('127.0.0.1', 80) -> socket
 *
 * Creates a new Kgio::TCPSocket object and initiates a
 * non-blocking connection.  The caller should select/poll
 * on the socket for writability before attempting to write
 * or optimistically attempt a write and handle Kgio::WaitWritable
 * or Errno::EAGAIN.
 *
 * Unlike the TCPSocket.new in Ruby, this does NOT perform DNS
 * lookups (which is subject to a different set of timeouts and
 * best handled elsewhere).
 *
 * This is only intended as a convenience for testing,
 * Kgio::Socket.new (along with a cached/memoized addr argument)
 * is recommended for applications that repeatedly connect to
 * the same backend servers.
 */
static VALUE kgio_tcp_connect(VALUE klass, VALUE ip, VALUE port)
{
	return tcp_connect(klass, ip, port, 1);
}

static VALUE kgio_tcp_start(VALUE klass, VALUE ip, VALUE port)
{
	return tcp_connect(klass, ip, port, 0);
}

static VALUE unix_connect(VALUE klass, VALUE path, int io_wait)
{
	struct sockaddr_un addr = { 0 };
	long len;

	StringValue(path);
	len = RSTRING_LEN(path);
	if (sizeof(addr.sun_path) <= len)
		rb_raise(rb_eArgError,
		         "too long unix socket path (max: %dbytes)",
		         (int)sizeof(addr.sun_path)-1);

	memcpy(addr.sun_path, RSTRING_PTR(path), len);
	addr.sun_family = AF_UNIX;

	return my_connect(klass, io_wait, PF_UNIX, &addr, sizeof(addr));
}

/*
 * call-seq:
 *
 *	Kgio::UNIXSocket.new("/path/to/unix/socket") -> socket
 *
 * Creates a new Kgio::UNIXSocket object and initiates a
 * non-blocking connection.  The caller should select/poll
 * on the socket for writability before attempting to write
 * or optimistically attempt a write and handle Kgio::WaitWritable
 * or Errno::EAGAIN.
 *
 * This is only intended as a convenience for testing,
 * Kgio::Socket.new (along with a cached/memoized addr argument)
 * is recommended for applications that repeatedly connect to
 * the same backend servers.
 */
static VALUE kgio_unix_connect(VALUE klass, VALUE path)
{
	return unix_connect(klass, path, 1);
}

static VALUE kgio_unix_start(VALUE klass, VALUE path)
{
	return unix_connect(klass, path, 0);
}

static VALUE stream_connect(VALUE klass, VALUE addr, int io_wait)
{
	int domain;
	socklen_t addrlen;
	struct sockaddr *sockaddr;

	if (TYPE(addr) == T_STRING) {
		sockaddr = (struct sockaddr *)(RSTRING_PTR(addr));
		addrlen = (socklen_t)RSTRING_LEN(addr);
	} else {
		rb_raise(rb_eTypeError, "invalid address");
	}
	switch (((struct sockaddr_in *)(sockaddr))->sin_family) {
	case AF_UNIX: domain = PF_UNIX; break;
	case AF_INET: domain = PF_INET; break;
#ifdef AF_INET6 /* IPv6 support incomplete */
	case AF_INET6: domain = PF_INET6; break;
#endif /* AF_INET6 */
	default:
		rb_raise(rb_eArgError, "invalid address family");
	}

	return my_connect(klass, io_wait, domain, sockaddr, addrlen);
}

static VALUE kgio_connect(VALUE klass, VALUE addr)
{
	return stream_connect(klass, addr, 1);
}

static VALUE kgio_start(VALUE klass, VALUE addr)
{
	return stream_connect(klass, addr, 0);
}

/*
 * call-seq:
 *
 *	addr = Socket.pack_sockaddr_in(80, 'example.com')
 *	Kgio::Socket.new(addr) -> socket
 *
 *	addr = Socket.pack_sockaddr_un("/tmp/unix.sock")
 *	Kgio::Socket.new(addr) -> socket
 *
 * Generic connect method for addr generated by Socket.pack_sockaddr_in
 * or Socket.pack_sockaddr_un
 */


void Init_kgio_ext(void)
{
	VALUE mKgio = rb_define_module("Kgio");
	VALUE mPipeMethods, mSocketMethods;
	VALUE cUNIXServer, cTCPServer, cUNIXSocket, cTCPSocket;

	rb_require("socket");
	cSocket = rb_const_get(rb_cObject, rb_intern("Socket"));
	cSocket = rb_define_class_under(mKgio, "Socket", cSocket);

	localhost = rb_str_new2("127.0.0.1");
	rb_const_set(mKgio, rb_intern("LOCALHOST"), localhost);

	/*
	 * The kgio_read method will return this when waiting for
	 * a read is required.
	 */
	mKgio_WaitReadable = rb_define_module_under(mKgio, "WaitReadable");

	/*
	 * The kgio_write method will return this when waiting for
	 * a write is required.
	 */
	mKgio_WaitWritable = rb_define_module_under(mKgio, "WaitWritable");

	rb_define_singleton_method(mKgio, "wait_readable=", set_wait_rd, 1);
	rb_define_singleton_method(mKgio, "wait_writable=", set_wait_wr, 1);
	rb_define_singleton_method(mKgio, "wait_readable", wait_rd, 0);
	rb_define_singleton_method(mKgio, "wait_writable", wait_wr, 0);
	rb_define_singleton_method(mKgio, "accept_cloexec?", get_cloexec, 0);
	rb_define_singleton_method(mKgio, "accept_cloexec=", set_cloexec, 1);
	rb_define_singleton_method(mKgio, "accept_nonblock?", get_nonblock, 0);
	rb_define_singleton_method(mKgio, "accept_nonblock=", set_nonblock, 1);

	mPipeMethods = rb_define_module_under(mKgio, "PipeMethods");
	rb_define_method(mPipeMethods, "kgio_read", kgio_read, -1);
	rb_define_method(mPipeMethods, "kgio_write", kgio_write, 1);
	rb_define_method(mPipeMethods, "kgio_tryread", kgio_tryread, -1);
	rb_define_method(mPipeMethods, "kgio_trywrite", kgio_trywrite, 1);

	mSocketMethods = rb_define_module_under(mKgio, "SocketMethods");
	rb_define_method(mSocketMethods, "kgio_read", kgio_recv, -1);
	rb_define_method(mSocketMethods, "kgio_write", kgio_send, 1);
	rb_define_method(mSocketMethods, "kgio_tryread", kgio_tryrecv, -1);
	rb_define_method(mSocketMethods, "kgio_trywrite", kgio_trysend, 1);

	rb_define_attr(mSocketMethods, "kgio_addr", 1, 1);
	rb_include_module(cSocket, mSocketMethods);
	rb_define_singleton_method(cSocket, "new", kgio_connect, 1);
	rb_define_singleton_method(cSocket, "start", kgio_start, 1);

	cUNIXServer = rb_const_get(rb_cObject, rb_intern("UNIXServer"));
	cUNIXServer = rb_define_class_under(mKgio, "UNIXServer", cUNIXServer);
	rb_define_method(cUNIXServer, "kgio_tryaccept", unix_tryaccept, 0);
	rb_define_method(cUNIXServer, "kgio_accept", unix_accept, 0);

	cTCPServer = rb_const_get(rb_cObject, rb_intern("TCPServer"));
	cTCPServer = rb_define_class_under(mKgio, "TCPServer", cTCPServer);
	rb_define_method(cTCPServer, "kgio_tryaccept", tcp_tryaccept, 0);
	rb_define_method(cTCPServer, "kgio_accept", tcp_accept, 0);

	cTCPSocket = rb_const_get(rb_cObject, rb_intern("TCPSocket"));
	cTCPSocket = rb_define_class_under(mKgio, "TCPSocket", cTCPSocket);
	rb_include_module(cTCPSocket, mSocketMethods);
	rb_define_singleton_method(cTCPSocket, "new", kgio_tcp_connect, 2);
	rb_define_singleton_method(cTCPSocket, "start", kgio_tcp_start, 2);

	cUNIXSocket = rb_const_get(rb_cObject, rb_intern("UNIXSocket"));
	cUNIXSocket = rb_define_class_under(mKgio, "UNIXSocket", cUNIXSocket);
	rb_include_module(cUNIXSocket, mSocketMethods);
	rb_define_singleton_method(cUNIXSocket, "new", kgio_unix_connect, 1);
	rb_define_singleton_method(cUNIXSocket, "start", kgio_unix_start, 1);

	iv_kgio_addr = rb_intern("@kgio_addr");
	init_sock_for_fd();
}
