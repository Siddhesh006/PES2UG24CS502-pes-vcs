#ifndef COMMIT_H
#define COMMIT_H

#include "pes.h"

typedef struct {
    ObjectID tree;
    ObjectID parent;
    int has_parent;
    char author[256];
    uint64_t timestamp;
    char message[4096];
} Commit;

int commit_create(const char *message, ObjectID *commit_id_out);

int commit_parse(const void *data, size_t len, Commit *commit_out);

int commit_serialize(const Commit *commit, void **data_out, size_t *len_out);

typedef void (*commit_walk_fn)(const ObjectID *id, const Commit *commit, void *ctx);
int commit_walk(commit_walk_fn callback, void *ctx);


int head_read(ObjectID *id_out);

int head_update(const ObjectID *new_commit);

#endif
