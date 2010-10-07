#ifndef KGIO_H
#define KGIO_H

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

#include "missing/ancient_ruby.h"
#include "nonblock.h"
#include "my_fileno.h"

struct io_args {
	VALUE io;
	VALUE buf;
	char *ptr;
	long len;
	int fd;
};

void init_kgio_wait(void);
void init_kgio_read_write(void);
void init_kgio_accept(void);
void init_kgio_connect(void);

void kgio_wait_writable(VALUE io, int fd);
void kgio_wait_readable(VALUE io, int fd);

#endif /* KGIO_H */
