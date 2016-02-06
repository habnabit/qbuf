import collections
import struct

try:
    from qbuf._qbuf import BufferUnderflow
except ImportError:
    class BufferUnderflow(Exception):
        pass


class PythonBufferQueue(object):
    def __init__(self, delimiter=None):
        self.delimiter = delimiter
        self._buffer = collections.deque()
        self._offset = 0
        self._tot_length = 0

    def __len__(self):
        return self._tot_length

    def push(self, string):
        self._tot_length += len(string)
        self._buffer.append(string)

    def push_many(self, iterable):
        for x in iterable:
            self.push(x)

    def _advance_buffer(self):
        self._offset = 0
        return self._buffer.popleft()

    def pop(self, length=None, underflow=True, as_view=False):
        if length is None:
            length = self._tot_length
        elif length < 0:
            raise ValueError()
        elif length > self._tot_length:
            if underflow:
                raise BufferUnderflow()
            else:
                length = self._tot_length

        if length == 0:
            return b''

        self._tot_length -= length
        offset = self._offset
        cur_string = self._buffer[0]
        cur_len = len(cur_string)
        if offset == 0 and length == cur_len:
            return self._advance_buffer()
        elif offset + length <= cur_len:
            if offset + length == cur_len:
                self._advance_buffer()
            else:
                self._offset += length
            if as_view:
                cur_string = memoryview(cur_string)
            return cur_string[offset:offset + length]

        ret = bytearray(length)
        copied = 0
        while copied < length:
            to_copy = length - copied
            if to_copy + offset >= cur_len:
                delta = cur_len - offset
                bytes_to_copy = memoryview(cur_string)[offset:]
                self._advance_buffer()
                if self._buffer:
                    cur_string = self._buffer[0]
                    cur_len = len(cur_string)
                    offset = 0
                else:
                    cur_string = cur_len = offset = None
            else:
                delta = to_copy
                bytes_to_copy = memoryview(cur_string)[offset:offset + to_copy]
                self._offset += delta
            ret[copied:copied + delta] = bytes_to_copy
            copied += delta

        return bytes(ret)

    def pop_atmost(self, length):
        return self.pop(length, underflow=False)

    def pop_view(self, length=None):
        return self.pop(length, as_view=True)

    def pop_struct(self, format):
        s = struct.Struct(format)
        return s.unpack(self.pop(s.size))

    def _find_delimiter(self, delimiter, exc):
        if delimiter is None:
            delimiter = self.delimiter
        if not delimiter:
            raise ValueError()
        delim_len = len(delimiter)
        if self._tot_length < delim_len:
            raise exc()

        offset = self._offset
        split_trial = None
        pos = 0
        for cur_string in self._buffer:
            cur_len = len(cur_string)
            if split_trial is not None:
                to_test = cur_string[:split_trial]
                test_len = len(to_test)
                delim_chunk = delimiter[
                    -split_trial:delim_len - split_trial + test_len]
                if to_test == delim_chunk:
                    split_trial -= test_len
                    pos += test_len - offset
                    if split_trial == 0:
                        return pos - delim_len, delim_len
                    continue
            delim_pos = cur_string.find(delimiter, offset)
            if delim_pos != -1:
                return pos + delim_pos - offset, delim_len
            if delim_len > 1:
                split_trial = next(
                    (delim_len + i
                     for i in range(-delim_len + 1, 0)
                     if delimiter.startswith(cur_string[i:])),
                    None)
            pos += cur_len - offset
            offset = 0

        raise exc()

    def popline(self, delimiter=None, keepends=False, _exc=BufferUnderflow):
        to_delim, delim_len = self._find_delimiter(delimiter, _exc)
        if keepends:
            return self.pop(to_delim + delim_len)
        else:
            ret = self.pop(to_delim)
            self.pop(delim_len)
            return ret

    def poplines(self, delimiter=None):
        ret = []
        while True:
            try:
                ret.append(self.popline(delimiter))
            except BufferUnderflow:
                break
        return ret

    def clear(self):
        self._buffer.clear()
        self._offset = 0
        self._tot_length = 0

    def __iter__(self):
        return self

    def __next__(self):
        return self.popline(keepends=True, _exc=StopIteration)

    next = __next__
