#include "immuset.h"

/* -------------------------------------------------------------------------
   ImmuSet Type
   ------------------------------------------------------------------------- */

static void immuset_dealloc(ImmuSet *self) {
    Py_XDECREF(self->root);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* immuset_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    ImmuSet *self = (ImmuSet*)type->tp_alloc(type, 0);
    if (!self) return NULL;
    self->root = NULL;
    self->len = 0;
    self->hash = -1;
    return (PyObject*)self;
}

static int immuset_init(ImmuSet *self, PyObject *args, PyObject *kwds) {
    PyObject *iterable = NULL;
    if (!PyArg_UnpackTuple(args, "ImmuSet", 0, 1, &iterable))
        return -1;
    if (iterable) {
        PyObject *it = PyObject_GetIter(iterable);
        if (!it) return -1;
        PyObject *key;
        while ((key = PyIter_Next(it))) {
            int added = 0;
            uint32_t hash = PyObject_Hash(key);
            if (hash == (uint32_t)-1) { Py_DECREF(it); Py_DECREF(key); return -1; }
            SetNode *new_root = _set_node_assoc(self->root, hash, key, &added, 0);
            if (!new_root) { Py_DECREF(it); Py_DECREF(key); return -1; }
            Py_XDECREF(self->root);
            self->root = new_root;
            self->len += added;
            Py_DECREF(key);
        }
        Py_DECREF(it);
    }
    return 0;
}

static Py_ssize_t immuset_length(ImmuSet *self) {
    return self->len;
}

static int immuset_contains(ImmuSet *self, PyObject *key) {
    uint32_t hash = PyObject_Hash(key);
    if (hash == (uint32_t)-1) return -1;
    return _set_node_find(self->root, hash, key, 0);
}

static PySequenceMethods immuset_as_sequence = {
    .sq_length = (lenfunc)immuset_length,
    .sq_contains = (objobjproc)immuset_contains,
};

static PyObject* immuset_add(ImmuSet *self, PyObject *key) {
    ImmuSet *newset = (ImmuSet*)immuset_new(&ImmuSet_Type, NULL, NULL);
    if (!newset) return NULL;
    newset->root = self->root;
    Py_XINCREF(self->root);
    newset->len = self->len;
    int added = 0;
    uint32_t hash = PyObject_Hash(key);
    if (hash == (uint32_t)-1) { Py_DECREF(newset); return NULL; }
    SetNode *new_root = _set_node_assoc(self->root, hash, key, &added, 0);
    if (!new_root) { Py_DECREF(newset); return NULL; }
    Py_XDECREF(newset->root);
    newset->root = new_root;
    newset->len += added;
    return (PyObject*)newset;
}

static PyObject* immuset_discard(ImmuSet *self, PyObject *key) {
    ImmuSet *newset = (ImmuSet*)immuset_new(&ImmuSet_Type, NULL, NULL);
    if (!newset) return NULL;
    newset->root = self->root;
    Py_XINCREF(self->root);
    newset->len = self->len;
    int removed = 0;
    uint32_t hash = PyObject_Hash(key);
    if (hash == (uint32_t)-1) { Py_DECREF(newset); return NULL; }
    SetNode *new_root = _set_node_without(self->root, hash, key, &removed, 0);
    if (!new_root) { Py_DECREF(newset); return NULL; }
    Py_XDECREF(newset->root);
    newset->root = new_root;
    newset->len -= removed;
    return (PyObject*)newset;
}

static PyObject* immuset_len_method(ImmuSet *self, PyObject *Py_UNUSED(ignored)) {
    return PyLong_FromSsize_t(self->len);
}

static PyObject* immuset_contains_method(ImmuSet *self, PyObject *key) {
    int res = immuset_contains(self, key);
    if (res < 0) return NULL;
    return PyBool_FromLong(res);
}

static Py_hash_t immuset_hash(ImmuSet *self) {
    if (self->hash == -1) {
        Py_hash_t h = 0;
        PyObject *iter = PyObject_GetIter((PyObject*)self);
        if (!iter) return -1;
        PyObject *key;
        while ((key = PyIter_Next(iter))) {
            Py_hash_t kh = PyObject_Hash(key);
            if (kh == -1) { Py_DECREF(iter); Py_DECREF(key); return -1; }
            h ^= kh;
            Py_DECREF(key);
        }
        Py_DECREF(iter);
        if (h == -1) h = -2;
        self->hash = h;
    }
    return self->hash;
}

