// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// index_load is in index.c which is not linked for test_tree.
// Declare weak so the binary links; tree_from_index won't be called by test_tree.
__attribute__((weak)) int index_load(Index *index) { (void)index; return -1; }

// Forward declaration (implemented in object.c)
extern int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── Implementation ──────────────────────────────────────────────────────────

// Recursive helper: build a tree from a slice of index entries that share a
// common prefix (the subtree at `prefix_depth` path components deep).
//
// entries      : pointer into the index entries array
// count        : number of entries in this slice (all share the current prefix)
// prefix_len   : byte length of the prefix already consumed (up to and
//                including the '/' separator that introduced this level)
// id_out       : receives the ObjectID of the written tree object
//
// Returns 0 on success, -1 on error.
static int write_tree_level(const IndexEntry *entries, int count,
                            size_t prefix_len, ObjectID *id_out)
{
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        const char *rel = entries[i].path + prefix_len; // path relative to this level

        // Find first '/' in the relative path
        const char *slash = strchr(rel, '/');

        if (!slash) {
            // ── Leaf file: add directly as a blob entry ──────────────────────
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            i++;
        } else {
            // ── Subdirectory: group all entries sharing this component ────────
            size_t dir_name_len = (size_t)(slash - rel);
            char dir_name[256];
            if (dir_name_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, rel, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Collect all entries that start with this same directory component
            int j = i + 1;
            while (j < count) {
                const char *rel_j = entries[j].path + prefix_len;
                // Same dir prefix if first '/' is at same position and names match
                const char *slash_j = strchr(rel_j, '/');
                if (!slash_j) break; // file — different level
                size_t dlen_j = (size_t)(slash_j - rel_j);
                if (dlen_j != dir_name_len || strncmp(rel_j, dir_name, dir_name_len) != 0)
                    break;
                j++;
            }

            // Recurse: build subtree for entries[i..j-1]
            size_t new_prefix = prefix_len + dir_name_len + 1; // +1 for '/'
            ObjectID subtree_id;
            if (write_tree_level(entries + i, j - i, new_prefix, &subtree_id) != 0)
                return -1;

            // Add dir entry to current tree
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = subtree_id;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';

            i = j;
        }
    }

    // Serialize and write this tree level to the object store
    void *raw;
    size_t raw_len;
    if (tree_serialize(&tree, &raw, &raw_len) != 0) return -1;
    int rc = object_write(OBJ_TREE, raw, raw_len, id_out);
    free(raw);
    return rc;
}

// Comparator for qsort — sorts IndexEntry pointers by path
static int compare_index_entries_by_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    // Step 1: Load the index
    Index index;
    if (index_load(&index) != 0) return -1;

    if (index.count == 0) return -1; // Nothing staged

    // Step 2: Sort entries by path to ensure grouping works correctly
    qsort(index.entries, index.count, sizeof(IndexEntry), compare_index_entries_by_path);

    // Step 3: Recursively build the root tree (prefix_len = 0)
    return write_tree_level(index.entries, index.count, 0, id_out);
}