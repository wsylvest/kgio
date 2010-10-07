#include "kgio.h"

static ID io_wait_rd, io_wait_wr;

void kgio_wait_readable(VALUE io, int fd)
{
	if (io_wait_rd) {
		(void)rb_funcall(io, io_wait_rd, 0, 0);
	} else {
		if (!rb_io_wait_readable(fd))
			rb_sys_fail("wait readable");
	}
}

void kgio_wait_writable(VALUE io, int fd)
{
	if (io_wait_wr) {
		(void)rb_funcall(io, io_wait_wr, 0, 0);
	} else {
		if (!rb_io_wait_writable(fd))
			rb_sys_fail("wait writable");
	}
}

/*
 * call-seq:
 *
 *	Kgio.wait_readable = :method_name
 *	Kgio.wait_readable = nil
 *
 * Sets a method for kgio_read to call when a read would block.
 * This is useful for non-blocking frameworks that use Fibers,
 * as the method referred to this may cause the current Fiber
 * to yield execution.
 *
 * A special value of nil will cause Ruby to wait using the
 * rb_io_wait_readable() function.
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

/*
 * call-seq:
 *
 *	Kgio.wait_writable = :method_name
 *	Kgio.wait_writable = nil
 *
 * Sets a method for kgio_write to call when a read would block.
 * This is useful for non-blocking frameworks that use Fibers,
 * as the method referred to this may cause the current Fiber
 * to yield execution.
 *
 * A special value of nil will cause Ruby to wait using the
 * rb_io_wait_writable() function.
 */
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

/*
 * call-seq:
 *
 *	Kgio.wait_writable	-> Symbol or nil
 *
 * Returns the symbolic method name of the method assigned to
 * call when EAGAIN is occurs on a Kgio::PipeMethods#kgio_write
 * or Kgio::SocketMethods#kgio_write call
 */
static VALUE wait_wr(VALUE mod)
{
	return io_wait_wr ? ID2SYM(io_wait_wr) : Qnil;
}

/*
 * call-seq:
 *
 *	Kgio.wait_readable	-> Symbol or nil
 *
 * Returns the symbolic method name of the method assigned to
 * call when EAGAIN is occurs on a Kgio::PipeMethods#kgio_read
 * or Kgio::SocketMethods#kgio_read call.
 */
static VALUE wait_rd(VALUE mod)
{
	return io_wait_rd ? ID2SYM(io_wait_rd) : Qnil;
}

void init_kgio_wait(void)
{
	VALUE mKgio = rb_define_module("Kgio");

	rb_define_singleton_method(mKgio, "wait_readable=", set_wait_rd, 1);
	rb_define_singleton_method(mKgio, "wait_writable=", set_wait_wr, 1);
	rb_define_singleton_method(mKgio, "wait_readable", wait_rd, 0);
	rb_define_singleton_method(mKgio, "wait_writable", wait_wr, 0);
}
