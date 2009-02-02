#define PY_SSIZE_T_CLEAN
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <Python.h>
#include "structmember.h"

#define INITIAL_BUFFER_SIZE 8

static PyObject *qbuf_underflow;

PyDoc_STRVAR(BufferQueue_doc,
"BufferQueue([delimiter])\n\
\n\
Initialize a new buffer. If the delimiter is provided, it can be\n\
used to pop lines off instead of just bytes.\n\
\n\
Iterating over a BufferQueue is the same as repeatedly calling\n\
.popline() on it.\n\
");

typedef struct {
    PyObject_HEAD
    PyStringObject **buffer;
    Py_ssize_t start_idx;
    Py_ssize_t end_idx;
    Py_ssize_t buffer_length;
    Py_ssize_t n_items;
    Py_ssize_t tot_length;
    Py_ssize_t cur_offset;
    char *delimiter;
    Py_ssize_t delim_size;
} BufferQueue;

typedef struct {
    BufferQueue *parent;
    Py_ssize_t string_idx;
    Py_ssize_t char_idx;
    PyStringObject *cur_string;
    Py_ssize_t s_size;
    char *s_ptr;
} BufferQueueIterator;

static void
BufferQueueIterator_update(BufferQueueIterator *self)
{
    self->cur_string = self->parent->buffer[self->string_idx];
    self->s_size = PyString_GET_SIZE(self->cur_string);
    self->s_ptr = PyString_AS_STRING(self->cur_string);
}

static void
BufferQueueIterator_init(BufferQueueIterator *self, BufferQueue *parent)
{
    self->parent = parent;
    self->string_idx = parent->start_idx;
    self->char_idx = parent->cur_offset;
    BufferQueueIterator_update(self);
}

static int
BufferQueueIterator_advance(BufferQueueIterator *self)
{
    if (++self->char_idx == self->s_size) {
        self->char_idx = 0;
        if (++self->string_idx == self->parent->buffer_length)
            self->string_idx = 0;
        if (self->string_idx == self->parent->end_idx)
            return 1;
        else
            BufferQueueIterator_update(self);
    }
    return 0;
}

static int
BufferQueue_push(BufferQueue *self, PyStringObject *string) 
{
    if (PyString_GET_SIZE(string) == 0)
        return 0;
    
    if (self->n_items == self->buffer_length) {
        PyStringObject **l_buffer = self->buffer, **l_buffer_end;
        PyMem_Resize(l_buffer, PyStringObject *, self->buffer_length * 2);
        if (l_buffer == NULL) {
            PyErr_SetString(PyExc_MemoryError, "failed to realloc bigger buffer");
            return -1;
        }
        l_buffer_end = l_buffer + self->buffer_length;
        Py_ssize_t width = sizeof(self->buffer);
        if (self->start_idx == 0 && self->end_idx == 0) {
            memcpy(l_buffer_end, l_buffer, self->buffer_length * width);
        } else {
            Py_ssize_t split = self->buffer_length - self->start_idx;
            memcpy(l_buffer_end, l_buffer + self->start_idx, split * width);
            memcpy(l_buffer_end + split, l_buffer, self->end_idx * width);
        }
        self->buffer = l_buffer;
        self->start_idx = self->buffer_length;
        self->end_idx = 0;
        self->buffer_length *= 2;
    }
    self->buffer[self->end_idx] = string;
    if (++self->end_idx == self->buffer_length)
        self->end_idx = 0;
    ++self->n_items;
    self->tot_length += PyString_GET_SIZE(string);
    return 0;
}

static void
BufferQueue_advance_start(BufferQueue *self)
{
    --self->n_items;
    if (++self->start_idx == self->buffer_length)
        self->start_idx = 0;
}

