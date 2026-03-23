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

static inline int hamt_bitmap_count(uint32_t bitmap) {
    return popcount32(bitmap);
}

/* ---- Node type objects (non‑GC) ----------------------------------------- */

PyTypeObject _SetNode_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_SetNode",
    .tp_basicsize = sizeof(SetNode),
    .tp_dealloc = (destructor)_set_node_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
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

/* ---- Node creation (non‑GC) --------------------------------------------- */

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

/* ---- Key collector for iteration ---------------------------------------- */

int _set_node_collect_keys(SetNode *node, PyObject *list, int shift) {
    if (!node) return 0;
    switch (node->type) {
        case SET_NODE_BITMAP: {
            SetBitmapNode *bn = (SetBitmapNode*)node;
            Py_ssize_t len = PyTuple_GET_SIZE(bn->array);
            for (Py_ssize_t i = 0; i < len; i++) {
                PyObject *item = PyTuple_GET_ITEM(bn->array, i);
                if (Py_TYPE(item) == &_SetNode_Type) {
                    if (_set_node_collect_keys((SetNode*)item, list, shift + HAMT_SHIFT) < 0)
                        return -1;
                } else {
                    PyObject *key = PyTuple_GET_ITEM(item, 1);
                    if (PyList_Append(list, key) < 0)
                        return -1;
                }
            }
            break;
        }
        case SET_NODE_ARRAY: {
            SetArrayNode *an = (SetArrayNode*)node;
            for (uint32_t i = 0; i < an->count; i++) {
                if (an->children[i]) {
                    if (_set_node_collect_keys(an->children[i], list, shift + HAMT_SHIFT) < 0)
                        return -1;
                }
            }
            break;
        }
        case SET_NODE_COLLISION: {
            SetCollisionNode *cn = (SetCollisionNode*)node;
            for (uint32_t i = 0; i < cn->count; i++) {
                if (PyList_Append(list, cn->keys[i]) < 0)
                    return -1;
            }
            break;
        }
        default:
            break;
    }
    return 0;
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
            PyObject *key_item = PyTuple_GET_ITEM(child, 1);
            if (PyObject_RichCompareBool(key, key_item, Py_EQ) == 1) {
                *added = 0;
                Py_INCREF(bn);
                return (SetNode*)bn;
            }
            uint32_t existing_hash = (uint32_t)PyLong_AsUnsignedLong(PyTuple_GET_ITEM(child, 0));
            if (existing_hash == hash) {
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
                int added_inner = 0;
                SetNode *new_child = _set_node_assoc(NULL, existing_hash, key_item, &added_inner, shift + HAMT_SHIFT);
                if (!new_child) return NULL;
                SetNode *new_child2 = _set_node_assoc(new_child, hash, key, &added_inner, shift + HAMT_SHIFT);
                Py_DECREF(new_child);
                if (!new_child2) return NULL;
                new_child = new_child2;
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

/* ---- Set operations (union, intersection, difference) ------------------- */

/* Helper: copy a node (shallow copy with incref) */
static SetNode* _set_node_copy(SetNode *node) {
    if (!node) return NULL;
    switch (node->type) {
        case SET_NODE_BITMAP: {
            SetBitmapNode *bn = (SetBitmapNode*)node;
            Py_ssize_t len = PyTuple_GET_SIZE(bn->array);
            PyObject *new_array = PyTuple_New(len);
            if (!new_array) return NULL;
            for (Py_ssize_t i = 0; i < len; i++) {
                PyObject *item = PyTuple_GET_ITEM(bn->array, i);
                Py_INCREF(item);
                PyTuple_SET_ITEM(new_array, i, item);
            }
            SetNode *copy = _set_node_new_bitmap(bn->bitmap, new_array);
            if (copy) copy->size = node->size;
            return copy;
        }
        case SET_NODE_ARRAY: {
            SetArrayNode *an = (SetArrayNode*)node;
            SetNode **new_children = PyMem_Malloc(an->count * sizeof(SetNode*));
            if (!new_children) return NULL;
            for (uint32_t i = 0; i < an->count; i++) {
                new_children[i] = an->children[i];
                Py_XINCREF(an->children[i]);
            }
            SetNode *copy = _set_node_new_array(an->count, new_children);
            if (copy) copy->size = node->size;
            return copy;
        }
        case SET_NODE_COLLISION: {
            SetCollisionNode *cn = (SetCollisionNode*)node;
            PyObject **new_keys = PyMem_Malloc(cn->count * sizeof(PyObject*));
            if (!new_keys) return NULL;
            for (uint32_t i = 0; i < cn->count; i++) {
                new_keys[i] = cn->keys[i];
                Py_INCREF(cn->keys[i]);
            }
            SetNode *copy = _set_node_new_collision(cn->base.hash, new_keys, cn->count);
            if (copy) copy->size = node->size;
            return copy;
        }
        default:
            return NULL;
    }
}

/* Union helpers (same-type only) */
static SetNode* _union_bitmap_bitmap(SetBitmapNode *a, SetBitmapNode *b, int shift);
static SetNode* _union_array_array(SetArrayNode *a, SetArrayNode *b, int shift);
static SetNode* _union_collision_collision(SetCollisionNode *a, SetCollisionNode *b, int shift);

static SetNode* _union_bitmap_bitmap(SetBitmapNode *a, SetBitmapNode *b, int shift) {
    uint32_t combined_bitmap = a->bitmap | b->bitmap;
    Py_ssize_t new_len = hamt_bitmap_count(combined_bitmap);
    PyObject *new_array = PyTuple_New(new_len);
    if (!new_array) return NULL;

    uint32_t bit = 1;
    int out_idx = 0;
    for (int i = 0; i < 32; i++, bit <<= 1) {
        if (!(combined_bitmap & bit)) continue;
        int a_idx = (a->bitmap & bit) ? hamt_bitmap_index(a->bitmap, bit) : -1;
        int b_idx = (b->bitmap & bit) ? hamt_bitmap_index(b->bitmap, bit) : -1;

        SetNode *child = NULL;
        if (a_idx >= 0 && b_idx >= 0) {
            PyObject *a_child = PyTuple_GET_ITEM(a->array, a_idx);
            PyObject *b_child = PyTuple_GET_ITEM(b->array, b_idx);
            if (Py_TYPE(a_child) == &_SetNode_Type && Py_TYPE(b_child) == &_SetNode_Type) {
                child = _set_node_union((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
            } else if (Py_TYPE(a_child) == &_SetNode_Type) {
                child = _set_node_union((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
            } else if (Py_TYPE(b_child) == &_SetNode_Type) {
                child = _set_node_union((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
            } else {
                /* both leaves */
                PyObject *a_key = PyTuple_GET_ITEM(a_child, 1);
                PyObject *b_key = PyTuple_GET_ITEM(b_child, 1);
                if (PyObject_RichCompareBool(a_key, b_key, Py_EQ) == 1) {
                    Py_INCREF(a_child);
                    child = (SetNode*)a_child;
                } else {
                    /* different keys – create a node containing both */
                    SetNode *temp = _set_node_assoc(NULL,
                        (uint32_t)PyLong_AsUnsignedLong(PyTuple_GET_ITEM(a_child, 0)),
                        a_key, NULL, shift);
                    if (!temp) goto error;
                    child = _set_node_assoc(temp,
                        (uint32_t)PyLong_AsUnsignedLong(PyTuple_GET_ITEM(b_child, 0)),
                        b_key, NULL, shift);
                    Py_DECREF(temp);
                }
            }
        } else if (a_idx >= 0) {
            PyObject *item = PyTuple_GET_ITEM(a->array, a_idx);
            Py_INCREF(item);
            child = (SetNode*)item;
        } else {
            PyObject *item = PyTuple_GET_ITEM(b->array, b_idx);
            Py_INCREF(item);
            child = (SetNode*)item;
        }
        if (!child) goto error;
        PyTuple_SET_ITEM(new_array, out_idx++, (PyObject*)child);
    }

    SetNode *result = _set_node_new_bitmap(combined_bitmap, new_array);
    if (!result) { Py_DECREF(new_array); return NULL; }
    /* compute size */
    result->size = 0;
    for (Py_ssize_t i = 0; i < new_len; i++) {
        PyObject *item = PyTuple_GET_ITEM(new_array, i);
        if (Py_TYPE(item) == &_SetNode_Type)
            result->size += ((SetNode*)item)->size;
        else
            result->size++;
    }
    return result;

error:
    Py_DECREF(new_array);
    return NULL;
}

static SetNode* _union_array_array(SetArrayNode *a, SetArrayNode *b, int shift) {
    uint32_t max_count = (a->count > b->count) ? a->count : b->count;
    SetNode **new_children = PyMem_Calloc(max_count, sizeof(SetNode*));
    if (!new_children) return NULL;
    for (uint32_t i = 0; i < max_count; i++) {
        SetNode *child_a = (i < a->count) ? a->children[i] : NULL;
        SetNode *child_b = (i < b->count) ? b->children[i] : NULL;
        SetNode *child = NULL;
        if (child_a && child_b) {
            child = _set_node_union(child_a, child_b, shift + HAMT_SHIFT);
        } else if (child_a) {
            child = _set_node_copy(child_a);
        } else if (child_b) {
            child = _set_node_copy(child_b);
        }
        new_children[i] = child;
    }
    SetNode *result = _set_node_new_array(max_count, new_children);
    if (!result) {
        for (uint32_t i=0; i<max_count; i++) Py_XDECREF(new_children[i]);
        PyMem_Free(new_children);
        return NULL;
    }
    /* compute size */
    result->size = 0;
    for (uint32_t i = 0; i < max_count; i++) {
        if (new_children[i])
            result->size += new_children[i]->size;
    }
    return result;
}

static SetNode* _union_collision_collision(SetCollisionNode *a, SetCollisionNode *b, int shift) {
    /* Merge key lists, remove duplicates using a Python set */
    PyObject *set = PySet_New(NULL);
    if (!set) return NULL;
    for (uint32_t i = 0; i < a->count; i++) {
        if (PySet_Add(set, a->keys[i]) < 0) { Py_DECREF(set); return NULL; }
    }
    for (uint32_t i = 0; i < b->count; i++) {
        if (PySet_Add(set, b->keys[i]) < 0) { Py_DECREF(set); return NULL; }
    }
    Py_ssize_t new_count = PySet_Size(set);
    if (new_count == 0) {
        Py_DECREF(set);
        return NULL;
    }
    if (new_count == 1) {
        PyObject *key = PySet_Pop(set); /* steals reference */
        PyObject *leaf = Py_BuildValue("(iO)", PyObject_Hash(key), key);
        Py_DECREF(key);
        Py_DECREF(set);
        if (!leaf) return NULL;
        return (SetNode*)leaf;
    }
    /* Build collision node */
    PyObject **keys = PyMem_Malloc(new_count * sizeof(PyObject*));
    if (!keys) { Py_DECREF(set); return NULL; }
    Py_ssize_t idx = 0;
    PyObject *key;
    Py_hash_t hash = 0;
    PyObject *iter = PyObject_GetIter(set);
    if (!iter) { PyMem_Free(keys); Py_DECREF(set); return NULL; }
    while ((key = PyIter_Next(iter))) {
        keys[idx++] = key;
        if (idx == 1) hash = PyObject_Hash(key);
        Py_INCREF(key);
    }
    Py_DECREF(iter);
    Py_DECREF(set);
    SetNode *result = _set_node_new_collision((uint32_t)hash, keys, (uint32_t)new_count);
    if (!result) {
        for (Py_ssize_t i=0; i<new_count; i++) Py_DECREF(keys[i]);
        PyMem_Free(keys);
        return NULL;
    }
    return result;
}

/* Main union function */
SetNode* _set_node_union(SetNode *a, SetNode *b, int shift) {
    if (!a) return _set_node_copy(b);
    if (!b) return _set_node_copy(a);

    if (a->type == SET_NODE_BITMAP && b->type == SET_NODE_BITMAP)
        return _union_bitmap_bitmap((SetBitmapNode*)a, (SetBitmapNode*)b, shift);
    if (a->type == SET_NODE_ARRAY && b->type == SET_NODE_ARRAY)
        return _union_array_array((SetArrayNode*)a, (SetArrayNode*)b, shift);
    if (a->type == SET_NODE_COLLISION && b->type == SET_NODE_COLLISION)
        return _union_collision_collision((SetCollisionNode*)a, (SetCollisionNode*)b, shift);

    /* Mixed types: fallback to collecting all keys and rebuilding */
    PyObject *list = PyList_New(0);
    if (!list) return NULL;
    if (_set_node_collect_keys(a, list, shift) < 0) {
        Py_DECREF(list);
        return NULL;
    }
    if (_set_node_collect_keys(b, list, shift) < 0) {
        Py_DECREF(list);
        return NULL;
    }
    SetNode *result = NULL;
    Py_ssize_t n = PyList_Size(list);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *key = PyList_GetItem(list, i); /* borrowed */
        int added = 0;
        uint32_t hash = PyObject_Hash(key);
        if (hash == (uint32_t)-1) { Py_DECREF(list); return NULL; }
        result = _set_node_assoc(result, hash, key, &added, shift);
        if (!result) { Py_DECREF(list); return NULL; }
    }
    Py_DECREF(list);
    return result;
}


/* ---- Intersection helpers (same-type only) ---------------------------- */

static SetNode* _intersection_bitmap_bitmap(SetBitmapNode *a, SetBitmapNode *b, int shift) {
    uint32_t intersect_bitmap = a->bitmap & b->bitmap;
    if (intersect_bitmap == 0) return NULL;  // no common children

    Py_ssize_t new_len = hamt_bitmap_count(intersect_bitmap);
    PyObject *new_array = PyTuple_New(new_len);
    if (!new_array) return NULL;

    uint32_t bit = 1;
    int out_idx = 0;
    for (int i = 0; i < 32; i++, bit <<= 1) {
        if (!(intersect_bitmap & bit)) continue;
        int a_idx = hamt_bitmap_index(a->bitmap, bit);
        int b_idx = hamt_bitmap_index(b->bitmap, bit);
        PyObject *a_child = PyTuple_GET_ITEM(a->array, a_idx);
        PyObject *b_child = PyTuple_GET_ITEM(b->array, b_idx);

        SetNode *child = NULL;
        if (Py_TYPE(a_child) == &_SetNode_Type && Py_TYPE(b_child) == &_SetNode_Type) {
            child = _set_node_intersection((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
        } else if (Py_TYPE(a_child) == &_SetNode_Type) {
            child = _set_node_intersection((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
        } else if (Py_TYPE(b_child) == &_SetNode_Type) {
            child = _set_node_intersection((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
        } else {
            // both leaves: compare keys
            PyObject *a_key = PyTuple_GET_ITEM(a_child, 1);
            PyObject *b_key = PyTuple_GET_ITEM(b_child, 1);
            if (PyObject_RichCompareBool(a_key, b_key, Py_EQ) == 1) {
                Py_INCREF(a_child);
                child = (SetNode*)a_child;
            } else {
                child = NULL; // no match
            }
        }
        if (child) {
            PyTuple_SET_ITEM(new_array, out_idx++, (PyObject*)child);
        } else {
            // This should not happen because bitmap indicates both have child, but intersection may yield empty.
            // However, if a child is a node that becomes empty after intersection, we must omit it.
            // We'll handle by shrinking the array (complex) – for simplicity we'll treat it as an error for now.
            // Actually, we should not encounter this because if child is a node, _set_node_intersection returns NULL only when empty.
            // For leaf, we already only include if keys equal.
            // If we ever get NULL, we need to shrink the array – we'll rebuild the array in a second pass.
            // To keep code manageable, we'll ignore this case; in practice it shouldn't happen for intersection.
            // For correctness, we should handle it. Let's do a simpler approach: we'll create a new tuple without the missing entry.
            // But that complicates. We'll assume intersection of nodes never becomes empty if both exist? Not true: e.g., two collision nodes with no common keys.
            // So we need to handle it. Let's use a two-pass: first compute which children survive, then allocate.
        }
    }

    // If we had to shrink because some children became empty, we need to rebuild.
    // Simpler: use a list and then convert to tuple at the end.
    // We'll rewrite to use a list first.
    // Let's do a more robust version:
    PyObject *list = PyList_New(0);
    if (!list) { Py_DECREF(new_array); return NULL; }
    uint32_t new_bitmap = 0;
    bit = 1;
    for (int i = 0; i < 32; i++, bit <<= 1) {
        if (!(intersect_bitmap & bit)) continue;
        int a_idx = hamt_bitmap_index(a->bitmap, bit);
        int b_idx = hamt_bitmap_index(b->bitmap, bit);
        PyObject *a_child = PyTuple_GET_ITEM(a->array, a_idx);
        PyObject *b_child = PyTuple_GET_ITEM(b->array, b_idx);
        SetNode *child = NULL;
        if (Py_TYPE(a_child) == &_SetNode_Type && Py_TYPE(b_child) == &_SetNode_Type) {
            child = _set_node_intersection((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
        } else if (Py_TYPE(a_child) == &_SetNode_Type) {
            child = _set_node_intersection((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
        } else if (Py_TYPE(b_child) == &_SetNode_Type) {
            child = _set_node_intersection((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
        } else {
            if (PyObject_RichCompareBool(PyTuple_GET_ITEM(a_child, 1),
                                         PyTuple_GET_ITEM(b_child, 1), Py_EQ) == 1) {
                Py_INCREF(a_child);
                child = (SetNode*)a_child;
            }
        }
        if (child) {
            new_bitmap |= bit;
            if (PyList_Append(list, (PyObject*)child) < 0) {
                Py_DECREF(list);
                return NULL;
            }
        }
    }
    Py_ssize_t new_len_list = PyList_Size(list);
    if (new_len_list == 0) {
        Py_DECREF(list);
        return NULL;
    }
    new_array = PyTuple_New(new_len_list);
    if (!new_array) { Py_DECREF(list); return NULL; }
    for (Py_ssize_t i = 0; i < new_len_list; i++) {
        PyObject *item = PyList_GET_ITEM(list, i);
        Py_INCREF(item);
        PyTuple_SET_ITEM(new_array, i, item);
    }
    Py_DECREF(list);
    SetNode *result = _set_node_new_bitmap(new_bitmap, new_array);
    if (!result) { Py_DECREF(new_array); return NULL; }
    result->size = 0;
    for (Py_ssize_t i = 0; i < new_len_list; i++) {
        PyObject *item = PyTuple_GET_ITEM(new_array, i);
        if (Py_TYPE(item) == &_SetNode_Type)
            result->size += ((SetNode*)item)->size;
        else
            result->size++;
    }
    return result;
}

static SetNode* _intersection_array_array(SetArrayNode *a, SetArrayNode *b, int shift) {
    uint32_t min_count = (a->count < b->count) ? a->count : b->count;
    SetNode **new_children = PyMem_Calloc(min_count, sizeof(SetNode*));
    if (!new_children) return NULL;
    Py_ssize_t new_size = 0;
    for (uint32_t i = 0; i < min_count; i++) {
        SetNode *child_a = a->children[i];
        SetNode *child_b = b->children[i];
        if (!child_a || !child_b) continue;
        SetNode *child = _set_node_intersection(child_a, child_b, shift + HAMT_SHIFT);
        if (child) {
            new_children[i] = child;
            new_size += child->size;
        }
    }
    // Count how many non-NULL children we have
    uint32_t non_null = 0;
    for (uint32_t i = 0; i < min_count; i++) {
        if (new_children[i]) non_null++;
    }
    if (non_null == 0) {
        for (uint32_t i = 0; i < min_count; i++) Py_XDECREF(new_children[i]);
        PyMem_Free(new_children);
        return NULL;
    }
    // Create a compact array (shrink to exact size)
    SetNode **compact = PyMem_Malloc(non_null * sizeof(SetNode*));
    if (!compact) {
        for (uint32_t i = 0; i < min_count; i++) Py_XDECREF(new_children[i]);
        PyMem_Free(new_children);
        return NULL;
    }
    uint32_t j = 0;
    for (uint32_t i = 0; i < min_count; i++) {
        if (new_children[i]) compact[j++] = new_children[i];
    }
    PyMem_Free(new_children);
    SetNode *result = _set_node_new_array(non_null, compact);
    if (!result) {
        for (uint32_t i = 0; i < non_null; i++) Py_DECREF(compact[i]);
        PyMem_Free(compact);
        return NULL;
    }
    result->size = new_size;
    return result;
}

static SetNode* _intersection_collision_collision(SetCollisionNode *a, SetCollisionNode *b, int shift) {
    if (a->base.hash != b->base.hash) return NULL; // no common hash -> empty
    // Build a set of keys from the smaller set for fast lookup
    PyObject *set = PySet_New(NULL);
    if (!set) return NULL;
    SetCollisionNode *small = (a->count <= b->count) ? a : b;
    SetCollisionNode *large = (a->count <= b->count) ? b : a;
    for (uint32_t i = 0; i < small->count; i++) {
        if (PySet_Add(set, small->keys[i]) < 0) {
            Py_DECREF(set);
            return NULL;
        }
    }
    // Collect keys that are in both
    PyObject **common = PyMem_Malloc(large->count * sizeof(PyObject*));
    if (!common) { Py_DECREF(set); return NULL; }
    uint32_t count = 0;
    for (uint32_t i = 0; i < large->count; i++) {
        if (PySet_Contains(set, large->keys[i]) == 1) {
            common[count++] = large->keys[i];
            Py_INCREF(large->keys[i]);
        }
    }
    Py_DECREF(set);
    if (count == 0) {
        PyMem_Free(common);
        return NULL;
    }
    if (count == 1) {
        PyObject *leaf = Py_BuildValue("(iO)", large->base.hash, common[0]);
        Py_DECREF(common[0]);
        PyMem_Free(common);
        if (!leaf) return NULL;
        return (SetNode*)leaf;
    }
    SetNode *result = _set_node_new_collision(large->base.hash, common, count);
    if (!result) {
        for (uint32_t i = 0; i < count; i++) Py_DECREF(common[i]);
        PyMem_Free(common);
        return NULL;
    }
    result->size = count;
    return result;
}

/* ---- Intersection and difference (stubs) ------------------------------- */
/* For now, we can implement them using the same fallback method.
   A full HAMT‑aware implementation would follow the pattern of union. */


SetNode* _set_node_intersection(SetNode *a, SetNode *b, int shift) {
    if (!a || !b) return NULL; // empty

    if (a->type == SET_NODE_BITMAP && b->type == SET_NODE_BITMAP)
        return _intersection_bitmap_bitmap((SetBitmapNode*)a, (SetBitmapNode*)b, shift);
    if (a->type == SET_NODE_ARRAY && b->type == SET_NODE_ARRAY)
        return _intersection_array_array((SetArrayNode*)a, (SetArrayNode*)b, shift);
    if (a->type == SET_NODE_COLLISION && b->type == SET_NODE_COLLISION)
        return _intersection_collision_collision((SetCollisionNode*)a, (SetCollisionNode*)b, shift);

    /* Mixed types: fallback to collecting keys from one set and checking membership in the other */
    // Choose the smaller set to iterate (by size)
    SetNode *iter_set = (a->size <= b->size) ? a : b;
    SetNode *check_set = (a->size <= b->size) ? b : a;
    PyObject *list = PyList_New(0);
    if (!list) return NULL;
    if (_set_node_collect_keys(iter_set, list, shift) < 0) {
        Py_DECREF(list);
        return NULL;
    }
    SetNode *result = NULL;
    Py_ssize_t n = PyList_Size(list);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *key = PyList_GetItem(list, i); // borrowed
        if (_set_node_find(check_set, PyObject_Hash(key), key, shift)) {
            int added = 0;
            uint32_t hash = PyObject_Hash(key);
            if (hash == (uint32_t)-1) { Py_DECREF(list); return NULL; }
            result = _set_node_assoc(result, hash, key, &added, shift);
            if (!result) { Py_DECREF(list); return NULL; }
        }
    }
    Py_DECREF(list);
    return result;
}


/* ---- Difference helpers (same-type only) ----------------------------- */

static SetNode* _difference_bitmap_bitmap(SetBitmapNode *a, SetBitmapNode *b, int shift) {
    uint32_t diff_bitmap = a->bitmap & ~b->bitmap;
    if (diff_bitmap == 0) return NULL; // no keys left

    Py_ssize_t new_len = hamt_bitmap_count(diff_bitmap);
    PyObject *new_array = PyTuple_New(new_len);
    if (!new_array) return NULL;

    uint32_t bit = 1;
    int out_idx = 0;
    for (int i = 0; i < 32; i++, bit <<= 1) {
        if (!(diff_bitmap & bit)) continue;
        int a_idx = hamt_bitmap_index(a->bitmap, bit);
        PyObject *a_child = PyTuple_GET_ITEM(a->array, a_idx);
        if (b->bitmap & bit) {
            int b_idx = hamt_bitmap_index(b->bitmap, bit);
            PyObject *b_child = PyTuple_GET_ITEM(b->array, b_idx);
            SetNode *child = NULL;
            if (Py_TYPE(a_child) == &_SetNode_Type && Py_TYPE(b_child) == &_SetNode_Type) {
                child = _set_node_difference((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
            } else if (Py_TYPE(a_child) == &_SetNode_Type) {
                child = _set_node_difference((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
            } else if (Py_TYPE(b_child) == &_SetNode_Type) {
                // a is leaf, b is node: leaf is only in a if not in b? Actually leaf not in b's subtree, so keep it.
                Py_INCREF(a_child);
                child = (SetNode*)a_child;
            } else {
                // both leaves: only keep if keys differ
                if (PyObject_RichCompareBool(PyTuple_GET_ITEM(a_child, 1),
                                             PyTuple_GET_ITEM(b_child, 1), Py_EQ) != 1) {
                    Py_INCREF(a_child);
                    child = (SetNode*)a_child;
                }
            }
            if (child) {
                PyTuple_SET_ITEM(new_array, out_idx++, (PyObject*)child);
            } else {
                // child became empty, we must remove it – we'll handle with list as before
                // For simplicity, we'll use the list approach again.
            }
        } else {
            // child only in a, keep it as is
            Py_INCREF(a_child);
            PyTuple_SET_ITEM(new_array, out_idx++, a_child);
        }
    }
    // Again, we need to handle case where some children became empty.
    // Let's use the list method for reliability.
    Py_DECREF(new_array);
    // Build list of surviving children
    PyObject *list = PyList_New(0);
    if (!list) return NULL;
    uint32_t new_bitmap = 0;
    bit = 1;
    for (int i = 0; i < 32; i++, bit <<= 1) {
        if (!(diff_bitmap & bit)) continue;
        int a_idx = hamt_bitmap_index(a->bitmap, bit);
        PyObject *a_child = PyTuple_GET_ITEM(a->array, a_idx);
        SetNode *child = NULL;
        if (b->bitmap & bit) {
            int b_idx = hamt_bitmap_index(b->bitmap, bit);
            PyObject *b_child = PyTuple_GET_ITEM(b->array, b_idx);
            if (Py_TYPE(a_child) == &_SetNode_Type && Py_TYPE(b_child) == &_SetNode_Type) {
                child = _set_node_difference((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
            } else if (Py_TYPE(a_child) == &_SetNode_Type) {
                child = _set_node_difference((SetNode*)a_child, (SetNode*)b_child, shift + HAMT_SHIFT);
            } else if (Py_TYPE(b_child) == &_SetNode_Type) {
                Py_INCREF(a_child);
                child = (SetNode*)a_child;
            } else {
                if (PyObject_RichCompareBool(PyTuple_GET_ITEM(a_child, 1),
                                             PyTuple_GET_ITEM(b_child, 1), Py_EQ) != 1) {
                    Py_INCREF(a_child);
                    child = (SetNode*)a_child;
                }
            }
        } else {
            Py_INCREF(a_child);
            child = (SetNode*)a_child;
        }
        if (child) {
            new_bitmap |= bit;
            if (PyList_Append(list, (PyObject*)child) < 0) {
                Py_DECREF(list);
                return NULL;
            }
        }
    }
    Py_ssize_t new_len_list = PyList_Size(list);
    if (new_len_list == 0) {
        Py_DECREF(list);
        return NULL;
    }
    new_array = PyTuple_New(new_len_list);
    if (!new_array) { Py_DECREF(list); return NULL; }
    for (Py_ssize_t i = 0; i < new_len_list; i++) {
        PyObject *item = PyList_GET_ITEM(list, i);
        Py_INCREF(item);
        PyTuple_SET_ITEM(new_array, i, item);
    }
    Py_DECREF(list);
    SetNode *result = _set_node_new_bitmap(new_bitmap, new_array);
    if (!result) { Py_DECREF(new_array); return NULL; }
    result->size = 0;
    for (Py_ssize_t i = 0; i < new_len_list; i++) {
        PyObject *item = PyTuple_GET_ITEM(new_array, i);
        if (Py_TYPE(item) == &_SetNode_Type)
            result->size += ((SetNode*)item)->size;
        else
            result->size++;
    }
    return result;
}

static SetNode* _difference_array_array(SetArrayNode *a, SetArrayNode *b, int shift) {
    // For each index, result is a - b (if b has a child, compute difference; else copy a's child)
    uint32_t max_count = a->count;  // only need up to a's count, b may be shorter
    SetNode **new_children = PyMem_Calloc(max_count, sizeof(SetNode*));
    if (!new_children) return NULL;
    Py_ssize_t new_size = 0;
    for (uint32_t i = 0; i < max_count; i++) {
        SetNode *child_a = a->children[i];
        if (!child_a) continue;
        SetNode *child_b = (i < b->count) ? b->children[i] : NULL;
        SetNode *child = NULL;
        if (child_b) {
            child = _set_node_difference(child_a, child_b, shift + HAMT_SHIFT);
        } else {
            child = _set_node_copy(child_a);
        }
        if (child) {
            new_children[i] = child;
            new_size += child->size;
        }
    }
    // Compact: remove NULLs
    uint32_t non_null = 0;
    for (uint32_t i = 0; i < max_count; i++) {
        if (new_children[i]) non_null++;
    }
    if (non_null == 0) {
        for (uint32_t i = 0; i < max_count; i++) Py_XDECREF(new_children[i]);
        PyMem_Free(new_children);
        return NULL;
    }
    SetNode **compact = PyMem_Malloc(non_null * sizeof(SetNode*));
    if (!compact) {
        for (uint32_t i = 0; i < max_count; i++) Py_XDECREF(new_children[i]);
        PyMem_Free(new_children);
        return NULL;
    }
    uint32_t j = 0;
    for (uint32_t i = 0; i < max_count; i++) {
        if (new_children[i]) compact[j++] = new_children[i];
    }
    PyMem_Free(new_children);
    SetNode *result = _set_node_new_array(non_null, compact);
    if (!result) {
        for (uint32_t i = 0; i < non_null; i++) Py_DECREF(compact[i]);
        PyMem_Free(compact);
        return NULL;
    }
    result->size = new_size;
    return result;
}

static SetNode* _difference_collision_collision(SetCollisionNode *a, SetCollisionNode *b, int shift) {
    if (a->base.hash != b->base.hash) {
        // different hash – all keys in a remain
        return _set_node_copy((SetNode*)a);
    }
    // Build a set of keys in b for fast removal
    PyObject *set = PySet_New(NULL);
    if (!set) return NULL;
    for (uint32_t i = 0; i < b->count; i++) {
        if (PySet_Add(set, b->keys[i]) < 0) {
            Py_DECREF(set);
            return NULL;
        }
    }
    // Collect keys from a that are not in b
    PyObject **result_keys = PyMem_Malloc(a->count * sizeof(PyObject*));
    if (!result_keys) { Py_DECREF(set); return NULL; }
    uint32_t count = 0;
    for (uint32_t i = 0; i < a->count; i++) {
        if (PySet_Contains(set, a->keys[i]) != 1) {
            result_keys[count++] = a->keys[i];
            Py_INCREF(a->keys[i]);
        }
    }
    Py_DECREF(set);
    if (count == 0) {
        PyMem_Free(result_keys);
        return NULL;
    }
    if (count == 1) {
        PyObject *leaf = Py_BuildValue("(iO)", a->base.hash, result_keys[0]);
        Py_DECREF(result_keys[0]);
        PyMem_Free(result_keys);
        if (!leaf) return NULL;
        return (SetNode*)leaf;
    }
    SetNode *result = _set_node_new_collision(a->base.hash, result_keys, count);
    if (!result) {
        for (uint32_t i = 0; i < count; i++) Py_DECREF(result_keys[i]);
        PyMem_Free(result_keys);
        return NULL;
    }
    result->size = count;
    return result;
}


SetNode* _set_node_difference(SetNode *a, SetNode *b, int shift) {
    if (!a) return NULL;
    if (!b) return _set_node_copy(a);

    if (a->type == SET_NODE_BITMAP && b->type == SET_NODE_BITMAP)
        return _difference_bitmap_bitmap((SetBitmapNode*)a, (SetBitmapNode*)b, shift);
    if (a->type == SET_NODE_ARRAY && b->type == SET_NODE_ARRAY)
        return _difference_array_array((SetArrayNode*)a, (SetArrayNode*)b, shift);
    if (a->type == SET_NODE_COLLISION && b->type == SET_NODE_COLLISION)
        return _difference_collision_collision((SetCollisionNode*)a, (SetCollisionNode*)b, shift);

    /* Mixed types: fallback to collecting keys from a and removing those in b */
    PyObject *list = PyList_New(0);
    if (!list) return NULL;
    if (_set_node_collect_keys(a, list, shift) < 0) {
        Py_DECREF(list);
        return NULL;
    }
    SetNode *result = NULL;
    Py_ssize_t n = PyList_Size(list);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *key = PyList_GetItem(list, i); // borrowed
        if (!_set_node_find(b, PyObject_Hash(key), key, shift)) {
            int added = 0;
            uint32_t hash = PyObject_Hash(key);
            if (hash == (uint32_t)-1) { Py_DECREF(list); return NULL; }
            result = _set_node_assoc(result, hash, key, &added, shift);
            if (!result) { Py_DECREF(list); return NULL; }
        }
    }
    Py_DECREF(list);
    return result;
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
