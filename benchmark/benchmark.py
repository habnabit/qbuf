import sys
import time

from six.moves import xrange
import six

import qbuf


def main():
    _, chunk_size, cls, warmth = sys.argv
    chunk_size = int(chunk_size)

    def new_buf():
        return getattr(qbuf, cls)(b'\n')

    if warmth == 'warm':
        buf = new_buf()
        for x in xrange(1000):
            buf.push_many((b'\0\1\2',) * 100)
            buf.poplines()
            buf.poplines(b'\1')

    if six.PY2:
        stdin = sys.stdin
    else:
        stdin = sys.stdin.detach()
    buf = new_buf()

    start = time.time()
    while True:
        chunk = stdin.read(chunk_size)
        if not chunk:
            break
        buf.push(chunk)
        for _ in buf:
            pass
    delta = time.time() - start
    sys.stdout.write(str(delta))


main()
