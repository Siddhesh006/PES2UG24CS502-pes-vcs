# Building PES-VCS — A Version Control System from Scratch

**Objective:** Build a local version control system that tracks file changes, stores snapshots efficiently, and supports commit history. Every component maps directly to operating system and filesystem concepts.

**Platform:** Ubuntu 22.04

---

## Project Overview

I built PES-VCS, a minimal but complete version control system that mirrors Git's internal object store and content-addressable filesystem. Like Git, PES-VCS stores every piece of tracked data — file contents, directory snapshots, and commit records — as immutable objects identified solely by their SHA-256 hash. Because an object's identity is its content hash, identical files are automatically deduplicated: store the same file twice and only one copy lives on disk.

The repository state machine closely follows Git:

- `pes init` creates a `.pes/` directory structure (objects store, refs, HEAD).
- `pes add <file>` hashes the file content, stores it as a **blob** object, and records the mapping in a text-based **index** (staging area).
- `pes commit -m <msg>` converts the index into a **tree** hierarchy of objects (one per directory level), wraps that tree hash with author metadata into a **commit** object, and advances the `main` branch pointer atomically.
- `pes log` walks the parent-pointer chain of commits backward from HEAD, printing each one.
- `pes status` compares the index against the working directory using `mtime` / `size` fast-diff.

Every write that matters is performed atomically: I write to a `.tmp` sibling file, call `fsync(2)` to flush hardware buffers, then `rename(2)` over the final path. On POSIX this rename is guaranteed to be atomic, so a power failure can never leave a half-written object or a corrupt index.

---

## Build & Execution Instructions

### Prerequisites

```bash
sudo apt update && sudo apt install -y gcc build-essential libssl-dev
```

### Building

```bash
make          # Build the pes binary
make all      # Build pes + test binaries (test_objects, test_tree)
make clean    # Remove all build artifacts and .pes/
```

### Running

```bash
./pes init
echo "Hello, world" > hello.txt
./pes add hello.txt
./pes commit -m "Initial commit"
./pes log
./pes status
```

---

## Implementation

### Phase 1: Object Storage Foundation (`object.c`)

I implemented `object_write` and `object_read` — the primitives that everything else builds on.

**`object_write`** constructs the full object on disk:
1. Prepend a text header (`"blob 16\0"`) to the raw data buffer.
2. Run SHA-256 over the combined header + data using OpenSSL's EVP API.
3. Skip writing if the object already exists (deduplication via `access()`).
4. Create the two-character shard directory (e.g., `.pes/objects/d5/`) with `mkdir(2)`.
5. Open a `.tmp` file with `O_CREAT | O_WRONLY | O_TRUNC`, write all bytes, call `fsync(2)`, then `rename(2)` over the final path. Also `fsync` the shard directory fd to persist the directory entry.

**`object_read`** reverses the process:
1. Reconstruct the path from the hash hex characters.
2. `fread` the entire file into a heap buffer.
3. Recompute the SHA-256 and compare byte-for-byte against the requested hash — return `-1` on mismatch (integrity check).
4. Parse the `"type size\0"` header with `sscanf` + `memchr`.
5. `malloc` and return the data slice (everything after the `\0`).

**Challenge overcome:** The atomic write pattern — writing directly to the target would leave a half-written file visible to concurrent readers. Using `rename()` on the same filesystem is guaranteed atomic on Linux/POSIX.

### Phase 2: Tree Objects (`tree.c`)

I implemented `tree_from_index`, which must recursively decompose a flat list of sorted paths into a tree hierarchy.

The key insight: after sorting index entries by path, entries sharing the same top-level directory component are grouped together. I wrote a recursive helper `write_tree_level(entries, count, prefix_len, id_out)` that:
1. Scans entries left-to-right.
2. If a path has no `/` after `prefix_len`, it's a leaf file → add it directly as a blob `TreeEntry`.
3. If it has a `/`, collect all consecutive entries sharing that same directory component and recurse with `prefix_len += dir_name_len + 1`.
4. After recursion, add the subtree's `ObjectID` as a directory entry (`mode = 0040000`).
5. Serialize the completed `Tree` struct with `tree_serialize` and write it with `object_write(OBJ_TREE, ...)`.

