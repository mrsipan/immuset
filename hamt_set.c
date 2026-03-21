#include "immuset.h"
#include <Python.h>
#include <stddef.h>

/* ---- Helper functions --------------------------------------------------- */

static inline uint32_t popcount32(uint32_t x) {
    x = x - ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x + (x >> 4)) & 0x0F0F0F0F;
    x = (x + (x >> 8)) & 0x00FF00FF;
    x = (x + (x >> 16)) & 0x0000FFFF;
    return x;
}

#define HAMT_SHIFT 5
#define HAMT_MASK  ((1 << HAMT_SHIFT) - 1)

static inline uint32_t hamt_fragment(uint32_t hash, int shift) {
    return (hash >> shift) & HAMT_MASK;
}

static inline int hamt_bitmap_index(uint32_t bitmap, uint32_t bit) {
    return popcount32(bitmap & (bit - 1));
}

/* ---- Node type objects (non-GC) ----------------------------------------- */

PyTypeObject _SetNode_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_SetNode",
    .tp_basicsize = sizeof(SetNode),
    .tp_dealloc = (destructor)_set_node_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,   /* no GC */
};

PyTypeObject _SetBitmapNode_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_SetBitmapNode",
    .tp_basicsize = sizeof(SetBitmapNode),
    .tp_dealloc = (destructor)_set_node_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

PyTypeObject _SetArrayNode_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_SetArrayNode",
    .tp_basicsize = sizeof(SetArrayNode),
    .tp_dealloc = (destructor)_set_node_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

PyTypeObject _SetCollisionNode_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_SetCollisionNode",
    .tp_basicsize = sizeof(SetCollisionNode),
    .tp_dealloc = (destructor)_set_node_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

/* ---- Node creation (non-GC) --------------------------------------------- */

static SetNode* _set_node_new_bitmap(uint32_t bitmap, PyObject *array) {
    SetBitmapNode *node = PyObject_New(SetBitmapNode, &_SetBitmapNode_Type);
    if (!node) return NULL;
    node->base.type = SET_NODE_BITMAP;
    node->base.hash = 0;
    node->base.size = 0;
    node->bitmap = bitmap;
    node->array = array;
    Py_XINCREF(array);
    return (SetNode*)node;
}

static SetNode* _set_node_new_array(uint32_t count, SetNode **children) {
    SetArrayNode *node = PyObject_New(SetArrayNode, &_SetArrayNode_Type);
    if (!node) return NULL;
    node->base.type = SET_NODE_ARRAY;
    node->base.hash = 0;
    node->base.size = 0;
    node->count = count;
    node->children = children;
    return (SetNode*)node;
}

static SetNode* _set_node_new_collision(uint32_t hash, PyObject **keys, uint32_t count) {
    SetCollisionNode *node = PyObject_New(SetCollisionNode, &_SetCollisionNode_Type);
    if (!node) return NULL;
    node->base.type = SET_NODE_COLLISION;
    node->base.hash = hash;
    node->base.size = count;
    node->count = count;
    node->keys = keys;
    for (uint32_t i = 0; i < count; i++) Py_INCREF(keys[i]);
    return (SetNode*)node;
}

void _set_node_dealloc(SetNode *node) {
    if (!node) return;
    switch (node->type) {
        case SET_NODE_BITMAP: {
            SetBitmapNode *bn = (SetBitmapNode*)node;
            Py_XDECREF(bn->array);
            break;
        }
        case SET_NODE_ARRAY: {
            SetArrayNode *an = (SetArrayNode*)node;
            for (uint32_t i = 0; i < an->count; i++)
                Py_XDECREF(an->children[i]);
            PyMem_Free(an->children);
            break;
        }
        case SET_NODE_COLLISION: {
            SetCollisionNode *cn = (SetCollisionNode*)node;
            for (uint32_t i = 0; i < cn->count; i++)
                Py_XDECREF(cn->keys[i]);
            PyMem_Free(cn->keys);
            break;
        }
        default: break;
    }
    PyObject_Del(node);
}

/* ---- Search ------------------------------------------------------------- */