static PyObject* immuset_iter(PyObject *self) {
    ImmuSet *set = (ImmuSet*)self;
    PyObject *keys = PyList_New(0);
    if (!keys) return NULL;
    if (_set_node_collect_keys(set->root, keys, 0) < 0) {
        Py_DECREF(keys);
        return NULL;
    }
    return PyObject_GetIter(keys);
}

static PyObject* immuset_repr(ImmuSet *self) {
    PyObject *repr = PyUnicode_FromString("<immuset.ImmuSet({");
    if (!repr) return NULL;
    PyObject *iter = PyObject_GetIter((PyObject*)self);
    if (!iter) { Py_DECREF(repr); return NULL; }
    int first = 1;
    PyObject *key;
    while ((key = PyIter_Next(iter))) {
        PyObject *key_repr = PyObject_Repr(key);
        if (!key_repr) { Py_DECREF(iter); Py_DECREF(repr); Py_DECREF(key); return NULL; }
        if (!first) PyUnicode_AppendAndDel(&repr, PyUnicode_FromString(", "));
        PyUnicode_AppendAndDel(&repr, key_repr);
        first = 0;
        Py_DECREF(key);
    }
    Py_DECREF(iter);
    PyUnicode_AppendAndDel(&repr, PyUnicode_FromString("})"));
    return repr;
}

static PyObject* immuset_richcompare(PyObject *self, PyObject *other, int op) {
    if (!PyObject_TypeCheck(other, &ImmuSet_Type))
        Py_RETURN_NOTIMPLEMENTED;
    ImmuSet *a = (ImmuSet*)self;
    ImmuSet *b = (ImmuSet*)other;
    if (op == Py_EQ) {
        if (a->len != b->len) Py_RETURN_FALSE;
        PyObject *iter = PyObject_GetIter((PyObject*)a);
        if (!iter) return NULL;
        PyObject *key;
        while ((key = PyIter_Next(iter))) {
            if (!_set_node_find(b->root, PyObject_Hash(key), key, 0)) {
                Py_DECREF(iter); Py_DECREF(key);
                Py_RETURN_FALSE;
            }
            Py_DECREF(key);
        }
        Py_DECREF(iter);
        Py_RETURN_TRUE;
    } else if (op == Py_NE) {
        if (a->len != b->len) Py_RETURN_TRUE;
        PyObject *iter = PyObject_GetIter((PyObject*)a);
        if (!iter) return NULL;
        PyObject *key;
        while ((key = PyIter_Next(iter))) {
            if (!_set_node_find(b->root, PyObject_Hash(key), key, 0)) {
                Py_DECREF(iter); Py_DECREF(key);
                Py_RETURN_TRUE;
            }
            Py_DECREF(key);
        }
        Py_DECREF(iter);
        Py_RETURN_FALSE;
    } else {
        Py_RETURN_NOTIMPLEMENTED;
    }
}

/* Set operations */
static PyObject* immuset_union(ImmuSet *self, PyObject *other) {
    if (!PyObject_TypeCheck(other, &ImmuSet_Type)) {
        PyErr_SetString(PyExc_TypeError, "can only union with ImmuSet");
        return NULL;
    }
    ImmuSet *other_set = (ImmuSet*)other;
    SetNode *new_root = _set_node_union(self->root, other_set->root, 0);
    if (!new_root) {
        PyObject *empty = immuset_new(&ImmuSet_Type, NULL, NULL);
        return empty ? empty : NULL;
    }
    ImmuSet *newset = (ImmuSet*)immuset_new(&ImmuSet_Type, NULL, NULL);
    if (!newset) {
        _set_node_dealloc(new_root);
        return NULL;
    }
    newset->root = new_root;
    newset->len = new_root->size;
    return (PyObject*)newset;
}

static PyObject* immuset_intersection(ImmuSet *self, PyObject *other) {
    if (!PyObject_TypeCheck(other, &ImmuSet_Type)) {
        PyErr_SetString(PyExc_TypeError, "can only intersect with ImmuSet");
        return NULL;
    }
    ImmuSet *other_set = (ImmuSet*)other;
    SetNode *new_root = _set_node_intersection(self->root, other_set->root, 0);
    if (!new_root) {
        PyObject *empty = immuset_new(&ImmuSet_Type, NULL, NULL);
        return empty ? empty : NULL;
    }
    ImmuSet *newset = (ImmuSet*)immuset_new(&ImmuSet_Type, NULL, NULL);
    if (!newset) {
        _set_node_dealloc(new_root);
        return NULL;
    }
    newset->root = new_root;
    newset->len = new_root->size;
    return (PyObject*)newset;
}

