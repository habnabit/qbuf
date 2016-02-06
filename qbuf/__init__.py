from qbuf._python import PythonBufferQueue

try:
    from qbuf._qbuf import BufferQueue, BufferUnderflow
except ImportError:
    from qbuf._python import BufferUnderflow


__version__ = '0.9.4'
__all__ = 'BufferQueue', 'BufferUnderflow', 'PythonBufferQueue'