int _set_node_find(SetNode *node, uint32_t hash, PyObject *key, int shift) {
    if (!node) return 0;

    switch (node->type) {
        case SET_NODE_BITMAP: {
            SetBitmapNode *bn = (SetBitmapNode*)node;
            uint32_t frag = hamt_fragment(hash, shift);
            uint32_t bit = 1U << frag;
            if (!(bn->bitmap & bit)) return 0;
            int idx = hamt_bitmap_index(bn->bitmap, bit);
            PyObject *item = PyTuple_GET_ITEM(bn->array, idx);
            if (Py_TYPE(item) == &_SetNode_Type) {
                return _set_node_find((SetNode*)item, hash, key, shift + HAMT_SHIFT);
            } else {
                PyObject *key_item = PyTuple_GET_ITEM(item, 1);
                return PyObject_RichCompareBool(key, key_item, Py_EQ) == 1;
            }
        }
        case SET_NODE_ARRAY: {
            SetArrayNode *an = (SetArrayNode*)node;
            uint32_t frag = hamt_fragment(hash, shift);
            if (frag >= an->count) return 0;
            SetNode *child = an->children[frag];
            return child ? _set_node_find(child, hash, key, shift + HAMT_SHIFT) : 0;
        }
        case SET_NODE_COLLISION: {
            SetCollisionNode *cn = (SetCollisionNode*)node;
            if (cn->base.hash != hash) return 0;
            for (uint32_t i = 0; i < cn->count; i++) {
                if (PyObject_RichCompareBool(key, cn->keys[i], Py_EQ) == 1)
                    return 1;
            }
            return 0;
        }
        default: return 0;
    }
}

/* ---- Insertion (assoc) -------------------------------------------------- */

static SetNode* _assoc_into_bitmap(SetBitmapNode *bn, uint32_t hash,
                                   PyObject *key, int *added, int shift) {
    uint32_t frag = hamt_fragment(hash, shift);
    uint32_t bit = 1U << frag;
    int idx = hamt_bitmap_index(bn->bitmap, bit);
    Py_ssize_t old_len = PyTuple_GET_SIZE(bn->array);
    PyObject *new_array = NULL;
    SetNode *new_node = NULL;

    if (bn->bitmap & bit) {
        PyObject *child = PyTuple_GET_ITEM(bn->array, idx);
        if (Py_TYPE(child) == &_SetNode_Type) {
            SetNode *new_child = _set_node_assoc((SetNode*)child, hash, key, added, shift + HAMT_SHIFT);
            if (!new_child) return NULL;
            if (new_child == (SetNode*)child) {
                Py_INCREF(bn);
                return (SetNode*)bn;
            }
            new_array = PyTuple_New(old_len);
            if (!new_array) { Py_DECREF(new_child); return NULL; }
            for (Py_ssize_t i = 0; i < old_len; i++) {
                PyObject *item = PyTuple_GET_ITEM(bn->array, i);
                if (i == idx)
                    PyTuple_SET_ITEM(new_array, i, (PyObject*)new_child);
                else {
                    Py_INCREF(item);
                    PyTuple_SET_ITEM(new_array, i, item);
                }
            }
            new_node = _set_node_new_bitmap(bn->bitmap, new_array);
            if (!new_node) { Py_DECREF(new_array); return NULL; }
            new_node->size = bn->base.size + (*added);
            return new_node;
        } else {
            /* leaf */
            PyObject *key_item = PyTuple_GET_ITEM(child, 1);
            if (PyObject_RichCompareBool(key, key_item, Py_EQ) == 1) {
                *added = 0;
                Py_INCREF(bn);
                return (SetNode*)bn;
            }
            uint32_t existing_hash = (uint32_t)PyLong_AsUnsignedLong(PyTuple_GET_ITEM(child, 0));
            if (existing_hash == hash) {
                /* collision node */
                PyObject **keys = PyMem_Malloc(2 * sizeof(PyObject*));
                if (!keys) return NULL;
                keys[0] = key_item;
                keys[1] = key;
                Py_INCREF(key_item);
                Py_INCREF(key);
                SetNode *coll = _set_node_new_collision(hash, keys, 2);
                if (!coll) { PyMem_Free(keys); return NULL; }
                coll->size = 2;
                new_array = PyTuple_New(old_len);
                if (!new_array) { Py_DECREF(coll); return NULL; }
                for (Py_ssize_t i = 0; i < old_len; i++) {
                    PyObject *item = PyTuple_GET_ITEM(bn->array, i);
                    if (i == idx)
                        PyTuple_SET_ITEM(new_array, i, (PyObject*)coll);
                    else {
                        Py_INCREF(item);
                        PyTuple_SET_ITEM(new_array, i, item);
                    }
                }
                new_node = _set_node_new_bitmap(bn->bitmap, new_array);
                if (!new_node) { Py_DECREF(new_array); return NULL; }
                *added = 1;
                new_node->size = bn->base.size + 1;
                return new_node;
            } else {
                /* different hash, same fragment */
                int added_inner = 0;
                SetNode *new_child = _set_node_assoc(NULL, existing_hash, key_item, &added_inner, shift + HAMT_SHIFT);
                if (!new_child) return NULL;
                new_child = _set_node_assoc(new_child, hash, key, &added_inner, shift + HAMT_SHIFT);
                if (!new_child) return NULL;
                new_array = PyTuple_New(old_len);
                if (!new_array) { Py_DECREF(new_child); return NULL; }
                for (Py_ssize_t i = 0; i < old_len; i++) {
                    PyObject *item = PyTuple_GET_ITEM(bn->array, i);
                    if (i == idx)
                        PyTuple_SET_ITEM(new_array, i, (PyObject*)new_child);
                    else {
                        Py_INCREF(item);
                        PyTuple_SET_ITEM(new_array, i, item);
                    }
                }
                new_node = _set_node_new_bitmap(bn->bitmap, new_array);
                if (!new_node) { Py_DECREF(new_array); return NULL; }
                *added = 1;
                new_node->size = bn->base.size + 1;
                return new_node;
            }
        }
    } else {
        /* new entry */
        new_array = PyTuple_New(old_len + 1);
        if (!new_array) return NULL;
        int inserted = 0;
        for (Py_ssize_t i = 0; i < old_len; i++) {
            PyObject *item = PyTuple_GET_ITEM(bn->array, i);
            Py_INCREF(item);
            if (!inserted && i == idx) {
                PyObject *leaf = Py_BuildValue("(iO)", hash, key);
                if (!leaf) { Py_DECREF(new_array); return NULL; }
                PyTuple_SET_ITEM(new_array, i, leaf);
                inserted = 1;
                PyTuple_SET_ITEM(new_array, i+1, item);
            } else if (inserted) {
                PyTuple_SET_ITEM(new_array, i+1, item);
            } else {
                PyTuple_SET_ITEM(new_array, i, item);
            }
        }
        if (!inserted) {
            PyObject *leaf = Py_BuildValue("(iO)", hash, key);
            if (!leaf) { Py_DECREF(new_array); return NULL; }
            PyTuple_SET_ITEM(new_array, old_len, leaf);
        }
        uint32_t new_bitmap = bn->bitmap | bit;
        new_node = _set_node_new_bitmap(new_bitmap, new_array);
        if (!new_node) { Py_DECREF(new_array); return NULL; }
        *added = 1;
        new_node->size = bn->base.size + 1;
        return new_node;
    }
}

