#include "pes.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);
int object_exists(const ObjectID *id);
void object_path(const ObjectID *id, char *path_out, size_t path_size);

void test_blob_storage(void) {
    const char *content = "Hello, PES-VCS!\n";
    ObjectID id;

    int rc = object_write(OBJ_BLOB, content, strlen(content), &id);
    assert(rc == 0);

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);
    printf("Stored blob with hash: %s\n", hex);

    char path[512];
    object_path(&id, path, sizeof(path));
    printf("Object stored at: %s\n", path);

    ObjectType type;
    void *data;
    size_t len;
    rc = object_read(&id, &type, &data, &len);
    assert(rc == 0);
    assert(type == OBJ_BLOB);
    assert(len == strlen(content));
    assert(memcmp(data, content, len) == 0);
    free(data);

    printf("PASS: blob storage\n");
}

void test_deduplication(void) {
    const char *content = "Duplicate content\n";
    ObjectID id1, id2;

    object_write(OBJ_BLOB, content, strlen(content), &id1);
    object_write(OBJ_BLOB, content, strlen(content), &id2);

    assert(memcmp(&id1, &id2, sizeof(ObjectID)) == 0);

    printf("PASS: deduplication\n");
}

void test_integrity(void) {
    const char *content = "Test integrity\n";
    ObjectID id;
    object_write(OBJ_BLOB, content, strlen(content), &id);

    char path[512];
    object_path(&id, path, sizeof(path));

    FILE *f = fopen(path, "r+b");
    assert(f != NULL);
    fseek(f, 20, SEEK_SET);
    fputc('X', f);
    fclose(f);

    ObjectType type;
    void *data;
    size_t len;
    int rc = object_read(&id, &type, &data, &len);
    assert(rc == -1);

    printf("PASS: integrity check\n");
}

int main(void) {
    int rc __attribute__((unused));
    rc = system("rm -rf .pes");
    rc = system("mkdir -p .pes/objects .pes/refs/heads");

    test_blob_storage();
    test_deduplication();
    test_integrity();

    printf("\nAll Phase 1 tests passed.\n");
    return 0;
}
