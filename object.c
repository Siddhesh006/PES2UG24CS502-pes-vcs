// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── Implementation ──────────────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // Step 1: Determine type string
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // Step 2: Build header "type size\0" and full object buffer
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    // header_len does NOT include the null terminator snprintf writes,
    // but the null byte itself IS part of the object format.
    size_t full_len = (size_t)header_len + 1 + len; // +1 for the '\0' separator

    uint8_t *full_obj = malloc(full_len);
    if (!full_obj) return -1;

    memcpy(full_obj, header, (size_t)header_len);
    full_obj[header_len] = '\0';
    memcpy(full_obj + header_len + 1, data, len);

    // Step 3: Compute SHA-256 of the full object
    ObjectID id;
    compute_hash(full_obj, full_len, &id);

    // Step 4: Deduplication — if already stored, just return
    if (object_exists(&id)) {
        *id_out = id;
        free(full_obj);
        return 0;
    }

    // Step 5: Build shard directory path and create it
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);

    char shard_dir[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755); // OK if already exists

    // Step 6: Build final path and temp path
    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);

    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%.2s/%s.tmp", OBJECTS_DIR, hex, hex + 2);

    // Step 7: Write to temp file
    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full_obj);
        return -1;
    }

    ssize_t written = write(fd, full_obj, full_len);
    free(full_obj);

    if (written < 0 || (size_t)written != full_len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    // Step 8: fsync the file to flush to disk
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);

    // Step 9: Atomic rename temp -> final
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    // Step 10: fsync the shard directory to persist the rename
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    *id_out = id;
    return 0;
}

// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // Step 1: Get file path
    char path[512];
    object_path(id, path, sizeof(path));

    // Step 2: Open and read entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return -1;
    }

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    size_t nread = fread(buf, 1, (size_t)file_size, f);
    fclose(f);

    if (nread != (size_t)file_size) {
        free(buf);
        return -1;
    }

    // Step 3: Integrity check — recompute hash and compare
    ObjectID computed;
    compute_hash(buf, nread, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;
    }

    // Step 4: Parse header — find the '\0' separating header from data
    uint8_t *null_byte = memchr(buf, '\0', nread);
    if (!null_byte) {
        free(buf);
        return -1;
    }

    // Header is: "type size"
    char type_str[16] = {0};
    size_t declared_size = 0;
    if (sscanf((char *)buf, "%15s %zu", type_str, &declared_size) != 2) {
        free(buf);
        return -1;
    }

    // Step 5: Map type string to ObjectType
    if (strcmp(type_str, "blob") == 0)        *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0)   *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(buf);
        return -1;
    }

    // Step 6: Extract data portion (after the '\0')
    uint8_t *data_start = null_byte + 1;
    size_t data_len = (size_t)(buf + nread - data_start);

    if (data_len != declared_size) {
        free(buf);
        return -1;
    }

    void *out = malloc(data_len + 1); // +1 for safety null terminator
    if (!out) {
        free(buf);
        return -1;
    }
    memcpy(out, data_start, data_len);
    ((uint8_t *)out)[data_len] = '\0';

    free(buf);

    *data_out = out;
    *len_out = data_len;
    return 0;
}