SetNode* _set_node_assoc(SetNode *node, uint32_t hash, PyObject *key,
                         int *added, int shift) {
    if (!node) {
        PyObject *leaf = Py_BuildValue("(iO)", hash, key);
        if (!leaf) return NULL;
        PyObject *array = PyTuple_Pack(1, leaf);
        Py_DECREF(leaf);
        if (!array) return NULL;
        SetNode *new_node = _set_node_new_bitmap(1U << hamt_fragment(hash, shift), array);
        if (!new_node) { Py_DECREF(array); return NULL; }
        new_node->size = 1;
        *added = 1;
        return new_node;
    }

    switch (node->type) {
        case SET_NODE_BITMAP:
            return _assoc_into_bitmap((SetBitmapNode*)node, hash, key, added, shift);
        case SET_NODE_ARRAY: {
            SetArrayNode *an = (SetArrayNode*)node;
            uint32_t frag = hamt_fragment(hash, shift);
            if (frag >= an->count) {
                uint32_t new_count = an->count;
                while (new_count <= frag) new_count *= 2;
                SetNode **new_children = PyMem_Calloc(new_count, sizeof(SetNode*));
                if (!new_children) return NULL;
                memcpy(new_children, an->children, an->count * sizeof(SetNode*));
                for (uint32_t i = an->count; i < new_count; i++)
                    new_children[i] = NULL;
                SetArrayNode *new_an = (SetArrayNode*)_set_node_new_array(new_count, new_children);
                if (!new_an) { PyMem_Free(new_children); return NULL; }
                new_an->base.size = an->base.size;
                new_an->children = new_children;
                for (uint32_t i = 0; i < an->count; i++) Py_INCREF(an->children[i]);
                Py_DECREF(node);
                node = (SetNode*)new_an;
                an = new_an;
            }
            SetNode *child = an->children[frag];
            SetNode *new_child = _set_node_assoc(child, hash, key, added, shift + HAMT_SHIFT);
            if (!new_child) return NULL;
            if (new_child == child) {
                Py_INCREF(node);
                return node;
            }
            SetNode **new_children = PyMem_Malloc(an->count * sizeof(SetNode*));
            if (!new_children) { Py_DECREF(new_child); return NULL; }
            memcpy(new_children, an->children, an->count * sizeof(SetNode*));
            Py_XINCREF(new_child);
            Py_XDECREF(an->children[frag]);
            new_children[frag] = new_child;
            SetArrayNode *new_an = (SetArrayNode*)_set_node_new_array(an->count, new_children);
            if (!new_an) { PyMem_Free(new_children); Py_DECREF(new_child); return NULL; }
            new_an->base.size = an->base.size + (*added);
            return (SetNode*)new_an;
        }
        case SET_NODE_COLLISION: {
            SetCollisionNode *cn = (SetCollisionNode*)node;
            if (cn->base.hash != hash) {
                return _set_node_assoc(NULL, hash, key, added, shift);
            }
            for (uint32_t i = 0; i < cn->count; i++) {
                if (PyObject_RichCompareBool(key, cn->keys[i], Py_EQ) == 1) {
                    *added = 0;
                    Py_INCREF(node);
                    return node;
                }
            }
            PyObject **new_keys = PyMem_Malloc((cn->count + 1) * sizeof(PyObject*));
            if (!new_keys) return NULL;
            for (uint32_t i = 0; i < cn->count; i++) {
                new_keys[i] = cn->keys[i];
                Py_INCREF(cn->keys[i]);
            }
            new_keys[cn->count] = key;
            Py_INCREF(key);
            SetNode *new_node = _set_node_new_collision(hash, new_keys, cn->count + 1);
            if (!new_node) { PyMem_Free(new_keys); return NULL; }
            new_node->size = cn->base.size + 1;
            *added = 1;
            return new_node;
        }
        default:
            PyErr_SetString(PyExc_RuntimeError, "unknown node type");
            return NULL;
    }
}

