from qbuf import BufferQueue, BufferUnderflow
from twisted.internet import protocol
import struct

MODE_RAW, MODE_DELIMITED, MODE_STATEFUL = xrange(3)

class MultiBufferer(protocol.Protocol):
    mode = MODE_RAW
    initial_delimiter = '\r\n'
    current_state = None
    
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
        self.mode = mode
        if state is not None:
            self.state = state
        if extra and flush:
            self.dataReceived(extra)
        elif extra:
            self._buffer.push(extra)
    
    def rawDataReceived(self, data):
        raise NotImplementedError
    
    def lineReceived(self, line):
        raise NotImplementedError
    
    def getInitialState(self):
        raise NotImplementedError

class IntNStringReceiver(MultiBufferer):
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
    structFormat = '!I'
    prefixLength = struct.calcsize(structFormat)
