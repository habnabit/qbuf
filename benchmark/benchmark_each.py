import itertools
import json
import os
import subprocess
import sys
import time

import qbuf


def main():
    _, config_path, toxenv = sys.argv

    here = os.path.dirname(os.path.abspath(__file__))

    with open(os.path.join(here, 'data.json')) as infile:
        data = json.load(infile)

    it = itertools.product(
        data,
        [2, 4, 8, 32, 1024],
        [cls for cls in ('BufferQueue', 'PythonBufferQueue')
         if hasattr(qbuf, cls)],
        ['cold',
         # 'warm',
         ],
        range(10))

    times = []
    for item, chunk_kb, cls, warmth, trial in it:
        item = item.copy()
        args = [sys.executable,
                # '-mvmprof', '--web', '--config', config_path,
                os.path.join(here, 'benchmark.py'),
                str(chunk_kb * 1024), cls, warmth]
        sys.stderr.write('%s\n' % (args,))
        start = time.time()
        proc = subprocess.Popen(
            args, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        stdout, _ = proc.communicate(item.pop('data').encode())
        total_delta = time.time() - start
        item.update({
            'total_delta': total_delta,
            'benchmark_delta': float(stdout),
            'chunk_kb': chunk_kb,
            'cls': cls,
            'warmth': warmth,
            'trial': trial,
        })
        times.append(item)

    with open(os.path.join(here, '%s.json' % (toxenv,)), 'w') as outfile:
        json.dump(times, outfile)


main()
