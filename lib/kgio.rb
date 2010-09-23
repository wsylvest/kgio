require 'kgio_ext'

# use Kgio::Pipe.popen and Kgio::Pipe.new instead of IO.popen
# and IO.pipe to get kgio_read and kgio_write methods.
class Kgio::Pipe < IO
  include Kgio::PipeMethods
  class << self
    alias new pipe
  end
end