/* ---- Deletion (without) ------------------------------------------------- */

static SetNode* _without_into_bitmap(SetBitmapNode *bn, uint32_t hash,
                                     PyObject *key, int *removed, int shift) {
    uint32_t frag = hamt_fragment(hash, shift);
    uint32_t bit = 1U << frag;
    if (!(bn->bitmap & bit)) {
        *removed = 0;
        Py_INCREF(bn);
        return (SetNode*)bn;
    }
    int idx = hamt_bitmap_index(bn->bitmap, bit);
    PyObject *child = PyTuple_GET_ITEM(bn->array, idx);
    Py_ssize_t old_len = PyTuple_GET_SIZE(bn->array);

    if (Py_TYPE(child) == &_SetNode_Type) {
        SetNode *new_child = _set_node_without((SetNode*)child, hash, key, removed, shift + HAMT_SHIFT);
        if (!new_child) return NULL;
        if (new_child == (SetNode*)child) {
            Py_INCREF(bn);
            return (SetNode*)bn;
        }
        if (new_child == NULL) {
            if (old_len == 1) {
                *removed = 1;
                return NULL;
            }
            PyObject *new_array = PyTuple_New(old_len - 1);
            if (!new_array) return NULL;
            uint32_t new_bitmap = bn->bitmap & ~bit;
            int j = 0;
            for (Py_ssize_t i = 0; i < old_len; i++) {
                if (i == idx) continue;
                PyObject *item = PyTuple_GET_ITEM(bn->array, i);
                Py_INCREF(item);
                PyTuple_SET_ITEM(new_array, j++, item);
            }
            SetNode *new_node = _set_node_new_bitmap(new_bitmap, new_array);
            if (!new_node) { Py_DECREF(new_array); return NULL; }
            new_node->size = bn->base.size - 1;
            *removed = 1;
            return new_node;
        } else {
            PyObject *new_array = PyTuple_New(old_len);
            if (!new_array) { Py_DECREF(new_child); return NULL; }
            for (Py_ssize_t i = 0; i < old_len; i++) {
                PyObject *item = PyTuple_GET_ITEM(bn->array, i);
                if (i == idx)
                    PyTuple_SET_ITEM(new_array, i, (PyObject*)new_child);
                else {
                    Py_INCREF(item);
                    PyTuple_SET_ITEM(new_array, i, item);
                }
            }
            SetNode *new_node = _set_node_new_bitmap(bn->bitmap, new_array);
            if (!new_node) { Py_DECREF(new_array); return NULL; }
            new_node->size = bn->base.size + (*removed);
            return new_node;
        }
    } else {
        PyObject *key_item = PyTuple_GET_ITEM(child, 1);
        if (PyObject_RichCompareBool(key, key_item, Py_EQ) == 1) {
            if (old_len == 1) {
                *removed = 1;
                return NULL;
            }
            PyObject *new_array = PyTuple_New(old_len - 1);
            if (!new_array) return NULL;
            uint32_t new_bitmap = bn->bitmap & ~bit;
            int j = 0;
            for (Py_ssize_t i = 0; i < old_len; i++) {
                if (i == idx) continue;
                PyObject *item = PyTuple_GET_ITEM(bn->array, i);
                Py_INCREF(item);
                PyTuple_SET_ITEM(new_array, j++, item);
            }
            SetNode *new_node = _set_node_new_bitmap(new_bitmap, new_array);
            if (!new_node) { Py_DECREF(new_array); return NULL; }
            new_node->size = bn->base.size - 1;
            *removed = 1;
            return new_node;
        } else {
            *removed = 0;
            Py_INCREF(bn);
            return (SetNode*)bn;
        }
    }
}

