"""Twisted support for qbuf.

MODE_RAW, MODE_DELIMITED, and MODE_STATEFUL are module-level constants for 
changing the buffering mode of MultiBufferer.
"""

from __future__ import absolute_import
from qbuf import BufferQueue, BufferUnderflow
from twisted.internet import protocol
import struct

MODE_RAW, MODE_DELIMITED, MODE_STATEFUL = xrange(3)

class MultiBufferer(protocol.Protocol):
    """A replacement for a couple of buffering classes provided by twisted. 
    
    Without subclassing, it can work the same way as LineReceiver and
    StatefulProtocol.
    
    When the buffering mode is MODE_RAW, rawDataReceived is called with all of
    the data that comes in.
    
    When the buffering mode is MODE_DELIMITED, lineReceived is called with 
    each complete line without the delimiter on the end.
    
    When the buffering mode is MODE_STATEFUL, a user-passed function is 
    called for every so many bytes received. If self.current_state is None,
    getInitialState is called to get the initial state. See the documentation
    on getInitialState.
    """
    mode = MODE_RAW
    initial_delimiter = '\r\n'
    current_state = _generator = None
    
    def __init__(self):
        self._buffer = BufferQueue(self.initial_delimiter)
    
    def dataReceived(self, data):
        self._buffer.push(data)
        while self._buffer:
            if self.mode == MODE_RAW:
                self.rawDataReceived(self._buffer.pop())
            elif self.mode == MODE_DELIMITED:
                try:
                    line = self._buffer.popline()
                except ValueError:
                    break
                else:
                    self.lineReceived(line)
            elif self.mode == MODE_STATEFUL:
                if self.current_state is None:
                    self.current_state = self.getInitialState()
                try:
                    chunk = self._buffer.pop(self.current_state[1])
                except BufferUnderflow:
                    break
                else:
                    result = self.current_state[0](chunk)
                    if result:
                        self.current_state = result
    
    def setMode(self, mode, extra='', flush=False, state=None):
        """Change the buffering mode. 
        
        If 'extra' is provided, add that to the buffer. If 'flush' is True and 
        'extra' is provided, also flush the buffer as much as possible. If 
        'state' is not None, assigns that value to self.current_state before 
        anything else.
        """
        self.mode = mode
        if state is not None:
            self.current_state = state
        if extra and flush:
            self.dataReceived(extra)
        elif extra:
            self._buffer.push(extra)
    
    def rawDataReceived(self, data):
        """Called when the buffering mode is MODE_RAW and there is new data 
        available.
        """
        raise NotImplementedError
    
    def lineReceived(self, line):
        """Called when the buffering mode is MODE_DELIMITED and there is a new 
        line available.
        """
        raise NotImplementedError
    
    def getInitialState(self):
        """Called when there is no current state for MODE_STATEFUL. 
        
        This function must return a tuple of (some_callable, bytes_to_read), 
        where the callable will be called when that number of bytes has been 
        read, with those same bytes as the only argument. That callable can 
        return None to keep the same (callable, n_bytes) state, or return a 
        new (callable, n_bytes) tuple.
        """
        raise NotImplementedError
    
    def startGenerator(self, gen):
        """Use a generator in place of MODE_STATEFUL callbacks.
        
        A convenience function for use with MODE_STATEFUL. A generator passed
        to this method should yield the number of bytes to read, and then will
        be sent those bytes when they become available.
        
        The generator must change the MultiBufferer's state when it is done
        yielding values and before it returns; any StopIteration exceptions 
        are ignored. Otherwise, the MultiBufferer would overwrite the state 
        set by the generator.
        """
        self._generator = gen
        size = gen.next()
        self.setMode(MODE_STATEFUL, state=(self._updateGenerator, size))
    
    def _updateGenerator(self, data):
        try:
            size = self._generator.send(data)
        except StopIteration:
            self._generator = None
        else:
            return self._updateGenerator, size

class IntNStringReceiver(MultiBufferer):
    """This class is identical to the IntNStringReceiver provided by Twisted,
    implemented using MultiBufferer as a demonstration of how MODE_STATEFUL
    works.
    """
    mode = MODE_STATEFUL
    
    def getInitialState(self):
        return self.receiveLength, self.prefixLength
    
    def receiveLength(self, data):
        length, = struct.unpack(self.structFormat, data)
        return self.receiveString, length
    
    def receiveString(self, string):
        self.stringReceived(string)
        return self.receiveLength, self.prefixLength
    
    def sendString(self, data):
        self.transport.write(struct.pack(self.structFormat, len(data)) + data)
    
    def stringReceived(self, string):
        raise NotImplementedError

class Int32StringReceiver(IntNStringReceiver):
    """This class is an implementation of the abstract IntNStringReceiver
    class, only provided as a demonstration of MODE_STATEFUL.
    """
    structFormat = '!I'
    prefixLength = struct.calcsize(structFormat)
