import sys

from setuptools import setup, Extension


ext_modules = []
if sys.version_info < (3,):
    ext_modules.append(Extension('qbuf._qbuf', ['qbufmodule.c']))


setup(
    name='qbuf',
    version='0.9.4',
    packages=['qbuf', 'qbuf.support'],
    ext_modules=ext_modules,

    install_requires=['six'],

    author='Aaron Gallagher',
    author_email='habnabit@gmail.com',
    url='http://www.habnabit.org/qbuf',
    description='Simple string buffering for python written in C.',
    classifiers=[
        'Development Status :: 4 - Beta',
        'Framework :: Twisted',
        'Intended Audience :: Developers',
        'License :: OSI Approved :: BSD License',
        'Operating System :: OS Independent',
        'Programming Language :: C',
        'Programming Language :: Python :: 2',
        'Topic :: Utilities',
    ],
)
