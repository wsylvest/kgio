require 'test/unit'
require 'io/nonblock'
$-w = true
require 'kgio'
require 'tempfile'

class TestKgioUNIXServer < Test::Unit::TestCase

  def setup
    tmp = Tempfile.new('kgio_unix')
    @path = tmp.path
    File.unlink(@path)
    tmp.close rescue nil
    @srv = Kgio::UNIXServer.new(@path)
  end

  def teardown
    @srv.close unless @srv.closed?
    File.unlink(@path)
    Kgio.accept_cloexec = true
  end

  def test_accept
    a = UNIXSocket.new(@path)
    b = @srv.kgio_accept
    assert_kind_of Kgio::Socket, b
    assert_equal "127.0.0.1", b.kgio_addr
  end

  def test_accept_nonblock
    @srv.nonblock = true
    assert_equal nil, @srv.kgio_accept
  end
end
