#define PY_SSIZE_T_CLEAN
#include <stdio.h>
#include <string.h>
#include <Python.h>
#include "structmember.h"

PyDoc_STRVAR(Ringbuf_doc,
"Ringbuf(size[, delimiter])\n\
\n\
Makes a new ring buffer of at most 'size' bytes. If the delimiter\n\
is provided, it can be used to pop lines off of the buffer.\n\
");

typedef struct {
    PyObject_HEAD
    void *buffer;
    void *buffer_end;
    void *ptr_start;
    void *ptr_end;
    Py_ssize_t length;
    char *delimiter;
    Py_ssize_t delim_size;
    Py_ssize_t buffer_size;
} Ringbuf;

static int
Ringbuf_push(Ringbuf *self, char *data, Py_ssize_t length)
{
    if (length + self->length > self->buffer_size)
        return -1;
    if (self->ptr_end + length > self->buffer_end)
    {
        Py_ssize_t overflow = self->buffer_end - self->ptr_end;
        memcpy(self->ptr_end, data, overflow);
        memcpy(self->buffer, data + overflow, length - overflow);
        self->ptr_end = self->buffer + length - overflow;
    }
    else
    {
        memcpy(self->ptr_end, data, length);
        self->ptr_end += length;
    }
    self->length += length;
    return 0;
}

static int
Ringbuf_pop(Ringbuf *self, Py_ssize_t length, PyObject **ret_obj)
{
    if (length > self->length)
        return -1;
    PyObject *ret;
    if (self->ptr_start + length > self->buffer_end)
    {
        Py_ssize_t overflow = self->buffer_end - self->ptr_start;
        ret = PyString_FromStringAndSize((char *) 0, length);
        if (ret == NULL)
            return 0;
        memcpy(PyString_AS_STRING(ret), self->ptr_start, overflow);
        memcpy(PyString_AS_STRING(ret) + overflow, 
            self->buffer, length - overflow);
        self->ptr_start = self->buffer + length - overflow;
    }
    else
    {
        ret = PyString_FromStringAndSize(self->ptr_start, length);
        if (ret == NULL)
            return 0;
        self->ptr_start += length;
    }
    self->length -= length;
    *ret_obj = ret;
    return 0;
}

