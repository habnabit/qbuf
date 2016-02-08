import itertools
import json
import os
import subprocess
import sys
import tempfile

import click

import qbuf


def show(item):
    if item is None:
        return None
    return '{0[lines]}/{0[length]}  {1}kB  {2!r}'.format(*item)


def main():
    _, config_path, toxenv, iterations = sys.argv

    here = os.path.dirname(os.path.abspath(__file__))

    with open(os.path.join(here, 'data.json')) as infile:
        data = json.load(infile)

    for item in data:
        outfile = tempfile.NamedTemporaryFile()
        outfile.write(item.pop('data').encode())
        item['data_file'] = outfile

    prod = list(itertools.product(
        data,
        [2, 4, 8, 32, 1024],
        [cls for cls in ('BufferQueue', 'PythonBufferQueue', '')
         if not cls or hasattr(qbuf, cls)]))

    times = []
    with click.progressbar(prod, show_eta=False, item_show_func=show,
                           bar_template='[%(bar)s] %(info)s') as bar:
        for item, chunk_kb, cls in bar:
            item = item.copy()
            args = [sys.executable,
                    # '-mvmprof', '--web', '--config', config_path,
                    os.path.join(here, 'benchmark.py'),
                    item.pop('data_file').name, str(chunk_kb * 1024), cls,
                    iterations]
            proc = subprocess.Popen(args, stdout=subprocess.PIPE)
            stdout, _ = proc.communicate()
            item.update({
                'benchmarks': [float(line) for line in stdout.splitlines()],
                'chunk_kb': chunk_kb,
                'cls': cls or None,
            })
            times.append(item)

    with open(os.path.join(here, '%s.json' % (toxenv,)), 'w') as outfile:
        json.dump(times, outfile)


main()
