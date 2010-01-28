from distutils.core import setup, Extension
setup(
    name='qbuf', 
    version='0.9.4',
    packages=['qbuf', 'qbuf.support'],
    ext_modules=[
        Extension('qbuf._qbuf', ['qbufmodule.c'])],
    
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
