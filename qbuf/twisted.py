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
    each complete line without the delimiter on the end. The 'delimiter' 
    instance attribute is used for keeping track of the current delimiter.
    
    When the buffering mode is MODE_STATEFUL, a user-passed function is 
    called for every so many bytes received. If self.current_state is None,
    getInitialState is called to get the initial state. See the documentation
    on getInitialState.
    """
    mode = MODE_RAW
    initial_delimiter = '\r\n'
    current_state = _generator = _generator_state = None
    _closed = False
    
    def __init__(self):
        self._buffer = BufferQueue(self.initial_delimiter)
    
    def dataReceived(self, data):
        if self._closed: 
            return
        
        self._buffer.push(data)
        while self._buffer and not self._closed:
            if self.mode == MODE_RAW:
                self._rawDataReceived(self._buffer.pop())
            elif self.mode == MODE_DELIMITED:
                try:
                    line = self._buffer.popline()
                except ValueError:
                    break
                else:
                    self._lineReceived(line)
            elif self.mode == MODE_STATEFUL:
                if self._generator:
                    try:
                        chunk = self._buffer.pop(self._generator_state)
                    except BufferUnderflow:
                        break
                    else:
                        self._updateGenerator(chunk)
                else:
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
    
    def setMode(self, mode, extra='', flush=False, state=None, delimiter=None):
        """Change the buffering mode. 
        
        If 'extra' is provided, add that to the buffer. If 'flush' is True and 
        'extra' is provided, also flush the buffer as much as possible. If 
        'state' is not None, that value will be assigned to self.current_state 
        before anything else. If 'delimiter' is not None, the delimiter will
        be set before anything else.
        """
        self.mode = mode
        if state is not None:
            self.current_state = state
        if delimiter is not None:
            self.delimiter = delimiter
        if extra and flush:
            self.dataReceived(extra)
        elif extra:
            self._buffer.push(extra)
    
    def _get_delimiter(self):
        return self._buffer.delimiter
    
    def _set_delimiter(self, delimiter):
        self._buffer.delimiter = delimiter
    
    delimiter = property(_get_delimiter, _set_delimiter)
    
    def _rawDataReceived(self, data):
        if self._generator:
            self._updateGenerator(data)
        else:
            self.rawDataReceived(data)
    
    def rawDataReceived(self, data):
        """Called when the buffering mode is MODE_RAW and there is new data 
        available.
        """
        raise NotImplementedError
    
    def _lineReceived(self, line):
        if self._generator:
            self._updateGenerator(line)
        else:
            self.lineReceived(line)
    
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
    
    def startGenerator(self, gen, mode=None):
        """Use a generator in place of callbacks.
        
        A convenience function for use with any buffering mode. A generator 
        passed to this method will receive data as it comes in. In MODE_RAW
        and MODE_DELIMITED, the value yielded by the generator must be None. 
        In MODE_STATEFUL, the generator must yield the number of bytes to be 
        sent.
        
        The generator must change the MultiBufferer's state when it is done
        yielding values and before it returns; any StopIteration exceptions 
        are ignored. Otherwise, the MultiBufferer would overwrite the state 
        set by the generator. Calling startGenerator from a started generator
        will overwrite the previous generator but otherwise function as 
        expected.
        
        If 'mode' is passed, the buffering mode is changed before the 
        generator starts.
        """
        self._generator = gen
        if mode is not None:
            self.setMode(mode)
        # Since g.next() is the same as g.send(None), let's not duplicate the
        # setting of self._generator_state and catching the StopIteration.
        self._updateGenerator(None)
    
    def _updateGenerator(self, data):
        # Someone might call startGenerator from within a started generator,
        # so make sure that if we unset the generator, we're unsetting the 
        # generator that raised the StopIteration.
        g = self._generator
        try:
            self._generator_state = g.send(data)
        except StopIteration:
            if self._generator is g:
                self._generator = None
    
    def close(self, disconnect=True):
        """Stop buffering incoming data.
        
        This will clear the input buffer and stop adding new data to the 
        buffer. If 'disconnect' is True, this will also lose the connection on
        the transport.
        """
        self._buffer.clear()
        self._closed = True
        if disconnect:
            self.transport.loseConnection()

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
