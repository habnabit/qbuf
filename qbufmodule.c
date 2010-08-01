#define PY_SSIZE_T_CLEAN
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <Python.h>
#include "structmember.h"

#ifndef Py_RETURN_NONE
#  define Py_RETURN_NONE do { Py_INCREF(Py_None); return Py_None; } while (0)
#endif

#ifndef Py_CLEAR
#  define Py_CLEAR(x) do { \
        if (x) { \
            PyObject *_clear_tmp = (PyObject *)x; \
            (x) = NULL; \
            Py_XDECREF(_clear_tmp); \
        } \
    } while (0)
#endif

#ifndef PyMODINIT_FUNC
#  define PyMODINIT_FUNC void
#endif

#if PY_VERSION_HEX < 0x02050000
  typedef int Py_ssize_t;
  typedef int (*lenfunc) (PyObject *);
#  define PyNumber_AsSsize_t(ob, exc) PyInt_AsLong(ob)
#  define ARG_PY_SSIZE_T "i"
#  define FMT_PY_SSIZE_T "%i"
#else
#  define ARG_PY_SSIZE_T "n"
#  define FMT_PY_SSIZE_T "%zd"
#endif

#define INITIAL_BUFFER_SIZE 8

static PyObject *qbuf_underflow;
static PyObject *_struct_obj;

