"""Unit tests for qbuf.
"""
import StringIO
import unittest
import random
import struct
import qbuf

class BufferPair(object):
    def __init__(self, case, buf=None, data=None, data_length=10240):
        self.case = case
        if buf is None:
            buf = qbuf.BufferQueue()
        self.test_buf = buf
        if data is None:
            data = ''.join([chr(random.randrange(256)) 
                for _ in xrange(data_length)])
        self.in_buf = StringIO.StringIO(data)
        self.out_buf = StringIO.StringIO(data)
        self.size_delta = 0
    
    def push(self, size):
        data = self.in_buf.read(size)
        self.case.assertEquals(len(data), size)
        self.test_buf.push(data)
        self.size_delta += size
    
    def pop(self, size=None):
        if size is None:
            size = self.size_delta
        self.case.assertEquals(
            self.out_buf.read(size), self.test_buf.pop(size))
        self.size_delta -= size
    
    def popline(self):
        line = self.test_buf.popline()
        size = len(line) + len(self.test_buf.delimiter)
        self.case.assertEquals(
            self.out_buf.read(size), line + self.test_buf.delimiter)
        self.size_delta -= size
    
    def popline_with_delim(self, delimiter):
        line = self.test_buf.popline(delimiter)
        if delimiter is None:
            delimiter = self.test_buf.delimiter
        size = len(line) + len(delimiter)
        self.case.assertEquals(self.out_buf.read(size), line + delimiter)
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
        self.case.assertEquals(len(self.test_buf), 0)

class QbufTest(unittest.TestCase):
    def test_growth(self):
        pair = BufferPair(self)
        for x in xrange(128):
            pair.push(x + 1)
        pair.pop()
        pair.close()
    
    def test_circularity(self):
        pair = BufferPair(self)
        pair.push(24)
        for x in xrange(16):
            pair.push(24)
            pair.pop(24)
        pair.pop(24)
        pair.close()
    
    def test_growth_and_circularity(self):
        pair = BufferPair(self)
        pair.push(23)
        for x in xrange(128):
            pair.push(23)
            pair.pop(6)
        pair.pop(23*129 - 6*128)
        pair.close()
    
    def test_delimiter_search(self):
        data = (
            'extra ' # 6
            'chaff ' # 6
            'foo ***' # 7
            ' bar **' # 7
            '* baz *' # 7
            '** bat *' # 8
            '*' # 1
            '* blat ' # 7
            '***' # 3
            '**' # 2
        )
        pair = BufferPair(self, qbuf.BufferQueue('***'), data)
        for x in [6, 6, 7, 7, 7, 8, 1, 7, 3, 2]:
            pair.push(x)
        for x in xrange(5):
            pair.popline()
        pair.pop(2)
        pair.close()
    
    def test_passed_delimiter(self):
        data = (
            'foo*bar&f' # 9
            'oo*bar*foo&bar' # 14
        )
        pair = BufferPair(self, qbuf.BufferQueue('&'), data)
        for x in [9, 14]:
            pair.push(x)
        pair.popline_with_delim('*')
        pair.popline_with_delim('&')
        pair.popline_with_delim(None)
        pair.pop(3)
        pair.close()
    
    def test_repr(self):
        buf = qbuf.BufferQueue()
        self.assertEquals(
            '<BufferQueue of 0 bytes at %#x>' % id(buf), repr(buf))
        buf.push('foobar')
        self.assertEquals(
            '<BufferQueue of 6 bytes at %#x>' % id(buf), repr(buf))
    
    def test_iter(self):
        buf = qbuf.BufferQueue('\n')
        buf.push('foo\nbar\nbaz')
        self.assertEquals(['foo\n', 'bar\n'], list(buf))
        self.assertEquals('baz', buf.pop())
    
    def test_exceptions(self):
        buf = qbuf.BufferQueue()
        self.assertRaises(ValueError, buf.next)
        self.assertRaises(ValueError, buf.popline)
        self.assertRaises(ValueError, buf.popline, '')
        self.assertRaises(ValueError, buf.poplines)
        self.assertRaises(ValueError, buf.pop, -1)
        self.assertRaises(ValueError, buf.pop_atmost, -1)
        self.assertRaises(qbuf.BufferUnderflow, buf.pop, 1)
        self.assertRaises(TypeError, buf.push, None)
        self.assertRaises(TypeError, buf.push_many, None)
        self.assertRaises(ValueError, buf.push_many, [None])
        buf.delimiter = '***'
        self.assertRaises(ValueError, buf.popline)
    
    def test_corners(self):
        buf = qbuf.BufferQueue()
        buf.push('')
        self.assertEquals('', buf.pop(0))
        self.assertEquals(None, buf.delimiter)
    
    def test_pushpop(self):
        buf = qbuf.BufferQueue()
        buf.push_many(['foo', 'bar', 'baz'])
        self.assertEquals('foobarbaz', buf.pop_atmost(10))
        buf.delimiter = '\n'
        buf.push('foo\nbar\nbaz')
        self.assertEquals(['foo', 'bar'], buf.poplines())
        self.assertEquals('baz', buf.pop())
    
    def test_clear(self):
        pair = BufferPair(self)
        for x in xrange(24):
            pair.push(8)
        pair.clear()
        self.assertRaises(qbuf.BufferUnderflow, pair.test_buf.pop, 1)
        for x in xrange(12):
            pair.push(8)
        pair.pop(8*6)
        pair.clear()
        self.assertRaises(qbuf.BufferUnderflow, pair.test_buf.pop, 1)
        pair.close()
    
    def test_pop_view(self):
        buf = qbuf.BufferQueue()
        buf.push_many(['foo', 'bar'])
        for part in ['fo', 'ob', 'ar']:
            b = buf.pop_view(2)
            self.assert_(isinstance(b, buffer))
            self.assertEquals(part, b[:])
        self.assertRaises(qbuf.BufferUnderflow, buf.pop_view, 2)
        b = buf.pop_view(0)
        self.assert_(isinstance(b, buffer))
        self.assertEquals('', b[:])
        buf.push_many(['foo', 'bar', 'baz'])
        b = buf.pop_view()
        self.assert_(isinstance(b, buffer))
        self.assertEquals('foobarbaz', b[:])
    
    def test_pop_struct(self):
        buf = qbuf.BufferQueue()
        buf.push('\x01\x02\x03\x04\x05\x06')
        a, b = buf.pop_struct('!BH')
        self.assertEquals(a, 0x1)
        self.assertEquals(b, 0x203)
        a, = buf.pop_struct('!H')
        self.assertEquals(a, 0x405)
        self.assertRaises(qbuf.BufferUnderflow, buf.pop_struct, '!H')
        self.assertRaises(struct.error, buf.pop_struct, '_bad_struct_format')
