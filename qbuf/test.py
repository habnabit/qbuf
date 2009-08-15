"""Unit tests for qbuf.
"""
from __future__ import with_statement
import StringIO
import unittest
import qbuf
import os

class BufferPair(object):
    def __init__(self, case, buf=None, data=None, data_length=10240):
        self.case = case
        if buf is None:
            buf = qbuf.BufferQueue()
        self.test_buf = buf
        if data is None:
            data = os.urandom(data_length)
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
    
    def clear(self):
        self.test_buf.clear()
        self.out_buf.seek(self.size_delta, 1)
        self.size_delta = 0
    
    def __enter__(self):
        return self
    
    def __exit__(self, *exc_info):
        self.case.assertEquals(len(self.test_buf), 0)

class QbufTest(unittest.TestCase):
    def test_growth(self):
        with BufferPair(self) as pair:
            for x in xrange(128):
                pair.push(x + 1)
            pair.pop()
    
    def test_circularity(self):
        with BufferPair(self) as pair:
            pair.push(24)
            for x in xrange(16):
                pair.push(24)
                pair.pop(24)
            pair.pop(24)
    
    def test_growth_and_circularity(self):
        with BufferPair(self) as pair:
            pair.push(23)
            for x in xrange(128):
                pair.push(23)
                pair.pop(6)
            pair.pop(23*129 - 6*128)
    
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
        with BufferPair(self, qbuf.BufferQueue('***'), data) as pair:
            for x in [6, 6, 7, 7, 7, 8, 1, 7, 3, 2]:
                pair.push(x)
            for x in xrange(5):
                pair.popline()
            pair.pop(2)
    
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
        with BufferPair(self) as pair:
            for x in xrange(24):
                pair.push(8)
            pair.clear()
            self.assertRaises(qbuf.BufferUnderflow, pair.test_buf.pop, 1)
            for x in xrange(12):
                pair.push(8)
            pair.pop(8*6)
            pair.clear()
            self.assertRaises(qbuf.BufferUnderflow, pair.test_buf.pop, 1)
