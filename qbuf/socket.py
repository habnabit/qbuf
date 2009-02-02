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
        return self.sock.fileno()
    
    def send(self, data):
        return self.sock.send(data)
    
    def sendall(self, data):
        self.sock.sendall(data)
    
    def close(self):
        self.closed = True
        self.sock.close()
    
    def pump_buffer(self):
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
    def __init__(self, sock, buffer_size=4096,
            delimiter="\r\n", auto_pump=True):
        super(LineReceiver, self).__init__(self, sock, buffer_size)
        self.buffer.delimiter = delimiter
        self.auto_pump = auto_pump
    
    def __iter__(self):
        if self.auto_pump:
            if not self.pump_buffer():
                raise SocketClosedError
        return self.buffer
    
    def readline(self):
        if self.auto_pump:
            if not self.pump_buffer():
                return ''
        try:
            return self.buffer.popline()
        except ValueError:
            raise socket.error(errno.EAGAIN, 'no full line available')

class StatefulProtocol(_SocketWrapper):
    def __init__(self, sock, buffer_size=4096):
        super(StatefulProtocol, self).__init__(self, sock, buffer_size)
        self.next_func, self.waiting_on = self.get_initial_state()
    
    def update(self):
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