static PyStringObject *
BufferQueue_pop(BufferQueue *self, Py_ssize_t length)
{
    if (length == 0)
        return (PyStringObject *)PyString_FromString("");
    
    PyObject *ret, *cur_string = (PyObject *)self->buffer[self->start_idx];
    if (self->cur_offset == 0 && PyString_GET_SIZE(cur_string) == length) {
        ret = (PyObject *)cur_string;
        BufferQueue_advance_start(self);
    } else if (self->cur_offset + length == PyString_GET_SIZE(cur_string)) {
        ret = PyString_FromStringAndSize(
            PyString_AS_STRING(cur_string) + self->cur_offset, length);
        if (ret == NULL)
            return NULL;
        self->cur_offset = 0;
        Py_DECREF(cur_string);
        BufferQueue_advance_start(self);
    } else {
        ret = PyString_FromStringAndSize((char *) NULL, length);
        if (ret == NULL)
            return NULL;
        char *ret_dest = PyString_AS_STRING(ret);
        Py_ssize_t copied = 0, to_copy, size, delta;
        while (copied < length) {
            to_copy = length - copied;
            size = PyString_GET_SIZE(cur_string);
            if (to_copy + self->cur_offset >= size) {
                delta = size - self->cur_offset;
                memcpy(ret_dest + copied, 
                    PyString_AS_STRING(cur_string) + self->cur_offset, delta);
                self->cur_offset = 0;
                Py_DECREF(cur_string);
                BufferQueue_advance_start(self);
                cur_string = (PyObject *)self->buffer[self->start_idx];
            } else {
                delta = to_copy;
                memcpy(ret_dest + copied, 
                    PyString_AS_STRING(cur_string) + self->cur_offset, delta);
                self->cur_offset += delta;
            }
            copied += delta;
        }
    }
    self->tot_length -= length;
    return (PyStringObject *)ret;
}

static int
BufferQueue_find_delim(BufferQueue *self, Py_ssize_t *loc) 
{
    if (self->delim_size > self->tot_length)
        return -1;
    
    BufferQueueIterator iter, split_iter;
    BufferQueueIterator_init(&iter, self);
    Py_ssize_t pos, delim_pos;
    Py_ssize_t target = self->tot_length - self->delim_size;
    for (pos = 0; pos <= target; ++pos) {
        if (iter.char_idx + self->delim_size > iter.s_size) {
            split_iter = iter;
            for (delim_pos = 0; delim_pos < self->delim_size; ++delim_pos) {
                if (self->delimiter[delim_pos] != split_iter.s_ptr[split_iter.char_idx])
                    goto next_iter;
                BufferQueueIterator_advance(&split_iter);
            }
            *loc = pos;
            return 0;
        } else {
            if (memcmp(iter.s_ptr + iter.char_idx, self->delimiter, self->delim_size) == 0) {
                *loc = pos;
                return 0;
            }
        }
        next_iter:
        BufferQueueIterator_advance(&iter);
    }
    return -1;
}

static int
BufferQueue_popline(BufferQueue *self, PyStringObject **ret)
{
    Py_ssize_t line_size;
    if (BufferQueue_find_delim(self, &line_size) == -1)
        return 0;
    
    PyStringObject *line = BufferQueue_pop(self, line_size);
    if (line == NULL)
        return -1;
    PyStringObject *delim = BufferQueue_pop(self, self->delim_size);
    if (delim == NULL)
        return -1;
    Py_DECREF(delim);
    *ret = line;
    return 1;
}

static void
BufferQueue_clear_buffer(BufferQueue *self)
{
    Py_ssize_t count, index = self->start_idx;
    for (count = 0; count < self->n_items; ++count) {
        Py_DECREF(self->buffer[index]);
        if (++index == self->buffer_length)
            index = 0;
    }
}

static void
BufferQueue_dealloc(BufferQueue *self)
{
    BufferQueue_clear_buffer(self);
    PyMem_Free(self->buffer);
    PyMem_Free(self->delimiter);
    self->ob_type->tp_free((PyObject *)self);
}

static PyObject *
BufferQueue_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    BufferQueue *self;

    self = (BufferQueue *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->buffer = (PyStringObject **)NULL;
        self->start_idx = self->end_idx = 0;
        self->delimiter = NULL;
        self->delim_size = 0;
        self->n_items = self->tot_length = self->cur_offset = 0;
    }

    return (PyObject *)self;
}