PyDoc_STRVAR(BufferQueue_doc,
"BufferQueue([delimiter])\n\
\n\
Initialize a new buffer. If the delimiter is provided, it can be\n\
used to pop lines off instead of just bytes.\n\
\n\
Iterating over a BufferQueue is the same as repeatedly calling\n\
.popline() on it, except that the delimiter is included in the\n\
string yielded. An empty BufferQueue evaluates to boolean false.\n\
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
    PyObject *delim_obj;
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
BufferQueueIterator_advance_string(BufferQueueIterator *self)
{
    self->char_idx = 0;
    if (++self->string_idx == self->parent->buffer_length)
        self->string_idx = 0;
    if (self->string_idx == self->parent->end_idx)
        return 1;
    else
        BufferQueueIterator_update(self);
    return 0;
}

static int
BufferQueueIterator_advance_char(BufferQueueIterator *self)
{
    if (++self->char_idx == self->s_size)
        return BufferQueueIterator_advance_string(self);
    return 0;
}

static int
BufferQueue_push(BufferQueue *self, PyStringObject *string)
{
    PyStringObject **l_buffer, **l_buffer_end;
    Py_ssize_t width, split;
    if (PyString_GET_SIZE(string) == 0)
        return 0;

    if (self->n_items == self->buffer_length) {
        l_buffer = PyMem_New(PyStringObject *, self->buffer_length * 2);
        if (!l_buffer) {
            PyErr_SetString(PyExc_MemoryError, "failed to alloc bigger buffer");
            return -1;
        }
        l_buffer_end = l_buffer + self->buffer_length;
        width = sizeof(self->buffer);
        if (self->start_idx == 0 && self->end_idx == 0) {
            memcpy(l_buffer_end, self->buffer, self->buffer_length * width);
        } else {
            split = self->buffer_length - self->start_idx;
            memcpy(l_buffer_end, self->buffer + self->start_idx, split * width);
            memcpy(l_buffer_end + split, self->buffer, self->end_idx * width);
        }
        PyMem_Free(self->buffer);
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
    Py_DECREF(self->buffer[self->start_idx]);
    self->cur_offset = 0;
    --self->n_items;
    if (++self->start_idx == self->buffer_length)
        self->start_idx = 0;
}

static PyObject *
BufferQueue_pop(BufferQueue *self, Py_ssize_t length, int as_buffer)
{
    PyObject *ret = NULL, *cur_string;
    Py_ssize_t copied, to_copy, size, delta;
    char *ret_dest;
    if (length == 0) {
        ret = PyString_FromString("");
        goto cleanup;
    }

    cur_string = (PyObject *)self->buffer[self->start_idx];
    if (self->cur_offset == 0 && PyString_GET_SIZE(cur_string) == length) {
        ret = (PyObject *)cur_string;
        Py_INCREF(ret);
        BufferQueue_advance_start(self);
    } else if (as_buffer && self->cur_offset + length <=
            PyString_GET_SIZE(cur_string)) {
        if (!(ret = PyBuffer_FromObject(cur_string, self->cur_offset, length)))
            return NULL;
        if (self->cur_offset + length == PyString_GET_SIZE(cur_string))
            BufferQueue_advance_start(self);
        else
            self->cur_offset += length;
    } else if (self->cur_offset + length == PyString_GET_SIZE(cur_string)) {
        if (!(ret = PyString_FromStringAndSize(
                PyString_AS_STRING(cur_string) + self->cur_offset, length)))
            return NULL;
        BufferQueue_advance_start(self);
    } else {
        if (!(ret = PyString_FromStringAndSize(NULL, length)))
            return NULL;
        ret_dest = PyString_AS_STRING(ret);
        copied = 0;
        while (copied < length) {
            to_copy = length - copied;
            size = PyString_GET_SIZE(cur_string);
            if (to_copy + self->cur_offset >= size) {
                delta = size - self->cur_offset;
                memcpy(ret_dest + copied,
                    PyString_AS_STRING(cur_string) + self->cur_offset, delta);
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

cleanup:
    if (ret && as_buffer && !PyBuffer_Check(ret)) {
        PyObject *tmp = ret;
        /* Implicitly returns NULL on error. */
        ret = PyBuffer_FromObject(tmp, 0, Py_END_OF_BUFFER);
        Py_DECREF(tmp);
    }
    return ret;
}

static int
BufferQueue_find_delim(BufferQueue *self, PyStringObject *delim_obj)
{
    BufferQueueIterator iter, split_iter;
    Py_ssize_t pos = 0, i, offset, delim_pos;
    PyObject *tmp = NULL;
    char *delimiter = PyString_AS_STRING(delim_obj);
    Py_ssize_t delim_size = PyString_GET_SIZE(delim_obj);
    if (delim_size > self->tot_length)
        return -1;

    BufferQueueIterator_init(&iter, self);
    do {
        if (!(tmp = PyObject_CallMethod((PyObject *)iter.cur_string, "find",
                "O" ARG_PY_SSIZE_T, delim_obj, iter.char_idx)))
            goto cleanup;
        if ((delim_pos = PyNumber_AsSsize_t(tmp, PyExc_OverflowError)) == -1
                && PyErr_Occurred())
            goto cleanup;
        Py_CLEAR(tmp);
        if (delim_pos != -1)
            return pos + delim_pos - iter.char_idx;

        pos += iter.s_size - iter.char_idx;
        for (offset = 1; offset < delim_size; ++offset) {
            if (pos + offset > self->tot_length)
                break;
            split_iter = iter;
            split_iter.char_idx = split_iter.s_size - delim_size + offset;
            for (i = 0; i < delim_size; ++i) {
                if (delimiter[i] != split_iter.s_ptr[split_iter.char_idx])
                    break;
                BufferQueueIterator_advance_char(&split_iter);
            }
            if (i == delim_size)
                return pos - delim_size + offset;
        }
    } while(!BufferQueueIterator_advance_string(&iter));

cleanup:
    Py_XDECREF(tmp);
    return -1;
}

static int
BufferQueue_popline(BufferQueue *self, PyStringObject **ret,
        PyStringObject *delim_obj)
{
    PyObject *line, *delim;
    Py_ssize_t line_size;
    if (!delim_obj)
        delim_obj = (PyStringObject *)self->delim_obj;
    if (!delim_obj || !PyString_GET_SIZE(delim_obj)) {
        PyErr_SetString(PyExc_ValueError, "no delimiter");
        return -1;
    }
    if ((line_size = BufferQueue_find_delim(self, delim_obj)) == -1)
        return 0;

    if (!(line = BufferQueue_pop(self, line_size, 0)))
        return -1;
    if (!(delim = BufferQueue_pop(self, PyString_GET_SIZE(delim_obj), 0)))
        return -1;
    Py_DECREF(delim);
    *ret = (PyStringObject *)line;
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
    Py_CLEAR(self->delim_obj);
    self->ob_type->tp_free((PyObject *)self);
}

static PyObject *
BufferQueue_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    BufferQueue *self;

    if ((self = (BufferQueue *)type->tp_alloc(type, 0))) {
        self->buffer = NULL;
        self->start_idx = self->end_idx = 0;
        self->delim_obj = NULL;
        self->n_items = self->tot_length = self->cur_offset = 0;
    }

    return (PyObject *)self;
}

static int BufferQueue_setdelim(BufferQueue *, PyObject *, void *);

static int
BufferQueue_init(BufferQueue *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"delimiter", NULL};
    PyObject *delim_tmp = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &delim_tmp))
        return -1;

    if (BufferQueue_setdelim(self, delim_tmp, NULL) == -1)
        return -1;

    self->buffer_length = INITIAL_BUFFER_SIZE;
    if (!(self->buffer = PyMem_New(PyStringObject *, self->buffer_length))) {
        Py_XDECREF(self->delim_obj);
        PyErr_SetString(PyExc_MemoryError, "malloc of buffer failed");
        return -1;
    }

    return 0;
}

static PyObject *
BufferQueue_getdelim(BufferQueue *self, void *closure)
{
    if (!self->delim_obj)
        Py_RETURN_NONE;
    else {
        Py_INCREF(self->delim_obj);
        return self->delim_obj;
    }
}

static int
BufferQueue_setdelim(BufferQueue *self, PyObject *value, void *closure)
{
    if (!PyString_Check(value) && value != Py_None) {
        PyErr_SetString(PyExc_TypeError, "delimiter must be a string or None");
        return -1;
    }
    Py_XDECREF(self->delim_obj);
    if (value == Py_None || !PyString_GET_SIZE(value)) {
        self->delim_obj = NULL;
        return 0;
    }
    self->delim_obj = value;
    Py_INCREF(self->delim_obj);
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
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "S:push", kwlist,
            &in_string))
        return NULL;
    if (BufferQueue_push(self, in_string) == -1)
        return NULL;
    if (PyString_GET_SIZE(in_string))
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
    PyObject *iter = NULL, *item = NULL, *ret = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O:push_many", kwlist,
            &iter))
        goto cleanup;
    if (!(iter = PyObject_GetIter(iter)))
        goto cleanup;

    while ((item = PyIter_Next(iter))) {
        if (!PyString_Check(item)) {
            PyErr_Format(PyExc_ValueError, "push_many() requires an iterable "
                "of strings (got %.50s instead)", item->ob_type->tp_name);
            goto cleanup;
        }
        if (BufferQueue_push(self, (PyStringObject *)item) == -1)
            goto cleanup;
    }
    item = NULL;

    if (PyErr_Occurred())
        goto cleanup;

    ret = Py_None;
    Py_INCREF(ret);

