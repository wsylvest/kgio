require 'kgio_ext'

# use Kgio::Pipe.popen and Kgio::Pipe.new instead of IO.popen
# and IO.pipe to get PipeMethods#kgio_read and PipeMethod#kgio_write
# methods.
class Kgio::Pipe < IO
  include Kgio::PipeMethods
  class << self

    # call-seq:
    #
    #   rd, wr = Kgio::Pipe.new
    #
    # This creates a new pipe(7) with Kgio::Pipe objects that respond
    # to PipeMethods#kgio_read and PipeMethod#kgio_write
    alias new pipe
  end
end
