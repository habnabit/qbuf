"""socket module support for qbuf.
"""

#from __future__ import absolute_import
from qbuf import BufferQueue, BufferUnderflow
import socket, errno

class SocketClosedError(Exception):
    pass

class _SocketWrapper(object):
    def __init__(self, sock, buffer_size=4096):
        self.buffer = BufferQueue()
        self.sock = sock
        self.buffer_size = buffer_size

    def fileno(self):
        """Returns the wrapped socket's fileno.
        """
        return self.sock.fileno()

    def send(self, data):
        """Calls send on the wrapped socket.
        """
        return self.sock.send(data)

    def sendall(self, data):
        """Calls sendall on the wrapped socket.
        """
        self.sock.sendall(data)

    def close(self):
        """Closes the wrapped socket.
        """
        self.closed = True
        self.sock.close()

    def pump_buffer(self):
        """Try to read from the wrapped socket into the buffer.

        Returns True if the socket is still open, and False otherwise.
        """
        try:
            data = self.sock.recv(self.buffer_size)
        except socket.error, e:
            if e[0] == errno.EGAIN:
                return True
            else:
                raise
        if not data:
            self.closed = True
            return False
        self.buffer.push(data)
        return True

class LineReceiver(_SocketWrapper):
    """A socket wrapper for line buffering.

    Iterating over a LineBuffer will yield each line available in the buffer.
    If the socket is closed, iterating over a LineBuffer will raise a
    SocketClosedError.
    """

    def __init__(self, sock, buffer_size=4096,
            delimiter="\r\n", auto_pump=True):
        """Wrap a socket for easier line buffering of incoming data.

        The buffer_size parameter indicates how much should be read from the
        socket at a time. If the auto_pump parameter is True, iterating over
        the LineReceiver and calling readline will automatically try to read
        new socket data into the buffer first.
        """
        super(LineReceiver, self).__init__(sock, buffer_size)
        self.buffer.delimiter = delimiter
        self.auto_pump = auto_pump

    def __iter__(self):
        if self.auto_pump:
            if not self.pump_buffer():
                raise SocketClosedError
        return self.buffer

    def readline(self):
        """Read a line out of the buffer.

        If the socket is closed, this function returns an empty string. If no
        line is available, socket.error is raised with an errno of EAGAIN.
        Otherwise, return the next line available in the buffer.
        """
        if self.auto_pump:
            if not self.pump_buffer():
                return ''
        try:
            return self.buffer.popline()
        except ValueError:
            raise socket.error(errno.EAGAIN, 'no full line available')

class StatefulProtocol(_SocketWrapper):
    """A socket wrapper similar to Twisted's StatefulProtocol.
    """
    def __init__(self, sock, buffer_size=4096):
        """Wrap a socket for stateful reads.

        The buffer_size parameter indicates how much should be read from the
        socket at a time.
        """
        super(StatefulProtocol, self).__init__(sock, buffer_size)
        self.next_func, self.waiting_on = self.get_initial_state()

    def update(self):
        """Pump the buffer and try to make as many callbacks as possible.

        If the socket is closed, a SocketClosedError is raised.
        """
        if not self.pump_buffer():
            raise SocketClosedError
        while self.buffer:
            try:
                data = self.buffer.pop(self.waiting_on)
            except BufferUnderflow:
                break
            else:
                result = self.next_func(data)
                if result:
                    self.next_func, self.waiting_on = result

    def get_initial_state(self):
        """Get the initial state for the StatefulProtocol.

        This function must return a tuple of (some_callable, bytes_to_read),
        where the callable will be called when that number of bytes has been
        read, with those same bytes as the only argument. That callable can
        return None to keep the same (callable, n_bytes) state, or return a
        new (callable, n_bytes) tuple.
        """
        pass
