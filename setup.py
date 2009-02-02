from distutils.core import setup, Extension
setup(
    name='qbuf', 
    version='1.0',
    packages=['qbuf'],
    ext_modules=[
        Extension('qbuf._qbuf', ['qbufmodule.c'])],
)