cleanup:
    Py_XDECREF(iter);
    Py_XDECREF(item);
    return ret;
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
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|" ARG_PY_SSIZE_T ":pop",
            kwlist, &out_string_size))
        return NULL;
    if (out_string_size < 0) {
        PyErr_SetString(PyExc_ValueError, "tried to pop a negative number of "
            "bytes from buffer");
        return NULL;
    } else if (out_string_size > self->tot_length) {
        PyErr_Format(qbuf_underflow, "buffer underflow: currently at "
            FMT_PY_SSIZE_T " bytes, tried to pop " FMT_PY_SSIZE_T " bytes",
            self->tot_length, out_string_size);
        return NULL;
    }
    return BufferQueue_pop(self, out_string_size, 0);
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
    if (!PyArg_ParseTupleAndKeywords(args, kwds, ARG_PY_SSIZE_T ":pop_atmost",
            kwlist, &out_string_size))
        return NULL;
    if (out_string_size < 0) {
        PyErr_SetString(PyExc_ValueError, "tried to pop a negative number of "
            "bytes from buffer");
        return NULL;
    }
    if (out_string_size > self->tot_length)
        out_string_size = self->tot_length;
    return BufferQueue_pop(self, out_string_size, 0);
}

PyDoc_STRVAR(BufferQueue_doc_pop_view,
"pop_view([length]) -> buffer\n\
\n\
Pop some bytes from the buffer and return it as a python 'buffer'\n\
object. If possible, no new strings will be constructed and the\n\
buffer returned will just be a view of one of the strings pushed\n\
into the buffer.\n\
");