static PyObject* immuset_difference(ImmuSet *self, PyObject *other) {
    if (!PyObject_TypeCheck(other, &ImmuSet_Type)) {
        PyErr_SetString(PyExc_TypeError, "can only difference with ImmuSet");
        return NULL;
    }
    ImmuSet *other_set = (ImmuSet*)other;
    SetNode *new_root = _set_node_difference(self->root, other_set->root, 0);
    if (!new_root) {
        PyObject *empty = immuset_new(&ImmuSet_Type, NULL, NULL);
        return empty ? empty : NULL;
    }
    ImmuSet *newset = (ImmuSet*)immuset_new(&ImmuSet_Type, NULL, NULL);
    if (!newset) {
        _set_node_dealloc(new_root);
        return NULL;
    }
    newset->root = new_root;
    newset->len = new_root->size;
    return (PyObject*)newset;
}

static PyObject* immuset_reduce(ImmuSet *self, PyObject *Py_UNUSED(ignored)) {
    PyObject *keys = PyList_New(0);
    if (!keys) return NULL;
    if (_set_node_collect_keys(self->root, keys, 0) < 0) {
        Py_DECREF(keys);
        return NULL;
    }
    PyObject *keys_tuple = PyList_AsTuple(keys);
    Py_DECREF(keys);
    if (!keys_tuple) return NULL;
    PyObject *args = PyTuple_Pack(1, keys_tuple);
    Py_DECREF(keys_tuple);
    if (!args) return NULL;
    PyObject *result = Py_BuildValue("(OO)", &ImmuSet_Type, args);
    Py_DECREF(args);
    return result;
}

/* GC support for ImmuSet */
static int immuset_traverse(ImmuSet *self, visitproc visit, void *arg) {
    Py_VISIT(self->root);
    return 0;
}

static int immuset_clear(ImmuSet *self) {
    Py_CLEAR(self->root);
    return 0;
}

/* Mutate method (forward declaration) */
static PyObject* immuset_mutate(ImmuSet *self, PyObject *args);

static PyMethodDef immuset_methods[] = {
    {"add", (PyCFunction)immuset_add, METH_O, "Add element, return new set"},
    {"discard", (PyCFunction)immuset_discard, METH_O, "Remove element if present, return new set"},
    {"union", (PyCFunction)immuset_union, METH_O, "Return union with another set"},
    {"intersection", (PyCFunction)immuset_intersection, METH_O, "Return intersection with another set"},
    {"difference", (PyCFunction)immuset_difference, METH_O, "Return difference with another set"},
    {"__len__", (PyCFunction)immuset_len_method, METH_NOARGS, "Return number of elements"},
    {"__contains__", (PyCFunction)immuset_contains_method, METH_O, "Check membership"},
    {"__reduce__", (PyCFunction)immuset_reduce, METH_NOARGS, "Support pickling"},
    {"mutate", (PyCFunction)immuset_mutate, METH_NOARGS, "Return a mutable context"},
    {NULL, NULL}
};

PyTypeObject ImmuSet_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "immuset.ImmuSet",
    .tp_basicsize = sizeof(ImmuSet),
    .tp_dealloc = (destructor)immuset_dealloc,
    .tp_repr = (reprfunc)immuset_repr,
    .tp_hash = (hashfunc)immuset_hash,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "Immutable set using HAMT",
    .tp_traverse = (traverseproc)immuset_traverse,
    .tp_clear = (inquiry)immuset_clear,
    .tp_methods = immuset_methods,
    .tp_iter = immuset_iter,
    .tp_richcompare = immuset_richcompare,
    .tp_as_sequence = &immuset_as_sequence,
    .tp_init = (initproc)immuset_init,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = immuset_new,
    .tp_free = PyObject_GC_Del,
};

/* -------------------------------------------------------------------------
   ImmuSetMutation Type
   ------------------------------------------------------------------------- */

static PyObject* immuset_mutate(ImmuSet *self, PyObject *args) {
    ImmuSetMutation *mut = (ImmuSetMutation*)ImmuSetMutation_Type.tp_alloc(&ImmuSetMutation_Type, 0);
    if (!mut) return NULL;
    mut->original = self;
    Py_INCREF(self);
    mut->root = self->root;
    Py_XINCREF(self->root);
    mut->len = self->len;
    mut->in_context = 0;
    return (PyObject*)mut;
}