static int
BufferQueue_init(BufferQueue *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"delimiter", NULL};
    char *delim_tmp;
    PyObject *delim_obj = Py_None;
    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &delim_obj))
        return -1; 
    
    if (delim_obj != Py_None) {
        if (! PyString_Check(delim_obj)) {
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

    self->buffer_length = INITIAL_BUFFER_SIZE;
    self->buffer = PyMem_New(PyStringObject *, self->buffer_length);
    if (self->buffer == NULL) {
        PyMem_Free(self->delimiter);
        PyErr_SetString(PyExc_MemoryError, "malloc of buffer failed");
        return -1;
    }

    return 0;
}

static PyObject *
BufferQueue_getdelim(BufferQueue *self, void *closure)
{
    if (self->delim_size == 0)
        Py_RETURN_NONE;
    else
        return PyString_FromStringAndSize(self->delimiter, self->delim_size);
}

static int
BufferQueue_setdelim(BufferQueue *self, PyObject *value, void *closure)
{
    if (value == Py_None) {
        PyMem_Free(self->delimiter);
        self->delimiter = NULL;
        self->delim_size = 0;
        return 0;
    }
    if (! PyString_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "delimiter must be a string");
        return -1;
    }
    char *delim_tmp;
    Py_ssize_t delim_size;
    if (PyString_AsStringAndSize(value, &delim_tmp, &delim_size) == -1)
        return -1;
    char *realloc_tmp = PyMem_Realloc(self->delimiter, delim_size);
    if (realloc_tmp == NULL) {
        PyErr_SetString(PyExc_MemoryError, "realloc of delimiter failed");
        return -1;
    }
    self->delimiter = realloc_tmp;
    memcpy(self->delimiter, delim_tmp, delim_size);
    self->delim_size = delim_size;
    return 0;
}

static PyGetSetDef BufferQueue_getset[] = {
    {"delimiter",
     (getter)BufferQueue_getdelim, (setter)BufferQueue_setdelim,
     "delimiter string",
     NULL},
    {NULL}  /* Sentinel */
};

PyDoc_STRVAR(BufferQueue_doc_push,
"push(string) -> None\n\
\n\
Push a string into the buffer.\n\
");

static PyObject *
BufferQueue_dopush(BufferQueue *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"string", NULL};
    PyStringObject *in_string;
    if (! PyArg_ParseTupleAndKeywords(args, kwds, "S:push", kwlist,
            &in_string))
        return NULL;
    if (BufferQueue_push(self, in_string) == -1)
        return NULL;
    Py_INCREF(in_string);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(BufferQueue_doc_push_many,
"push_many(iterable) -> None\n\
\n\
Push each string in the provided iterable into the buffer.\n\
");

static PyObject *
BufferQueue_dopush_many(BufferQueue *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"iterable", NULL};
    PyObject *iter;
    if (! PyArg_ParseTupleAndKeywords(args, kwds, "O:push_many", kwlist,
            &iter))
        return NULL;
    iter = PyObject_GetIter(iter);
    if (iter == NULL)
        return NULL;
    
    PyObject *item;
    while ((item = PyIter_Next(iter)) != NULL) {
        if (BufferQueue_push(self, (PyStringObject *)item) == -1)
            return NULL;
    }
    
    Py_DECREF(iter);
    if (PyErr_Occurred() != NULL)
        return NULL;
    else
        Py_RETURN_NONE;
}

PyDoc_STRVAR(BufferQueue_doc_pop,
"pop([length]) -> str\n\
\n\
Pop some bytes out of the buffer. If no argument is provided, pop\n\
the entire buffer out. Raises a BufferUnderflow exception if the\n\
buffer would underflow.\n\
");