**Challenge overcome:** Handling the link-time dependency — `test_tree` doesn't link `index.o`, so I declared `index_load` as `__attribute__((weak))` in `tree.c` to prevent a linker error without modifying the provided Makefile.

### Phase 3: The Index / Staging Area (`index.c`)

I implemented three functions:

**`index_load`:** Opens `.pes/index` with `fopen("r")`. Missing file is not an error — it means an empty index. Each line is parsed with `sscanf` using the format `"%o %64s %llu %u %511s"` to extract mode, hex hash, mtime, size, and path. I convert the hex string to binary with the provided `hex_to_hash`.

**`index_save`:** Creates a heap-allocated copy of the index, sorts it by path with `qsort`, then writes all entries to `.pes/index.tmp` using `fprintf`. Calls `fflush` (flush libc buffers) then `fsync(fileno(f))` (flush kernel page cache to disk), then atomically `rename(".pes/index.tmp", ".pes/index")`.

**`index_add`:** Reads the target file with `fread`, calls `lstat` for metadata (mtime, size, mode), calls `object_write(OBJ_BLOB, ...)` to store the content, then either updates an existing `IndexEntry` (via `index_find`) or appends a new one. Finishes with `index_save`.

**Challenge overcome:** The `Index` struct is ~5 MB on disk (10,000 entries × ~512 bytes). Stack-allocating a copy inside `index_save` triggered a stack overflow (`SIGSEGV` at `index.c:199`). I fixed this by heap-allocating the sorted copy with `malloc(sizeof(Index))` and freeing it before returning.

### Phase 4: Commits and History (`commit.c`)

I implemented `commit_create`, which orchestrates all prior phases:

1. Call `tree_from_index(&c.tree)` to recursively build and persist the root tree, getting back its hash.
2. Attempt `head_read(&c.parent)` — success means a parent commit exists; failure (first commit) sets `c.has_parent = 0`.
3. Copy the author string from `pes_author()` (reads `PES_AUTHOR` env var) and capture `time(NULL)` as the Unix timestamp.
4. Copy the commit message string.
5. Call `commit_serialize(&c, &raw, &raw_len)` to produce the multi-line text format.
6. Call `object_write(OBJ_COMMIT, raw, raw_len, commit_id_out)` and `free(raw)`.
7. Call `head_update(commit_id_out)` which atomically writes the new hash to the branch ref file (`.pes/refs/heads/main`).

**Challenge overcome:** Ensuring the parent link is handled correctly for the very first commit — `head_read` returns `-1` when no branch ref file exists yet, which I treat as `has_parent = 0` rather than a fatal error.

---

## Architecture Overview

```
┌───────────────────────────────────────────────────────────────┐
│                      WORKING DIRECTORY                        │
│                  (actual files you edit)                       │
└───────────────────────────────────────────────────────────────┘
                              │
                        pes add <file>
                              ▼
┌───────────────────────────────────────────────────────────────┐
│                           INDEX                               │
│                (staged changes, ready to commit)              │
│                100644 a1b2c3... src/main.c                    │
└───────────────────────────────────────────────────────────────┘
                              │
                       pes commit -m "msg"
                              ▼
┌───────────────────────────────────────────────────────────────┐
│                       OBJECT STORE                            │
│  ┌───────┐    ┌───────┐    ┌────────┐                         │
│  │ BLOB  │◄───│ TREE  │◄───│ COMMIT │                         │
│  │(file) │    │(dir)  │    │(snap)  │                         │
│  └───────┘    └───────┘    └────────┘                         │
│  Stored at: .pes/objects/XX/YYY...                            │
└───────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌───────────────────────────────────────────────────────────────┐
│                           REFS                                │
│       .pes/refs/heads/main  →  commit hash                    │
│       .pes/HEAD             →  "ref: refs/heads/main"         │
└───────────────────────────────────────────────────────────────┘
```

---