static SetNode* _without_into_array(SetArrayNode *an, uint32_t hash,
                                    PyObject *key, int *removed, int shift) {
    uint32_t frag = hamt_fragment(hash, shift);
    if (frag >= an->count) {
        *removed = 0;
        Py_INCREF(an);
        return (SetNode*)an;
    }
    SetNode *child = an->children[frag];
    if (!child) {
        *removed = 0;
        Py_INCREF(an);
        return (SetNode*)an;
    }
    SetNode *new_child = _set_node_without(child, hash, key, removed, shift + HAMT_SHIFT);
    if (!new_child) return NULL;
    if (new_child == child) {
        Py_INCREF(an);
        return (SetNode*)an;
    }
    SetNode **new_children = PyMem_Malloc(an->count * sizeof(SetNode*));
    if (!new_children) { Py_DECREF(new_child); return NULL; }
    memcpy(new_children, an->children, an->count * sizeof(SetNode*));
    Py_XINCREF(new_child);
    Py_XDECREF(an->children[frag]);
    new_children[frag] = new_child;
    SetArrayNode *new_an = (SetArrayNode*)_set_node_new_array(an->count, new_children);
    if (!new_an) { PyMem_Free(new_children); Py_DECREF(new_child); return NULL; }
    new_an->base.size = an->base.size + (*removed);
    return (SetNode*)new_an;
}

SetNode* _set_node_without(SetNode *node, uint32_t hash, PyObject *key,
                           int *removed, int shift) {
    if (!node) {
        *removed = 0;
        return NULL;
    }

    switch (node->type) {
        case SET_NODE_BITMAP:
            return _without_into_bitmap((SetBitmapNode*)node, hash, key, removed, shift);
        case SET_NODE_ARRAY:
            return _without_into_array((SetArrayNode*)node, hash, key, removed, shift);
        case SET_NODE_COLLISION: {
            SetCollisionNode *cn = (SetCollisionNode*)node;
            if (cn->base.hash != hash) {
                *removed = 0;
                Py_INCREF(node);
                return node;
            }
            for (uint32_t i = 0; i < cn->count; i++) {
                if (PyObject_RichCompareBool(key, cn->keys[i], Py_EQ) == 1) {
                    if (cn->count == 1) {
                        *removed = 1;
                        return NULL;
                    }
                    PyObject **new_keys = PyMem_Malloc((cn->count - 1) * sizeof(PyObject*));
                    if (!new_keys) return NULL;
                    int j = 0;
                    for (uint32_t k = 0; k < cn->count; k++) {
                        if (k == i) continue;
                        new_keys[j++] = cn->keys[k];
                        Py_INCREF(cn->keys[k]);
                    }
                    SetNode *new_node = _set_node_new_collision(hash, new_keys, cn->count - 1);
                    if (!new_node) { PyMem_Free(new_keys); return NULL; }
                    new_node->size = cn->base.size - 1;
                    *removed = 1;
                    return new_node;
                }
            }
            *removed = 0;
            Py_INCREF(node);
            return node;
        }
        default:
            PyErr_SetString(PyExc_RuntimeError, "unknown node type");
            return NULL;
    }
}

/* ---- Type initializer --------------------------------------------------- */

int _hamt_set_init_types(void) {
    if (PyType_Ready(&_SetNode_Type) < 0)
        return -1;
    if (PyType_Ready(&_SetBitmapNode_Type) < 0)
        return -1;
    if (PyType_Ready(&_SetArrayNode_Type) < 0)
        return -1;
    if (PyType_Ready(&_SetCollisionNode_Type) < 0)
        return -1;
    return 0;
}