static PyObject *
BufferQueue_dopop(BufferQueue *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"length", NULL};
    Py_ssize_t out_string_size = self->tot_length;
    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|n:pop", kwlist,
            &out_string_size))
        return NULL;
    if (out_string_size < 0) {
        PyErr_SetString(PyExc_ValueError, "tried to pop a negative number of "
            "bytes from buffer");
        return NULL;
    } else if (out_string_size > self->tot_length) {
        PyErr_Format(qbuf_underflow, "buffer underflow: currently at %zd "
            "bytes, tried to pop %zd bytes", self->tot_length, out_string_size);
        return NULL;
    }
    return (PyObject *)BufferQueue_pop(self, out_string_size);
}

PyDoc_STRVAR(BufferQueue_doc_pop_atmost,
"pop_atmost(length) -> str\n\
\n\
Pop at most some number of bytes from the buffer. The returned\n\
string will have a length anywhere between 0 and the length\n\
provided.\n\
");

static PyObject *
BufferQueue_dopop_atmost(BufferQueue *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"length", NULL};
    Py_ssize_t out_string_size;
    if (! PyArg_ParseTupleAndKeywords(args, kwds, "n:pop_atmost", kwlist,
            &out_string_size))
        return NULL;
    if (out_string_size < 0) {
        PyErr_SetString(PyExc_ValueError, "tried to pop a negative number of "
            "bytes from buffer");
        return NULL;
    }
    if (out_string_size > self->tot_length)
        out_string_size = self->tot_length;
    return (PyObject *)BufferQueue_pop(self, out_string_size);
}

PyDoc_STRVAR(BufferQueue_doc_popline,
"popline() -> str\n\
\n\
Pop one line of data from the buffer. This scans the buffer for\n\
the next occurrence of the buffer's delimiter, and then returns\n\
everything up to and including the delimiter. If the delimiter\n\
was not found or there was no delimiter set, a ValueError is\n\
raised.\n\
");

static PyObject *
BufferQueue_dopopline(BufferQueue *self)
{
    if (self->delim_size == 0) {
        PyErr_SetString(PyExc_ValueError, "no delimiter");
        return NULL;
    }
    PyStringObject *ret;
    int result = BufferQueue_popline(self, &ret);
    if (result == -1)
        return NULL;
    else if (result == 0) {
        PyErr_SetString(PyExc_ValueError, "delimiter not found");
        return NULL;
    }
    return (PyObject *)ret;
}

PyDoc_STRVAR(BufferQueue_doc_poplines,
"poplines() -> list\n\
\n\
Pop as many lines off of the buffer as is possible. This will\n\
collect and return a list of all of the lines that were in the\n\
buffer. If there was no delimiter set, a ValueError is raised.\n\
");

static PyObject *
BufferQueue_dopoplines(BufferQueue *self)
{
    if (self->delim_size == 0) {
        PyErr_SetString(PyExc_ValueError, "no delimiter");
        return NULL;
    }
    PyObject *ret = PyList_New(0);
    if (ret == NULL)
        return NULL;
    int result;
    PyStringObject *ret_str;
    while ((result = BufferQueue_popline(self, &ret_str)) == 1) {
        PyList_Append(ret, (PyObject *)ret_str);
        Py_DECREF(ret_str);
    }
    if (result == -1) {
        Py_DECREF(ret);
        return NULL;
    }
    return ret;
}

PyDoc_STRVAR(BufferQueue_doc_clear,
"clear() -> None\n\
\n\
Clear the buffer.\n\
");

static PyObject *
BufferQueue_doclear(BufferQueue *self)
{
    BufferQueue_clear_buffer(self);
    self->start_idx = self->end_idx = self->n_items = 0;
    self->tot_length = self->cur_offset = 0;
    Py_RETURN_NONE;
}

