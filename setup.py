from distutils.core import setup, Extension
setup(name="ringbuf", version="1.0",
      ext_modules=[Extension("ringbuf", ["ringbuf.c"])])
