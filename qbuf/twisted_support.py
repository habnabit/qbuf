"""Twisted support for qbuf.

MODE_RAW, MODE_DELIMITED, and MODE_STATEFUL are module-level constants for
changing the buffering mode of MultiBufferer.
"""

#from __future__ import absolute_import
from qbuf import BufferQueue, BufferUnderflow
from twisted.internet import protocol, defer
import collections
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

    MultiBufferers can also return Deferreds that are fired when a certain
    amount of data has been sent over the wire. This is intended for use with
    twisted.internet.defer.inlineCallbacks.
    """
    mode = MODE_RAW
    initial_delimiter = '\r\n'
    current_state = None
    _closed = False

    def __init__(self):
        self._buffer = BufferQueue(self.initial_delimiter)
        self._callbacks = collections.deque()

    def read(self, size=None):
        """Wait for some data to be received.

        If 'size' is provided, wait for that many bytes to be received.
        Otherwise, wait for the next chunk of incoming data, regardless of the
        size. Returns a Deferred that will be fired with the received data.
        """
        d = defer.Deferred()
        if size is None:
            self._callbacks.append((d, MODE_RAW, None))
        else:
            self._callbacks.append((d, MODE_STATEFUL, size))
        return d

    def unpack(self, fmt):
        """Wait for some struct to be received.

        This method takes a struct format as understood by the 'struct' module
        and waits for enough data to be received to unpack it. Returns a
        Deferred that will be fired with the unpacked struct.
        """
        def _transform(data):
            return struct.unpack(fmt, data)
        d = self.read(struct.calcsize(fmt))
        d.addCallback(_transform)
        return d

    def readline(self, delimiter=None):
        """Wait for a line to be received.

        Wait for a line of data to be received. If 'delimiter' is provided, the
        underlying BufferQueue's delimiter will be set to whatever is provided
        when the MultiBufferer is ready to receive that line. Returns a
        Deferred that will be fired with the received line, without delimiter.
        """
        d = defer.Deferred()
        self._callbacks.append((d, MODE_DELIMITED, delimiter))
        return d

    def write(self, data):
        """Send some data over the wire.

        This method merely forwards the data to the underlying transport,
        provided to parallel the read/readline methods.
        """
        self.transport.write(data)

    def _updateCallbacks(self, data):
        d, _, _ = self._callbacks.popleft()
        d.callback(data)

    def dataReceived(self, data):
        if self._closed:
            return

        self._buffer.push(data)
        while self._buffer and not self._closed:
            if self._callbacks:
                _, mode, extra = self._callbacks[0]
            else:
                mode, extra = self.mode, None
            if mode == MODE_RAW:
                self._rawDataReceived(self._buffer.pop())
            elif mode == MODE_DELIMITED:
                try:
                    line = self._buffer.popline(extra)
                except ValueError:
                    break
                else:
                    self._lineReceived(line)
            elif mode == MODE_STATEFUL:
                if self._callbacks:
                    try:
                        chunk = self._buffer.pop(extra)
                    except BufferUnderflow:
                        break
                    else:
                        self._updateCallbacks(chunk)
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
        if self._callbacks:
            self._updateCallbacks(data)
        else:
            self.rawDataReceived(data)

    def rawDataReceived(self, data):
        """Called when the buffering mode is MODE_RAW and there is new data
        available.
        """
        raise NotImplementedError

    def _lineReceived(self, line):
        if self._callbacks:
            self._updateCallbacks(line)
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

    def connectionLost(self, reason):
        self.close(False)
        for d, _, _ in self._callbacks:
            d.errback(reason)

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
