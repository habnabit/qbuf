import sys
import time

from six.moves import xrange

import qbuf


def main():
    _, data_file, chunk_size, cls, iterations = sys.argv
    chunk_size = int(chunk_size)
    iterations = int(iterations)

    def new_buf():
        if not cls:
            return None
        return getattr(qbuf, cls)(b'\n')

    for x in xrange(iterations):
        buf = new_buf()
        with open(data_file, 'rb') as infile:
            start = time.time()
            while True:
                chunk = infile.read(chunk_size)
                if not chunk:
                    break
                if buf is None:
                    continue
                buf.push(chunk)
                for _ in buf:
                    pass
            delta = time.time() - start
        sys.stdout.write('%s\n' % (delta,))


main()
