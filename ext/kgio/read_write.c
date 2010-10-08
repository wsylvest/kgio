#include "kgio.h"
static VALUE mKgio_WaitReadable, mKgio_WaitWritable;
static VALUE eErrno_EPIPE, eErrno_ECONNRESET;

/*
 * we know MSG_DONTWAIT works properly on all stream sockets under Linux
 * we can define this macro for other platforms as people care and
 * notice.
 */
#if defined(__linux__) && ! defined(USE_MSG_DONTWAIT)
#  define USE_MSG_DONTWAIT
#endif

NORETURN(static void raise_empty_bt(VALUE, const char *));
NORETURN(static void my_eof_error(void));
NORETURN(static void wr_sys_fail(const char *));

static void raise_empty_bt(VALUE err, const char *msg)
{
	VALUE exc = rb_exc_new2(err, msg);
	VALUE bt = rb_ary_new();

	rb_funcall(exc, rb_intern("set_backtrace"), 1, bt);
	rb_exc_raise(exc);
}

static void my_eof_error(void)
{
	raise_empty_bt(rb_eEOFError, "");
}

static void wr_sys_fail(const char *msg)
{
	switch (errno) {
	case EPIPE:
		errno = 0;
		raise_empty_bt(eErrno_EPIPE, msg);
	case ECONNRESET:
		errno = 0;
		raise_empty_bt(eErrno_ECONNRESET, msg);
	}
	rb_sys_fail(msg);
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
				kgio_wait_readable(a->io, a->fd);

				/* buf may be modified in other thread/fiber */
				rb_str_resize(a->buf, a->len);
				a->ptr = RSTRING_PTR(a->buf);
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

static VALUE my_read(int io_wait, int argc, VALUE *argv, VALUE io)
{
	struct io_args a;
	long n;

	prepare_read(&a, argc, argv, io);

	if (a.len > 0) {
		set_nonblocking(a.fd);
retry:
		n = (long)read(a.fd, a.ptr, a.len);
		if (read_check(&a, n, "read", io_wait) != 0)
			goto retry;
	}
	return a.buf;
}

/*
 * call-seq:
 *
 *	io.kgio_read(maxlen)           ->  buffer
 *	io.kgio_read(maxlen, buffer)   ->  buffer
 *
 * Reads at most maxlen bytes from the stream socket.  Returns with a
 * newly allocated buffer, or may reuse an existing buffer if supplied.
 *
 * Calls the method assigned to Kgio.wait_readable, or blocks in a
 * thread-safe manner for writability.
 *
 * Returns nil on EOF.
 *
 * This behaves like read(2) and IO#readpartial, NOT fread(3) or
 * IO#read which possess read-in-full behavior.
 */
static VALUE kgio_read(int argc, VALUE *argv, VALUE io)
{
	return my_read(1, argc, argv, io);
}

/*
 * Same as Kgio::PipeMethods#kgio_read, except EOFError is raised
 * on EOF without a backtrace
 */
static VALUE kgio_read_bang(int argc, VALUE *argv, VALUE io)
{
	VALUE rv = my_read(1, argc, argv, io);

	if (NIL_P(rv)) my_eof_error();
	return rv;
}

/*
 * call-seq:
 *
 *	io.kgio_tryread(maxlen)           ->  buffer
 *	io.kgio_tryread(maxlen, buffer)   ->  buffer
 *
 * Reads at most maxlen bytes from the stream socket.  Returns with a
 * newly allocated buffer, or may reuse an existing buffer if supplied.
 *
 * Returns nil on EOF.
 *
 * Returns Kgio::WaitReadable if EAGAIN is encountered.
 */
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

	if (a.len > 0) {
retry:
		n = (long)recv(a.fd, a.ptr, a.len, MSG_DONTWAIT);
		if (read_check(&a, n, "recv", io_wait) != 0)
			goto retry;
	}
	return a.buf;
}

/*
 * This method may be optimized on some systems (e.g. GNU/Linux) to use
 * MSG_DONTWAIT to avoid explicitly setting the O_NONBLOCK flag via fcntl.
 * Otherwise this is the same as Kgio::PipeMethods#kgio_read
 */
static VALUE kgio_recv(int argc, VALUE *argv, VALUE io)
{
	return my_recv(1, argc, argv, io);
}

/*
 * Same as Kgio::SocketMethods#kgio_read, except EOFError is raised
 * on EOF without a backtrace
 */
static VALUE kgio_recv_bang(int argc, VALUE *argv, VALUE io)
{
	VALUE rv = my_recv(1, argc, argv, io);

	if (NIL_P(rv)) my_eof_error();
	return rv;
}

/*
 * This method may be optimized on some systems (e.g. GNU/Linux) to use
 * MSG_DONTWAIT to avoid explicitly setting the O_NONBLOCK flag via fcntl.
 * Otherwise this is the same as Kgio::PipeMethods#kgio_tryread
 */