static PyObject *
BufferQueue_dopop_view(BufferQueue *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"length", NULL};
    Py_ssize_t out_string_size = self->tot_length;
    if (!PyArg_ParseTupleAndKeywords(args, kwds,
            "|" ARG_PY_SSIZE_T ":pop_view", kwlist, &out_string_size))
        return NULL;
    if (out_string_size < 0) {
        PyErr_SetString(PyExc_ValueError, "tried to pop a negative number of "
            "bytes from buffer");
        return NULL;
    } else if (out_string_size > self->tot_length) {
        PyErr_Format(qbuf_underflow, "buffer underflow: currently at "
            FMT_PY_SSIZE_T " bytes, tried to pop " FMT_PY_SSIZE_T " bytes",
            self->tot_length, out_string_size);
        return NULL;
    }
    return BufferQueue_pop(self, out_string_size, 1);
}

PyDoc_STRVAR(BufferQueue_doc_pop_struct,
"pop_struct(format) -> tuple\n\
\n\
Pop some bytes from the buffer and unpack them using the 'struct'\n\
module, returning the resulting tuple. The format string passed is\n\
the same as the 'struct' module format.\n\
");

static PyObject *
BufferQueue_dopop_struct(BufferQueue *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"format", NULL};
    PyObject *format, *struct_obj = NULL, *tmp = NULL, *ret = NULL;
    Py_ssize_t fmt_length;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O:pop_struct",
            kwlist, &format))
        goto cleanup;
    if (!(struct_obj = PyObject_CallFunction(_struct_obj, "O", format)))
        goto cleanup;
    if (!(tmp = PyObject_GetAttrString(struct_obj, "size")))
        goto cleanup;
    if ((fmt_length = PyNumber_AsSsize_t(tmp, PyExc_OverflowError)) == -1
            && PyErr_Occurred())
        goto cleanup;
    Py_CLEAR(tmp);
    if (fmt_length > self->tot_length) {
        PyErr_Format(qbuf_underflow, "buffer underflow: currently at "
            FMT_PY_SSIZE_T " bytes; this struct format requires "
            FMT_PY_SSIZE_T " bytes",
            self->tot_length, fmt_length);
        goto cleanup;
    } else if (fmt_length < 0) {
        PyErr_Format(PyExc_ValueError,
            "got a negative length from struct.calcsize");
        goto cleanup;
    }
    if (!(tmp = BufferQueue_pop(self, fmt_length, 1)))
        goto cleanup;
    if (!(ret = PyObject_CallMethod(struct_obj, "unpack_from", "O", tmp)))
        goto cleanup;

cleanup:
    Py_XDECREF(struct_obj);
    Py_XDECREF(tmp);
    return ret;
}

PyDoc_STRVAR(BufferQueue_doc_popline,
"popline([delimiter]) -> str\n\
\n\
Pop one line of data from the buffer. This scans the buffer for\n\
the next occurrence of the provided delimiter, or the buffer's\n\
delimiter if none was provided, and then returns everything up\n\
to and including the delimiter. If the delimiter was not found\n\
or there was no delimiter set, a ValueError is raised. The \n\
delimiter is not included in the string returned.\n\
");

static PyObject *
BufferQueue_dopopline(BufferQueue *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"delimiter", NULL};
    PyObject *delim_obj = Py_None;
    PyStringObject *ret;
    int result;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O:popline", kwlist,
            &delim_obj))
        return NULL;
    if (delim_obj != Py_None && !PyString_Check(delim_obj))
        return NULL;
    result = BufferQueue_popline(self, &ret,
        (delim_obj == Py_None)? NULL : (PyStringObject *)delim_obj);
    if (result == -1)
        return NULL;
    else if (result == 0) {
        PyErr_SetString(PyExc_ValueError, "delimiter not found");
        return NULL;
    }
    return (PyObject *)ret;
}

