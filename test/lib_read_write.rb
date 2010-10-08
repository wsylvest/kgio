# -*- encoding: binary -*-
require 'test/unit'
require 'io/nonblock'
require 'digest/sha1'
$-w = true
require 'kgio'

module LibReadWriteTest
  RANDOM_BLOB = File.open("/dev/urandom") { |fp| fp.read(10 * 1024 * 1024) }

  def teardown
    assert_nothing_raised do
      @rd.close unless @rd.closed?
      @wr.close unless @wr.closed?
    end
    assert_nothing_raised do
      Kgio.wait_readable = Kgio.wait_writable = nil
    end
  end

  def test_read_zero
    assert_equal "", @rd.kgio_read(0)
    buf = "foo"
    assert_equal buf.object_id, @rd.kgio_read(0, buf).object_id
    assert_equal "", buf
  end

  def test_tryread_zero
    assert_equal "", @rd.kgio_tryread(0)
    buf = "foo"
    assert_equal buf.object_id, @rd.kgio_tryread(0, buf).object_id
    assert_equal "", buf
  end

  def test_read_eof
    @wr.close
    assert_nil @rd.kgio_read(5)
  end

  def test_read_bang_eof
    @wr.close
    begin
      @rd.kgio_read!(5)
      assert false, "should never get here (line:#{__LINE__})"
    rescue EOFError => e
      assert_equal [], e.backtrace
    end
  end

  def test_tryread_eof
    @wr.close
    assert_nil @rd.kgio_tryread(5)
  end

  def test_write_closed
    @rd.close
    begin
      loop { @wr.kgio_write "HI" }
    rescue Errno::EPIPE, Errno::ECONNRESET => e
      assert_equal [], e.backtrace
      return
    end
    assert false, "should never get here (line:#{__LINE__})"
  end

  def test_trywrite_closed
    @rd.close
    begin
      loop { @wr.kgio_trywrite "HI" }
    rescue Errno::EPIPE, Errno::ECONNRESET => e
      assert_equal [], e.backtrace
      return
    end
    assert false, "should never get here (line:#{__LINE__})"
  end

  def test_trywrite_full
    buf = "\302\251" * 1024 * 1024
    buf2 = ""
    dig = Digest::SHA1.new
    t = Thread.new do
      sleep 1
      nr = 0
      begin
        dig.update(@rd.readpartial(4096, buf2))
        nr += buf2.size
      rescue EOFError
        break
      rescue => e
      end while true
      dig.hexdigest
    end
    50.times do
      wr = buf
      begin
        rv = @wr.kgio_trywrite(wr)
        case rv
        when String
          wr = rv
        when Kgio::WaitReadable
          assert false, "should never get here line=#{__LINE__}"
        when Kgio::WaitWritable
          IO.select(nil, [ @wr ])
        else
          wr = false
        end
      end while wr
    end
    @wr.close
    t.join
    assert_equal '8ff79d8115f9fe38d18be858c66aa08a1cc27a66', t.value
  end

  def test_write_conv
    assert_equal nil, @wr.kgio_write(10)
    assert_equal "10", @rd.kgio_read(2)
  end

  def test_trywrite_conv
    assert_equal nil, @wr.kgio_trywrite(10)
    assert_equal "10", @rd.kgio_tryread(2)
  end

  def test_tryread_empty
    assert_equal Kgio::WaitReadable, @rd.kgio_tryread(1)
  end

  def test_read_too_much
    assert_equal nil, @wr.kgio_write("hi")
    assert_equal "hi", @rd.kgio_read(4)
  end

  def test_tryread_too_much
    assert_equal nil, @wr.kgio_trywrite("hi")
    assert_equal "hi", @rd.kgio_tryread(4)
  end

  def test_read_short
    assert_equal nil, @wr.kgio_write("hi")
    assert_equal "h", @rd.kgio_read(1)
    assert_equal "i", @rd.kgio_read(1)
  end

  def test_tryread_short
    assert_equal nil, @wr.kgio_trywrite("hi")
    assert_equal "h", @rd.kgio_tryread(1)
    assert_equal "i", @rd.kgio_tryread(1)
  end

  def test_read_extra_buf
    tmp = ""
    tmp_object_id = tmp.object_id
    assert_equal nil, @wr.kgio_write("hi")
    rv = @rd.kgio_read(2, tmp)
    assert_equal "hi", rv
    assert_equal rv.object_id, tmp.object_id
    assert_equal tmp_object_id, rv.object_id
  end

  def test_trywrite_return_wait_writable
    tmp = []
    tmp << @wr.kgio_trywrite("HI") until tmp[-1] == Kgio::WaitWritable
    assert Kgio::WaitWritable === tmp[-1]
    assert(!(Kgio::WaitReadable === tmp[-1]))
    assert_equal Kgio::WaitWritable, tmp.pop
    assert tmp.size > 0
    penultimate = tmp.pop
    assert(penultimate == "I" || penultimate == nil)
    assert tmp.size > 0
    tmp.each { |count| assert_equal nil, count }
  end

  def test_tryread_extra_buf_eagain_clears_buffer
    tmp = "hello world"
    rv = @rd.kgio_tryread(2, tmp)
    assert_equal Kgio::WaitReadable, rv
    assert_equal "", tmp
  end

  def test_tryread_extra_buf_eof_clears_buffer
    tmp = "hello world"
    @wr.close
    assert_nil @rd.kgio_tryread(2, tmp)
    assert_equal "", tmp
  end

  def test_monster_trywrite
    buf = RANDOM_BLOB.dup
    rv = @wr.kgio_trywrite(buf)
    assert_kind_of String, rv
    assert rv.size < buf.size
    @rd.nonblock = false
    assert_equal(buf, @rd.read(buf.size - rv.size) + rv)
  end

  def test_monster_write
    buf = RANDOM_BLOB.dup
    thr = Thread.new { @wr.kgio_write(buf) }
    @rd.nonblock = false
    readed = @rd.read(buf.size)
    thr.join
    assert_nil thr.value
    assert_equal buf, readed
  end

  def test_monster_write_wait_writable
    @wr.instance_variable_set :@nr, 0
    def @wr.wait_writable
      @nr += 1
      IO.select(nil, [self])
    end
    Kgio.wait_writable = :wait_writable
    buf = "." * 1024 * 1024 * 10
    thr = Thread.new { @wr.kgio_write(buf) }
    readed = @rd.read(buf.size)
    thr.join
    assert_nil thr.value
    assert_equal buf, readed
    assert @wr.instance_variable_get(:@nr) > 0
  end

  def test_wait_readable_ruby_default
    assert_nothing_raised { Kgio.wait_readable = nil }
    elapsed = 0
    foo = nil
    t0 = Time.now
    thr = Thread.new { sleep 1; @wr.write "HELLO" }
    assert_nothing_raised do
      foo = @rd.kgio_read(5)
      elapsed = Time.now - t0
    end
    assert elapsed >= 1.0, "elapsed: #{elapsed}"
    assert_equal "HELLO", foo
    thr.join
    assert_equal 5, thr.value
  end

  def test_wait_writable_ruby_default
    buf = "." * 512
    nr = 0
    begin
      nr += @wr.write_nonblock(buf)
    rescue Errno::EAGAIN
      break
    end while true
    assert_nothing_raised { Kgio.wait_writable = nil }
    elapsed = 0
    foo = nil
    t0 = Time.now
    thr = Thread.new { sleep 1; @rd.readpartial(nr) }
    assert_nothing_raised do
      foo = @wr.kgio_write("HELLO")
      elapsed = Time.now - t0
    end
    assert_nil foo
    if @wr.stat.pipe?
      assert elapsed >= 1.0, "elapsed: #{elapsed}"
    end
    assert(String === foo || foo == nil)
    assert_kind_of String, thr.value
  end

  def test_wait_readable_method
    def @rd.moo
      defined?(@z) ? raise(RuntimeError, "Hello") : @z = "HI"
    end
    assert_nothing_raised { Kgio.wait_readable = :moo }
    foo = nil
    begin
      foo = @rd.kgio_read(5)
      assert false
    rescue RuntimeError => e
      assert_equal("Hello", e.message)
    end
    assert_equal "HI", @rd.instance_variable_get(:@z)
    assert_nil foo
  end

  def test_tryread_wait_readable_method
    def @rd.moo
      raise "Hello"
    end
    assert_nothing_raised { Kgio.wait_readable = :moo }
    assert_equal Kgio::WaitReadable, @rd.kgio_tryread(5)
  end

  def test_trywrite_wait_readable_method
    def @wr.moo
      raise "Hello"
    end
    assert_nothing_raised { Kgio.wait_writable = :moo }
    tmp = []
    buf = "." * 1024
    10000.times { tmp << @wr.kgio_trywrite(buf) }
    assert_equal Kgio::WaitWritable, tmp.pop
  end

  def test_wait_writable_method
    def @wr.moo
      defined?(@z) ? raise(RuntimeError, "Hello") : @z = "HI"
    end
    assert_nothing_raised { Kgio.wait_writable = :moo }
    n = []
    begin
      loop { n << @wr.kgio_write("HIHIHIHIHIHI") }
      assert false
    rescue RuntimeError => e
      assert_equal("Hello", e.message)
    end
    assert n.size > 0
    assert_equal "HI", @wr.instance_variable_get(:@z)
  end
end
