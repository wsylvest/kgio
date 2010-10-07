#include "kgio.h"
#include "sock_for_fd.h"

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
				kgio_wait_writable(io, fd);
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
 * non-blocking connection.
 *
 * This may block and call any method assigned to Kgio.wait_writable.
 *
 * Unlike the TCPSocket.new in Ruby, this does NOT perform DNS
 * lookups (which is subject to a different set of timeouts and
 * best handled elsewhere).
 */
static VALUE kgio_tcp_connect(VALUE klass, VALUE ip, VALUE port)
{
	return tcp_connect(klass, ip, port, 1);
}

/*
 * call-seq:
 *
 *	Kgio::TCPSocket.start('127.0.0.1', 80) -> socket
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
 */
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
 * non-blocking connection.
 *
 * This may block and call any method assigned to Kgio.wait_writable.
 */
static VALUE kgio_unix_connect(VALUE klass, VALUE path)
{
	return unix_connect(klass, path, 1);
}

/*
 * call-seq:
 *
 *	Kgio::UNIXSocket.start("/path/to/unix/socket") -> socket
 *
 * Creates a new Kgio::UNIXSocket object and initiates a
 * non-blocking connection.  The caller should select/poll
 * on the socket for writability before attempting to write
 * or optimistically attempt a write and handle Kgio::WaitWritable
 * or Errno::EAGAIN.
 */
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

/* call-seq:
 *
 *      addr = Socket.pack_sockaddr_in(80, 'example.com')
 *	Kgio::Socket.connect(addr) -> socket
 *
 *      addr = Socket.pack_sockaddr_un("/path/to/unix/socket")
 *	Kgio::Socket.connect(addr) -> socket
 *
 * Creates a generic Kgio::Socket object and initiates a
 * non-blocking connection.
 *
 * This may block and call any method assigned to Kgio.wait_writable.
 */
static VALUE kgio_connect(VALUE klass, VALUE addr)
{
	return stream_connect(klass, addr, 1);
}

/* call-seq:
 *
 *      addr = Socket.pack_sockaddr_in(80, 'example.com')
 *	Kgio::Socket.start(addr) -> socket
 *
 *      addr = Socket.pack_sockaddr_un("/path/to/unix/socket")
 *	Kgio::Socket.start(addr) -> socket
 *
 * Creates a generic Kgio::Socket object and initiates a
 * non-blocking connection.  The caller should select/poll
 * on the socket for writability before attempting to write
 * or optimistically attempt a write and handle Kgio::WaitWritable
 * or Errno::EAGAIN.
 */
static VALUE kgio_start(VALUE klass, VALUE addr)
{
	return stream_connect(klass, addr, 0);
}

void init_kgio_connect(void)
{
	VALUE mKgio = rb_define_module("Kgio");
	VALUE cSocket = rb_const_get(rb_cObject, rb_intern("Socket"));
	VALUE mSocketMethods = rb_const_get(mKgio, rb_intern("SocketMethods"));
	VALUE cKgio_Socket, cTCPSocket, cUNIXSocket;

	/*
	 * Document-class: Kgio::Socket
	 *
	 * A generic socket class with Kgio::SocketMethods included.
	 * This is returned by all Kgio methods that accept(2) a connected
	 * stream socket.
	 */
	cKgio_Socket = rb_define_class_under(mKgio, "Socket", cSocket);
	rb_include_module(cKgio_Socket, mSocketMethods);
	rb_define_singleton_method(cKgio_Socket, "new", kgio_connect, 1);
	rb_define_singleton_method(cKgio_Socket, "start", kgio_start, 1);

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
	init_sock_for_fd();
}
