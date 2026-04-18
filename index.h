#ifndef INDEX_H
#define INDEX_H

#include "pes.h"

#define MAX_INDEX_ENTRIES 10000

typedef struct {
    uint32_t mode;
    ObjectID hash;
    uint64_t mtime_sec;
    uint32_t size;
    char path[512];
} IndexEntry;

typedef struct {
    IndexEntry entries[MAX_INDEX_ENTRIES];
    int count;
} Index;

int index_load(Index *index);

int index_save(const Index *index);

int index_add(Index *index, const char *path);

int index_remove(Index *index, const char *path);

IndexEntry* index_find(Index *index, const char *path);

int index_status(const Index *index);

#endif