PyDoc_STRVAR(BufferQueue_doc_poplines,
"poplines([delimiter]) -> list\n\
\n\
Pop as many lines off of the buffer as is possible. This will\n\
collect and return a list of all of the lines that were in the\n\
buffer. If there was no delimiter set and no delimiter was \n\
provided, a ValueError is raised. The delimiter is not included\n\
in the strings returned.\n\
");

static PyObject *
BufferQueue_dopoplines(BufferQueue *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"delimiter", NULL};
    PyObject *ret, *delim_obj = Py_None;
    PyStringObject *ret_str;
    int result;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O:popline", kwlist,
            &delim_obj))
        return NULL;
    if (delim_obj != Py_None && !PyString_Check(delim_obj))
        return NULL;
    ret = PyList_New(0);
    if (ret == NULL)
        return NULL;
    if (delim_obj == Py_None)
        delim_obj = NULL;
    while ((result = BufferQueue_popline(
            self, &ret_str, (PyStringObject *)delim_obj)) == 1) {
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
    {"push", (PyCFunction)BufferQueue_dopush,
        METH_VARARGS | METH_KEYWORDS, BufferQueue_doc_push},
    {"push_many", (PyCFunction)BufferQueue_dopush_many,
        METH_VARARGS | METH_KEYWORDS, BufferQueue_doc_push_many},
    {"pop", (PyCFunction)BufferQueue_dopop,
        METH_VARARGS | METH_KEYWORDS, BufferQueue_doc_pop},
    {"pop_atmost", (PyCFunction)BufferQueue_dopop_atmost,
        METH_VARARGS | METH_KEYWORDS, BufferQueue_doc_pop_atmost},
    {"pop_view", (PyCFunction)BufferQueue_dopop_view,
        METH_VARARGS | METH_KEYWORDS, BufferQueue_doc_pop_view},
    {"pop_struct", (PyCFunction)BufferQueue_dopop_struct,
        METH_VARARGS | METH_KEYWORDS, BufferQueue_doc_pop_struct},
    {"popline", (PyCFunction)BufferQueue_dopopline,
        METH_VARARGS | METH_KEYWORDS, BufferQueue_doc_popline},
    {"poplines", (PyCFunction)BufferQueue_dopoplines,
        METH_VARARGS | METH_KEYWORDS, BufferQueue_doc_poplines},
    {"clear", (PyCFunction)BufferQueue_doclear,
        METH_NOARGS, BufferQueue_doc_clear},
    {NULL}  /* Sentinel */
};

static PyObject *
BufferQueue_repr(BufferQueue *self)
{
    return PyString_FromFormat("<BufferQueue of " FMT_PY_SSIZE_T " bytes "
        "at %p>", self->tot_length, self);
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
    Py_ssize_t out_string_size;
    if (!self->delim_obj) {
        PyErr_SetString(PyExc_ValueError, "no delimiter");
        return NULL;
    }

    if ((out_string_size = BufferQueue_find_delim(self,
            (PyStringObject *)self->delim_obj)) == -1) {
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }
    return BufferQueue_pop(self, 
        out_string_size + PyString_GET_SIZE(self->delim_obj), 0);
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

static PyMethodDef qbuf_methods[] = {
    {NULL}  /* Sentinel */
};

PyMODINIT_FUNC
init_qbuf(void)
{
    PyObject *m, *_struct = NULL;

    if (!(m = Py_InitModule3("_qbuf", qbuf_methods,
            "C implementations of things in the qbuf package.")))
        goto cleanup;

    if (PyType_Ready(&BufferQueueType) < 0)
        goto cleanup;
    Py_INCREF(&BufferQueueType);
    PyModule_AddObject(m, "BufferQueue", (PyObject *)&BufferQueueType);

    if (!(qbuf_underflow = PyErr_NewException(
            "qbuf.BufferUnderflow", NULL, NULL)))
        goto cleanup;
    Py_INCREF(qbuf_underflow);
    PyModule_AddObject(m, "BufferUnderflow", qbuf_underflow);

    if (!(_struct = PyImport_ImportModule("struct")))
        goto cleanup;
    if (!(_struct_obj = PyObject_GetAttrString(_struct, "Struct")))
        goto cleanup;

cleanup:
    Py_XDECREF(_struct);
}
