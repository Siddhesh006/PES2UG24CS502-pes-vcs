#ifndef TREE_H
#define TREE_H

#include "pes.h"

#define MAX_TREE_ENTRIES 1024

typedef struct {
    uint32_t mode;
    ObjectID hash;
    char name[256];
} TreeEntry;

typedef struct {
    TreeEntry entries[MAX_TREE_ENTRIES];
    int count;
} Tree;

int tree_parse(const void *data, size_t len, Tree *tree_out);

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out);

int tree_from_index(ObjectID *id_out);

#endif