static VALUE kgio_tryrecv(int argc, VALUE *argv, VALUE io)
{
	return my_recv(0, argc, argv, io);
}
#else /* ! USE_MSG_DONTWAIT */
#  define kgio_recv kgio_read
#  define kgio_recv_bang kgio_read_bang
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
done:
		a->buf = Qnil;
	} else if (n == -1) {
		if (errno == EINTR)
			return -1;
		if (errno == EAGAIN) {
			long written = RSTRING_LEN(a->buf) - a->len;

			if (io_wait) {
				kgio_wait_writable(a->io, a->fd);

				/* buf may be modified in other thread/fiber */
				a->len = RSTRING_LEN(a->buf) - written;
				if (a->len <= 0)
					goto done;
				a->ptr = RSTRING_PTR(a->buf) + written;
				return -1;
			} else if (written > 0) {
				a->buf = rb_str_new(a->ptr, a->len);
			} else {
				a->buf = mKgio_WaitWritable;
			}
			return 0;
		}
		wr_sys_fail(msg);
	} else {
		assert(n >= 0 && n < a->len && "write/send syscall broken?");
		a->ptr += n;
		a->len -= n;
		return -1;
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
 * call-seq:
 *
 *	io.kgio_write(str)	-> nil
 *
 * Returns nil when the write completes.
 *
 * Calls the method Kgio.wait_writable if it is set.  Otherwise this
 * blocks in a thread-safe manner until all data is written or a
 * fatal error occurs.
 */
static VALUE kgio_write(VALUE io, VALUE str)
{
	return my_write(io, str, 1);
}

/*
 * call-seq:
 *
 *	io.kgio_trywrite(str)	-> nil or Kgio::WaitWritable
 *
 * Returns nil if the write was completed in full.
 *
 * Returns a String containing the unwritten portion if EAGAIN
 * was encountered, but some portion was successfully written.
 *
 * Returns Kgio::WaitWritable if EAGAIN is encountered and nothing
 * was written.
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

/*
 * This method may be optimized on some systems (e.g. GNU/Linux) to use
 * MSG_DONTWAIT to avoid explicitly setting the O_NONBLOCK flag via fcntl.
 * Otherwise this is the same as Kgio::PipeMethods#kgio_write
 */
static VALUE kgio_send(VALUE io, VALUE str)
{
	return my_send(io, str, 1);
}

/*
 * This method may be optimized on some systems (e.g. GNU/Linux) to use
 * MSG_DONTWAIT to avoid explicitly setting the O_NONBLOCK flag via fcntl.
 * Otherwise this is the same as Kgio::PipeMethods#kgio_trywrite
 */
static VALUE kgio_trysend(VALUE io, VALUE str)
{
	return my_send(io, str, 0);
}
#else /* ! USE_MSG_DONTWAIT */
#  define kgio_send kgio_write
#  define kgio_trysend kgio_trywrite
#endif /* ! USE_MSG_DONTWAIT */

void init_kgio_read_write(void)
{
	VALUE mPipeMethods, mSocketMethods;
	VALUE mKgio = rb_define_module("Kgio");

	mKgio_WaitReadable = rb_const_get(mKgio, rb_intern("WaitReadable"));
	mKgio_WaitWritable = rb_const_get(mKgio, rb_intern("WaitWritable"));

	/*
	 * Document-module: Kgio::PipeMethods
	 *
	 * This module may be used used to create classes that respond to
	 * various Kgio methods for reading and writing.  This is included
	 * in Kgio::Pipe by default.
	 */
	mPipeMethods = rb_define_module_under(mKgio, "PipeMethods");
	rb_define_method(mPipeMethods, "kgio_read", kgio_read, -1);
	rb_define_method(mPipeMethods, "kgio_read!", kgio_read_bang, -1);
	rb_define_method(mPipeMethods, "kgio_write", kgio_write, 1);
	rb_define_method(mPipeMethods, "kgio_tryread", kgio_tryread, -1);
	rb_define_method(mPipeMethods, "kgio_trywrite", kgio_trywrite, 1);

	/*
	 * Document-module: Kgio::SocketMethods
	 *
	 * This method behaves like Kgio::PipeMethods, but contains
	 * optimizations for sockets on certain operating systems
	 * (e.g. GNU/Linux).
	 */
	mSocketMethods = rb_define_module_under(mKgio, "SocketMethods");
	rb_define_method(mSocketMethods, "kgio_read", kgio_recv, -1);
	rb_define_method(mSocketMethods, "kgio_read!", kgio_recv_bang, -1);
	rb_define_method(mSocketMethods, "kgio_write", kgio_send, 1);
	rb_define_method(mSocketMethods, "kgio_tryread", kgio_tryrecv, -1);
	rb_define_method(mSocketMethods, "kgio_trywrite", kgio_trysend, 1);

	/*
	 * Returns the client IPv4 address of the socket in dotted quad
	 * form as a string.  This is always the value of the
	 * Kgio::LOCALHOST constant for UNIX domain sockets.
	 */
	rb_define_attr(mSocketMethods, "kgio_addr", 1, 1);

	eErrno_EPIPE = rb_const_get(rb_mErrno, rb_intern("EPIPE"));
	eErrno_ECONNRESET = rb_const_get(rb_mErrno, rb_intern("ECONNRESET"));
}
