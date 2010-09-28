require 'mkmf'
$CPPFLAGS << ' -D_GNU_SOURCE'

have_func('accept4', %w(sys/socket.h))
if have_header('ruby/io.h')
  have_struct_member("rb_io_t", "fd", "ruby/io.h")
  have_struct_member("rb_io_t", "mode", "ruby/io.h")
else
  rubyio = %w(ruby.h rubyio.h)
  rb_io_t = have_type("OpenFile", rubyio) ? "OpenFile" : "rb_io_t"
  have_struct_member(rb_io_t, "f", rubyio)
  have_struct_member(rb_io_t, "f2", rubyio)
  have_struct_member(rb_io_t, "mode", rubyio)
  have_func('rb_fdopen')
end
have_func('rb_io_ascii8bit_binmode')
have_func('rb_thread_blocking_region')
have_func('rb_str_set_len')

dir_config('kgio')
create_makefile('kgio_ext')