static PyMethodDef BufferQueue_methods[] = {
    {"push", (PyCFunction)BufferQueue_dopush, METH_VARARGS | METH_KEYWORDS, 
        BufferQueue_doc_push},
    {"push_many", (PyCFunction)BufferQueue_dopush_many, 
        METH_VARARGS | METH_KEYWORDS, BufferQueue_doc_push_many},
    {"pop", (PyCFunction)BufferQueue_dopop, METH_VARARGS | METH_KEYWORDS, 
        BufferQueue_doc_pop},
    {"pop_atmost", (PyCFunction)BufferQueue_dopop_atmost, 
        METH_VARARGS | METH_KEYWORDS, BufferQueue_doc_pop_atmost},
    {"popline", (PyCFunction)BufferQueue_dopopline, METH_NOARGS, 
        BufferQueue_doc_popline},
    {"poplines", (PyCFunction)BufferQueue_dopoplines, METH_NOARGS,
        BufferQueue_doc_poplines},
    {"clear", (PyCFunction)BufferQueue_doclear, METH_NOARGS,
        BufferQueue_doc_clear},
    {NULL}  /* Sentinel */
};

static PyObject *
BufferQueue_repr(BufferQueue *self)
{
    return PyString_FromFormat("<BufferQueue of %zd bytes at %p>",
        self->tot_length, self);
}

static PyObject *
BufferQueue_iter(BufferQueue *self)
{
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *
BufferQueue_iternext(BufferQueue *self)
{
    if (self->delim_size == 0) {
        PyErr_SetString(PyExc_ValueError, "no delimiter");
        return NULL;
    }
    Py_ssize_t out_string_size; 
    if (BufferQueue_find_delim(self, &out_string_size) == -1) {
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }
    return (PyObject *)BufferQueue_pop(self, out_string_size + self->delim_size);
}

static Py_ssize_t
BufferQueue_length(BufferQueue *self)
{
    return self->tot_length;
}

static PySequenceMethods BufferQueue_as_sequence = {
    (lenfunc)BufferQueue_length,
};

static PyTypeObject BufferQueueType = {
    PyObject_HEAD_INIT(NULL)
    0,                          /*ob_size*/
    "qbuf.BufferQueue",         /*tp_name*/
    sizeof(BufferQueue),        /*tp_basicsize*/
    0,                          /*tp_itemsize*/
    (destructor)BufferQueue_dealloc, /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare*/
    (reprfunc)BufferQueue_repr, /*tp_repr*/
    0,                          /*tp_as_number*/
    &BufferQueue_as_sequence,   /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash */
    0,                          /*tp_call*/
    (reprfunc)BufferQueue_repr, /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    BufferQueue_doc,            /* tp_doc */
    0,                          /* tp_traverse */
    0,                          /* tp_clear */
    0,                          /* tp_richcompare */
    0,                          /* tp_weaklistoffset */
    (getiterfunc)BufferQueue_iter, /* tp_iter */
    (iternextfunc)BufferQueue_iternext, /* tp_iternext */
    BufferQueue_methods,        /* tp_methods */
    0,                          /* tp_members */
    BufferQueue_getset,         /* tp_getset */
    0,                          /* tp_base */
    0,                          /* tp_dict */
    0,                          /* tp_descr_get */
    0,                          /* tp_descr_set */
    0,                          /* tp_dictoffset */
    (initproc)BufferQueue_init, /* tp_init */
    0,                          /* tp_alloc */
    (newfunc)BufferQueue_new,   /* tp_new */
};

static PyMethodDef ringbuf_methods[] = {
    {NULL}  /* Sentinel */
};

#ifndef PyMODINIT_FUNC  /* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif

PyMODINIT_FUNC
init_qbuf(void) 
{
    PyObject* m;
    
    if (PyType_Ready(&BufferQueueType) < 0)
        return;

    m = Py_InitModule3("_qbuf", ringbuf_methods,
        "C implementations of things in the qbuf package.");

    Py_INCREF(&BufferQueueType);
    PyModule_AddObject(m, "BufferQueue", (PyObject *)&BufferQueueType);
    
    qbuf_underflow = PyErr_NewException("qbuf.BufferUnderflow", NULL, NULL);
	if (qbuf_underflow == NULL)
		return;
	Py_INCREF(qbuf_underflow);
	PyModule_AddObject(m, "BufferUnderflow", qbuf_underflow);
    
}