static int
Ringbuf_find_delim(Ringbuf *self, Py_ssize_t *loc)
{
    if (self->delim_size > self->length)
        return -1;
    
    Py_ssize_t delta, overflow = self->buffer_end - self->ptr_start;
    Py_ssize_t split, inv_split;
    Py_ssize_t target = self->length - self->delim_size;
    void *begin;
    for (delta = 0; delta <= target; ++delta)
    {
        if (delta > overflow)
        {
            begin = self->buffer - overflow + delta;
            if (memcmp(begin, self->delimiter, self->delim_size) == 0)
            {
                *loc = delta;
                return 0;
            }
        }
        else
        {
            begin = self->ptr_start + delta;
            if (begin + self->delim_size <= self->buffer_end)
            {
                if (memcmp(begin, self->delimiter, self->delim_size) == 0)
                {
                    *loc = delta;
                    return 0;
                }
            }
            else
            {
                split = self->buffer_end - begin;
                inv_split = self->delim_size - split;
                if (memcmp(begin, self->delimiter, split) == 0 &&
                    memcmp(self->buffer, self->delimiter + split, inv_split) == 0)
                {
                    *loc = delta;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static void
Ringbuf_dealloc(Ringbuf *self)
{
    PyMem_Free(self->buffer);
    PyMem_Free(self->delimiter);
    self->ob_type->tp_free((PyObject*)self);
}

static int
Ringbuf_init(Ringbuf *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"size", "delimiter", NULL};
    char *delim_tmp;
    PyObject *delim_obj;
    if (! PyArg_ParseTupleAndKeywords(args, kwds, "n|O", kwlist, 
            &self->buffer_size, &delim_obj))
        return -1; 
    
    if (delim_obj != Py_None)
    {
        if (! PyString_Check(delim_obj))
        {
            PyErr_SetString(PyExc_TypeError, "delimiter must be a string");
            return -1;
        }
        if (PyString_AsStringAndSize(delim_obj, &delim_tmp, &self->delim_size) == -1)
            return -1;
        self->delimiter = (char *)PyMem_Malloc(self->delim_size);
        if (self->delimiter == NULL) {
            PyErr_SetString(PyExc_MemoryError, "malloc of delimiter failed");
            return -1;
        }
        memcpy(self->delimiter, delim_tmp, self->delim_size);
    }

    self->buffer = PyMem_Malloc(self->buffer_size);
    if (self->buffer == NULL) {
        PyMem_Free(self->delimiter);
        PyErr_SetString(PyExc_MemoryError, "malloc of buffer failed");
        return -1;
    }
    self->ptr_start = self->ptr_end = self->buffer;
    self->buffer_end = self->buffer + self->buffer_size;

    return 0;
}

static PyObject *
Ringbuf_getdelim(Ringbuf *self, void *closure)
{
    return PyString_FromStringAndSize(self->delimiter, self->delim_size);
}

static int
Ringbuf_setdelim(Ringbuf *self, PyObject *value, void *closure)
{
    if (value == Py_None)
    {
        PyMem_Free(self->delimiter);
        self->delimiter = NULL;
        self->delim_size = 0;
        return 0;
    }
    if (! PyString_Check(value))
    {
        PyErr_SetString(PyExc_TypeError, "delimiter must be a string");
        return -1;
    }
    char *delim_tmp;
    Py_ssize_t delim_size;
    if (PyString_AsStringAndSize(value, &delim_tmp, &delim_size) == -1)
        return -1;
    char *realloc_tmp = PyMem_Realloc(self->delimiter, delim_size);
    if (realloc_tmp == NULL)
    {
        PyErr_SetString(PyExc_MemoryError, "realloc of delimiter failed");
        return -1;
    }
    self->delimiter = realloc_tmp;
    memcpy(self->delimiter, delim_tmp, delim_size);
    self->delim_size = delim_size;
    return 0;
}

static PyObject *
Ringbuf_getsize(Ringbuf *self, void *closure)
{
    return PyInt_FromSsize_t(self->buffer_size);
}

static PyObject *
Ringbuf_getlength(Ringbuf *self, void *closure)
{
    return PyInt_FromSsize_t(self->length);
}

static PyGetSetDef Ringbuf_getset[] = {
    {"delimiter",
     (getter)Ringbuf_getdelim, (setter)Ringbuf_setdelim,
     "delimiter string",
     NULL},
    {"size",
     (getter)Ringbuf_getsize, NULL,
     "buffer size in bytes",
     NULL},
    {"length",
     (getter)Ringbuf_getlength, NULL,
     "current number of bytes in the buffer",
     NULL},
    {NULL}  /* Sentinel */
};

PyDoc_STRVAR(Ringbuf_doc_push,
"push(a_string)\n\
\n\
Push a string into the buffer. Raises a ValueError if the new\n\
string would overflow the buffer.\n\
");

static PyObject *
Ringbuf_dopush(Ringbuf *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"string", NULL};
    char *in_string;
    Py_ssize_t in_string_size;
    if (! PyArg_ParseTupleAndKeywords(args, kwds, "s#:push", kwlist,
            &in_string, &in_string_size))
        return NULL;
    if (Ringbuf_push(self, in_string, in_string_size) == -1)
    {
        PyErr_Format(PyExc_ValueError, "buffer overflow: holds %zd bytes, "
            "currently at %zd bytes, tried to add %zd bytes",
            self->buffer_size, self->length, in_string_size);
        return NULL;
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(Ringbuf_doc_pop,
"pop([some_bytes])\n\
\n\
Pop some bytes out of the buffer. If no argument is provided, pop\n\
the entire buffer out. Raises a ValueError if the buffer would\n\
underflow.\n\
");

static PyObject *
Ringbuf_dopop(Ringbuf *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"length", NULL};
    Py_ssize_t out_string_size = self->length;
    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|n:pop", kwlist,
            &out_string_size))
        return NULL;
    if (out_string_size < 0)
    {
        PyErr_SetString(PyExc_ValueError, "tried to pop a negative number of "
            "bytes from buffer");
        return NULL;
    }
    PyObject *ret = NULL;
    if (Ringbuf_pop(self, out_string_size, &ret) == -1)
    {
        PyErr_Format(PyExc_ValueError, "buffer underflow: currently at %zd "
            "bytes, tried to pop %zd bytes", self->length, out_string_size);
        return NULL;
    }
    return ret;
}

static PyObject *
Ringbuf_dopopline(Ringbuf *self)
{
    if (self->delim_size == 0)
    {
        PyErr_SetString(PyExc_ValueError, "no delimiter");
        return NULL;
    }
    Py_ssize_t out_string_size; 
    if (Ringbuf_find_delim(self, &out_string_size) == -1)
    {
        PyErr_SetString(PyExc_ValueError, "delimiter not found");
        return NULL;
    }
    PyObject *ret = NULL;
    Ringbuf_pop(self, out_string_size + self->delim_size, &ret);
    return ret;
}

static PyObject *
Ringbuf_dopoplines(Ringbuf *self)
{
    if (self->delim_size == 0)
    {
        PyErr_SetString(PyExc_ValueError, "no delimiter");
        return NULL;
    }
    PyObject *ret = PyList_New(0), *ret_str = NULL;
    Py_ssize_t out_string_size; 
    while (Ringbuf_find_delim(self, &out_string_size) != -1)
    {
        Ringbuf_pop(self, out_string_size + self->delim_size, &ret_str);
        PyList_Append(ret, ret_str);
        Py_DECREF(ret_str);
    }
    return ret;
}

static PyMethodDef Ringbuf_methods[] = {
    {"push", (PyCFunction)Ringbuf_dopush, METH_VARARGS | METH_KEYWORDS, 
        Ringbuf_doc_push},
    {"pop", (PyCFunction)Ringbuf_dopop, METH_VARARGS | METH_KEYWORDS, 
        Ringbuf_doc_pop},
    {"popline", (PyCFunction)Ringbuf_dopopline, METH_NOARGS, 
        ""},
    {"poplines", (PyCFunction)Ringbuf_dopoplines, METH_NOARGS,
        ""},
    {NULL}  /* Sentinel */
};

static PyTypeObject RingbufType = {
    PyObject_HEAD_INIT(NULL)
    0,                          /*ob_size*/
    "ringbuf.Ringbuf",          /*tp_name*/
    sizeof(Ringbuf),            /*tp_basicsize*/
    0,                          /*tp_itemsize*/
    (destructor)Ringbuf_dealloc, /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare*/
    0,                          /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash */
    0,                          /*tp_call*/
    0,                          /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    Ringbuf_doc,          /* tp_doc */
    0,                          /* tp_traverse */
    0,                          /* tp_clear */
    0,                          /* tp_richcompare */
    0,                          /* tp_weaklistoffset */
    0,                          /* tp_iter */
    0,                          /* tp_iternext */
    Ringbuf_methods,              /* tp_methods */
    0,              /* tp_members */
    Ringbuf_getset,                          /* tp_getset */
    0,                          /* tp_base */
    0,                          /* tp_dict */
    0,                          /* tp_descr_get */
    0,                          /* tp_descr_set */
    0,                          /* tp_dictoffset */
    (initproc)Ringbuf_init,       /* tp_init */
};

static PyMethodDef ringbuf_methods[] = {
    {NULL}  /* Sentinel */
};

#ifndef PyMODINIT_FUNC  /* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif
PyMODINIT_FUNC
initringbuf(void) 
{
    PyObject* m;
    
    RingbufType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&RingbufType) < 0)
        return;

    m = Py_InitModule3("ringbuf", ringbuf_methods,
                       "Ring buffers for Python in C.");

    Py_INCREF(&RingbufType);
    PyModule_AddObject(m, "Ringbuf", (PyObject *)&RingbufType);
}
