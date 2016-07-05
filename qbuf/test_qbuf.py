"""Unit tests for qbuf.
"""

import io
import random
import struct

from six.moves import xrange
import pytest
import six

import qbuf


class BufferPair(object):
    def __init__(self, rng, buf, data=None, data_length=10240):
        self.test_buf = buf
        if data is None:
            data = bytes(bytearray(
                rng.randrange(256) for _ in xrange(data_length)))
        self.in_buf = io.BytesIO(data)
        self.out_buf = io.BytesIO(data)
        self.size_delta = 0

    def push(self, size):
        data = self.in_buf.read(size)
        assert len(data) == size
        self.test_buf.push(data)
        self.size_delta += size

    def pop(self, size=None):
        if size is None:
            size = self.size_delta
        assert self.out_buf.read(size) == self.test_buf.pop(size)
        self.size_delta -= size

    def popline(self):
        line = self.test_buf.popline()
        size = len(line) + len(self.test_buf.delimiter)
        assert self.out_buf.read(size) == line + self.test_buf.delimiter
        self.size_delta -= size

    def popline_with_delim(self, delimiter):
        line = self.test_buf.popline(delimiter)
        if delimiter is None:
            delimiter = self.test_buf.delimiter
        size = len(line) + len(delimiter)
        assert self.out_buf.read(size) == line + delimiter
        self.size_delta -= size

    def clear(self):
        self.test_buf.clear()
        self.out_buf.seek(self.size_delta, 1)
        self.size_delta = 0

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.close()

    def close(self):
        assert len(self.test_buf) == 0


@pytest.fixture(params=('python', 'c'))
def buf_factory(request):
    if request.param == 'python':
        return qbuf.PythonBufferQueue
    elif request.param == 'c':
        if six.PY3:
            pytest.skip('no C impl for py3')
        return qbuf.BufferQueue


@pytest.fixture
def pair_factory(request, buf_factory):
    rng = random.Random(str(request))

    def fac(**kw):
        return BufferPair(rng, buf_factory(kw.pop('delimiter', None)), **kw)

    return fac


@pytest.fixture
def pair(pair_factory):
    return pair_factory()


def test_growth(pair):
    for x in xrange(128):
        pair.push(x + 1)
    pair.pop()
    pair.close()


def test_circularity(pair):
    pair.push(24)
    for x in xrange(16):
        pair.push(24)
        pair.pop(24)
    pair.pop(24)
    pair.close()


def test_growth_and_circularity(pair):
    pair.push(23)
    for x in xrange(128):
        pair.push(23)
        pair.pop(6)
    pair.pop(23*129 - 6*128)
    pair.close()


def test_delimiter_search(pair_factory):
    data = (
        b'extra '  # 6
        b'chaff '  # 6
        b'foo ***'  # 7
        b' bar **'  # 7
        b'* baz *'  # 7
        b'** bat *'  # 8
        b'*'  # 1
        b'* blat '  # 7
        b'***'  # 3
        b'**'  # 2
    )
    pair = pair_factory(delimiter=b'***', data=data)
    for x in [6, 6, 7, 7, 7, 8, 1, 7, 3, 2]:
        pair.push(x)
    for x in xrange(5):
        pair.popline()
    pair.pop(2)
    pair.close()


def test_passed_delimiter(pair_factory):
    data = (
        b'foo*bar&f'  # 9
        b'oo*bar*foo&bar'  # 14
    )
    pair = pair_factory(delimiter=b'&', data=data)
    for x in [9, 14]:
        pair.push(x)
    pair.popline_with_delim(b'*')
    pair.popline_with_delim(b'&')
    pair.popline_with_delim(None)
    pair.pop(3)
    pair.close()


def test_repr(buf_factory):
    buf = buf_factory()
    assert '<BufferQueue of 0 bytes>' == repr(buf)
    buf.push('foobar')
    assert '<BufferQueue of 6 bytes>' == repr(buf)


def test_iter(buf_factory):
    buf = buf_factory(b'\n')
    buf.push(b'foo\nbar\nbaz')
    assert [b'foo\n', b'bar\n'] == list(buf)
    assert b'baz' == buf.pop()


def test_exceptions(buf_factory):
    buf = buf_factory()
    pytest.raises(ValueError, buf.next)
    pytest.raises(ValueError, buf.popline)
    pytest.raises(ValueError, buf.popline, '')
    pytest.raises(ValueError, buf.poplines)
    pytest.raises(ValueError, buf.pop, -1)
    pytest.raises(ValueError, buf.pop_atmost, -1)
    pytest.raises(qbuf.BufferUnderflow, buf.pop, 1)
    pytest.raises(TypeError, buf.push, None)
    pytest.raises(TypeError, buf.push_many, None)
    pytest.raises(TypeError, buf.push_many, [None])
    buf.delimiter = '***'
    pytest.raises(ValueError, buf.popline)


def test_corners(buf_factory):
    buf = buf_factory()
    buf.push(b'')
    assert buf.pop(0) == b''
    assert buf.delimiter == b''
    buf.delimiter = b'foo'
    buf.delimiter = b''
    assert buf.delimiter == b''


def test_pushpop(buf_factory):
    buf = buf_factory()
    buf.push_many([b'foo', b'bar', b'baz'])
    assert b'foobarbaz' == buf.pop_atmost(10)
    buf.delimiter = b'\n'
    buf.push(b'foo\nbar\nbaz')
    assert [b'foo', b'bar'] == buf.poplines()
    assert b'baz' == buf.pop()


def test_clear(pair):
    for x in xrange(24):
        pair.push(8)
    pair.clear()
    pytest.raises(qbuf.BufferUnderflow, pair.test_buf.pop, 1)
    for x in xrange(12):
        pair.push(8)
    pair.pop(8*6)
    pair.clear()
    pytest.raises(qbuf.BufferUnderflow, pair.test_buf.pop, 1)
    pair.close()


def test_pop_view(buf_factory):
    buf = buf_factory()
    buf.push_many([b'foo', b'bar'])
    for part in [b'fo', b'ob', b'ar']:
        b = buf.pop_view(2)
        assert part == memoryview(b).tobytes()
    pytest.raises(qbuf.BufferUnderflow, buf.pop_view, 2)
    b = buf.pop_view(0)
    assert b'' == memoryview(b).tobytes()
    buf.push_many([b'foo', b'bar', b'baz'])
    b = buf.pop_view()
    assert b'foobarbaz' == memoryview(b).tobytes()


def test_pop_struct(buf_factory):
    buf = buf_factory()
    buf.push(b'\x01\x02\x03\x04\x05\x06')
    a, b = buf.pop_struct('!BH')
    assert a == 0x1
    assert b == 0x203
    a, = buf.pop_struct('!H')
    assert a == 0x405
    pytest.raises(qbuf.BufferUnderflow, buf.pop_struct, '!H')
    pytest.raises(struct.error, buf.pop_struct, '_bad_struct_format')