static void immuset_mutation_dealloc(ImmuSetMutation *self) {
    Py_XDECREF(self->original);
    Py_XDECREF(self->root);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* immuset_mutation_add(ImmuSetMutation *self, PyObject *key) {
    int added = 0;
    uint32_t hash = PyObject_Hash(key);
    if (hash == (uint32_t)-1) return NULL;
    SetNode *new_root = _set_node_assoc(self->root, hash, key, &added, 0);
    if (!new_root) return NULL;
    Py_XDECREF(self->root);
    self->root = new_root;
    self->len += added;
    Py_RETURN_NONE;
}

static PyObject* immuset_mutation_discard(ImmuSetMutation *self, PyObject *key) {
    int removed = 0;
    uint32_t hash = PyObject_Hash(key);
    if (hash == (uint32_t)-1) return NULL;
    SetNode *new_root = _set_node_without(self->root, hash, key, &removed, 0);
    if (!new_root) return NULL;
    Py_XDECREF(self->root);
    self->root = new_root;
    self->len -= removed;
    Py_RETURN_NONE;
}

static PyObject* immuset_mutation_finish(ImmuSetMutation *self) {
    ImmuSet *newset = (ImmuSet*)immuset_new(&ImmuSet_Type, NULL, NULL);
    if (!newset) return NULL;
    newset->root = self->root;
    Py_XINCREF(self->root);
    newset->len = self->len;
    return (PyObject*)newset;
}

static PyObject* immuset_mutation_enter(ImmuSetMutation *self, PyObject *args) {
    if (self->in_context) {
        PyErr_SetString(PyExc_RuntimeError, "cannot re-enter context");
        return NULL;
    }
    self->in_context = 1;
    Py_INCREF(self);
    return (PyObject*)self;
}

static PyObject* immuset_mutation_exit(ImmuSetMutation *self, PyObject *args) {
    if (!self->in_context) {
        PyErr_SetString(PyExc_RuntimeError, "not in context");
        return NULL;
    }
    self->in_context = 0;
    Py_RETURN_NONE;
}

/* GC support for ImmuSetMutation */
static int immuset_mutation_traverse(ImmuSetMutation *self, visitproc visit, void *arg) {
    Py_VISIT(self->original);
    Py_VISIT(self->root);
    return 0;
}

static int immuset_mutation_clear(ImmuSetMutation *self) {
    Py_CLEAR(self->original);
    Py_CLEAR(self->root);
    return 0;
}

static PyMethodDef immuset_mutation_methods[] = {
    {"add", (PyCFunction)immuset_mutation_add, METH_O, "Add element"},
    {"discard", (PyCFunction)immuset_mutation_discard, METH_O, "Remove element if present"},
    {"finish", (PyCFunction)immuset_mutation_finish, METH_NOARGS, "Return new immutable set"},
    {"__enter__", (PyCFunction)immuset_mutation_enter, METH_VARARGS, NULL},
    {"__exit__", (PyCFunction)immuset_mutation_exit, METH_VARARGS, NULL},
    {NULL, NULL}
};

PyTypeObject ImmuSetMutation_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "immuset.ImmuSetMutation",
    .tp_basicsize = sizeof(ImmuSetMutation),
    .tp_dealloc = (destructor)immuset_mutation_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "Mutable context for building ImmuSet",
    .tp_traverse = (traverseproc)immuset_mutation_traverse,
    .tp_clear = (inquiry)immuset_mutation_clear,
    .tp_methods = immuset_mutation_methods,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = PyType_GenericNew,
    .tp_free = PyObject_GC_Del,
};

/* -------------------------------------------------------------------------
   Module Initialization
   ------------------------------------------------------------------------- */

static PyModuleDef immuset_module = {
    PyModuleDef_HEAD_INIT,
    "immuset",
    "Immutable set using HAMT",
    -1,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC PyInit_immuset(void) {
    PyObject *mod = PyModule_Create(&immuset_module);
    if (!mod) return NULL;

    /* Initialize internal node types (from hamt_set.c) */
    if (_hamt_set_init_types() < 0)
        return NULL;

    /* Initialize ImmuSet type */
    if (PyType_Ready(&ImmuSet_Type) < 0)
        return NULL;
    Py_INCREF(&ImmuSet_Type);
    PyModule_AddObject(mod, "ImmuSet", (PyObject*)&ImmuSet_Type);

    /* Initialize ImmuSetMutation type */
    if (PyType_Ready(&ImmuSetMutation_Type) < 0)
        return NULL;
    Py_INCREF(&ImmuSetMutation_Type);
    PyModule_AddObject(mod, "ImmuSetMutation", (PyObject*)&ImmuSetMutation_Type);

    return mod;
}

