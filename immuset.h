#ifndef IMMUSET_H
#define IMMUSET_H

#include <Python.h>
#include <stdint.h>

/* Node types */
typedef enum {
    SET_NODE_EMPTY = 0,
    SET_NODE_BITMAP,
    SET_NODE_ARRAY,
    SET_NODE_COLLISION
} SetNodeType;

/* Forward declarations of structs */
typedef struct _SetNode SetNode;
typedef struct _SetBitmapNode SetBitmapNode;
typedef struct _SetArrayNode SetArrayNode;
typedef struct _SetCollisionNode SetCollisionNode;
typedef struct _ImmuSet ImmuSet;
typedef struct _ImmuSetMutation ImmuSetMutation;

/* Full structure definitions */
struct _SetNode {
    PyObject_HEAD
    SetNodeType type;
    uint32_t hash;
    Py_ssize_t size;
};

struct _SetBitmapNode {
    SetNode base;
    uint32_t bitmap;
    PyObject *array;
};

struct _SetArrayNode {
    SetNode base;
    uint32_t count;
    SetNode **children;
};

struct _SetCollisionNode {
    SetNode base;
    uint32_t count;
    PyObject **keys;
};

/* Immutable set object */
struct _ImmuSet {
    PyObject_HEAD
    SetNode *root;
    Py_ssize_t len;
    Py_hash_t hash;
};

/* Mutable context */
struct _ImmuSetMutation {
    PyObject_HEAD
    ImmuSet *original;
    SetNode *root;
    Py_ssize_t len;
    int in_context;
};

/* --- Exported type objects (defined in hamt_set.c) --- */
extern PyTypeObject _SetNode_Type;
extern PyTypeObject _SetBitmapNode_Type;
extern PyTypeObject _SetArrayNode_Type;
extern PyTypeObject _SetCollisionNode_Type;

/* --- Core HAMT functions (from hamt_set.c) --- */
SetNode* _set_node_assoc(SetNode *node, uint32_t hash, PyObject *key,
                         int *added, int shift);
SetNode* _set_node_without(SetNode *node, uint32_t hash, PyObject *key,
                           int *removed, int shift);
/* Set operations */
SetNode* _set_node_union(SetNode *a, SetNode *b, int shift);
SetNode* _set_node_intersection(SetNode *a, SetNode *b, int shift);
SetNode* _set_node_difference(SetNode *a, SetNode *b, int shift);

int _set_node_find(SetNode *node, uint32_t hash, PyObject *key, int shift);
void _set_node_dealloc(SetNode *node);
int _hamt_set_init_types(void);
int _set_node_collect_keys(SetNode *node, PyObject *list, int shift);

/* --- ImmuSet and ImmuSetMutation type objects (defined in immuset.c) --- */
extern PyTypeObject ImmuSet_Type;
extern PyTypeObject ImmuSetMutation_Type;

#endif /* IMMUSET_H */
