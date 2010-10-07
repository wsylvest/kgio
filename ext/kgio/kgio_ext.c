#include "kgio.h"

void Init_kgio_ext(void)
{
	init_kgio_wait();
	init_kgio_read_write();
	init_kgio_connect();
	init_kgio_accept();
}
