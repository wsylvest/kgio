require 'test/unit'
require 'io/nonblock'
$-w = true
require 'kgio'

class TestKgioTCPServer < Test::Unit::TestCase

  def setup
    @host = ENV["TEST_HOST"] || '127.0.0.1'
    @srv = Kgio::TCPServer.new(@host, 0)
    @port = @srv.addr[1]
  end

  def teardown
    @srv.close unless @srv.closed?
    Kgio.accept_cloexec = true
    Kgio.accept_nonblock = false
  end

  def test_accept
    a = TCPSocket.new(@host, @port)
    b = @srv.kgio_accept
    assert_kind_of Kgio::Socket, b
    assert_equal @host, b.kgio_addr
  end

  def test_accept_nonblock
    @srv.nonblock = true
    assert_equal nil, @srv.kgio_accept
  end
end
