#include "kgio.h"

void Init_kgio_ext(void)
{
	VALUE mKgio = rb_const_get(rb_cObject, rb_intern("Kgio"));

	init_kgio_wait(mKgio);
	init_kgio_read_write(mKgio);
	init_kgio_connect(mKgio);
	init_kgio_accept(mKgio);
}
