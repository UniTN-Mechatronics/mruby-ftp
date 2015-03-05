#*************************************************************************#
#                                                                         #
# ftp.rb - mruby gem provoding ftp access                                 #
# Copyright (C) 2015 Paolo Bosetti and Matteo Ragni,                      #
# paolo[dot]bosetti[at]unitn.it and matteo[dot]ragni[at]unitn.it          #
# Department of Industrial Engineering, University of Trento              #
#                                                                         #
# This library is free software.  You can redistribute it and/or          #
# modify it under the terms of the GNU GENERAL PUBLIC LICENSE 2.0.        #
#                                                                         #
# This library is distributed in the hope that it will be useful,         #
# but WITHOUT ANY WARRANTY; without even the implied warranty of          #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           #
# Artistic License 2.0 for more details.                                  #
#                                                                         #
# See the file LICENSE                                                    #
#                                                                         #
#*************************************************************************#

class FTP
  # Class constant definitions
  # - State enumeration
  STATE = {
    :to_init   => -1,
    :closed    =>  0,
    :connected =>  1,
    :logged_in =>  2
  }
  # - Mode
  XFER = {
    :text   => 0,
    :binary => 1
  }
    
  def self.open(hostname, user="anonymous", pwd='')
    if block_given? then
      ftp = self.new(hostname, user, pwd)
      ftp.open
      yield ftp
      ftp.close
    else
      return self.new(hostname, user, pwd).open
    end
  end
  
  attr_reader :hostname, :user
  def initialize(hostname, user="anonymous", pwd='')
    @hostname = hostname
    @user     = user
    @pwd      = pwd
    #@data     = nil
    #self.data_init
  end
  
  def closed?
    (state == STATE[:closed] ? true : false)
  end
  
  def getbinaryfile(remote, local)
    get(remote, local, XFER[:binary])
  end
  
  def gettextfile(remote, local)
    get(remote, local, XFER[:text])
  end
  
  def putbinaryfile(local, remote)
    put(local, remote, XFER[:binary])
  end
  
  def puttextfile(local, remote)
    put(local, remote, XFER[:text])
  end
  def inspect
    "#<#{self.class}:0x#{self.hash.abs.to_s(16)} @user=#{@user || 'nil'}, @hostname=#{@hostname}, state=#{self.state}>"
  end
  
  alias :quit :close
  alias :connect :open
  alias :list :dir
  alias :ls :nlst
  alias :cd :chdir
end
