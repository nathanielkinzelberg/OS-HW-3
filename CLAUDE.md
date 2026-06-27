# OS HW 3 — OnlyFiles: A Block-Based Filesystem

Reichman University, Operating Systems — **Exercise 3** (`ex3.pdf`). Submitted via INGInious,
in **pairs**. Graded on Ubuntu 24.04 LTS, **GCC 13**, **GNU/C 17** (`gcc-13 -std=gnu17`),
**x86-64**.

The exercise has two parts:

- **Part A — Theory** (TQ1–TQ3): submitted as a **Jupyter notebook** (`theory.ipynb`). Answers
  must be typed — hand-written scans are rejected.
- **Part B — Programming**: implement the `OnlyFiles` block-based filesystem library (`fs.c`).

## Build & Test

`test.c` includes `"fs.h"`, which lives in the project root, so the include path (`-I.`) is
required:

```bash
# Local (matches the gcc-13 -std=gnu17 grading environment when gcc-13 is installed)
gcc-13 -std=gnu17 -g -Wall -Wextra -I. -o test test.c fs.c
./test
```

Expected output once implemented:

```
Removed existing disk file.
All files written successfully.
All files have been created successfully.
All files read successfully.
Success!
```

> `test.c` is for local testing only. Do **not** submit it, and the submitted library must
> contain **no `main()`**.

## Files

| File | Role |
|---|---|
| `fs.h` | Public API + `superblock` / `inode` structs + constants — **provided, do not modify or submit** |
| `fs.c` | **Your implementation** (the one file you submit, plus any private helper `.c/.h`) |
| `test.c` | Local test — **do not submit** |

## Disk Layout

The virtual disk (10 MB) is divided into fixed-size 4 KB blocks — 2560 blocks total:

| Region | Starting Block | Offset (bytes) | Size | Description |
|---|---|---|---|---|
| Superblock | 0 | 0 | 4 KB (1 block) | Global filesystem metadata |
| Block Bitmap | 1 | 4 KB | 4 KB (1 block) | Tracks free/used blocks |
| Inode Table | 2 | 8 KB | 32 KB (8 blocks) | File metadata (256 inodes) |
| Data Blocks | 10 | 40 KB | 9.96 MB (2550 blocks) | File contents |

## On-Disk Structures (from `fs.h` — do not modify)

### Superblock (block 0)
```c
typedef struct {
    int total_blocks;   // 2560 for 10 MB
    int block_size;     // 4096 bytes
    int free_blocks;    // number of available data blocks
    int total_inodes;   // 256
    int free_inodes;    // number of available inodes
} superblock;
```
- Initialize all fields in `fs_format`.
- Read and validate during `fs_mount`.
- Update `free_blocks` and `free_inodes` whenever allocating or freeing resources.
- Always write the updated superblock back to disk after any change.

### Inode (128 bytes each, 256 total — blocks 2–9)
```c
typedef struct {
    int used;                       // 1 if active, 0 if free
    char name[MAX_FILENAME];        // up to 28 chars + null terminator
    int size;                       // file size in bytes
    int blocks[MAX_DIRECT_BLOCKS];  // 12 direct block pointers
} inode;
```
- Max file size: 12 × 4 KB = **48 KB**.
- `blocks[i]` holds the absolute block number on disk (≥ 10 for data blocks).

### Block Bitmap (block 1)
```c
unsigned char bitmap[MAX_BLOCKS / 8];  // 1 bit per block (320 bytes used of 4 KB)

// Mark block N as used
bitmap[N / 8] |= (1 << (N % 8));

// Mark block N as free
bitmap[N / 8] &= ~(1 << (N % 8));

// Check if block N is in use
if (bitmap[N / 8] & (1 << (N % 8))) { /* in use */ }
```
- Metadata blocks (0–9) must be marked used at format time.
- Data blocks (10–2559) start free.

## API Specification

### Filesystem operations

| Function | Returns |
|---|---|
| `fs_format(const char *disk_path)` | 0 on success; -1 on failure |
| `fs_mount(const char *disk_path)` | 0 on success; -1 on failure |
| `fs_unmount()` | void |

**`fs_format`** — Creates and initializes the virtual disk:
- Opens (or overwrites) the file at `disk_path`.
- Creates a 10 MB file.
- Initializes the superblock, bitmap (metadata blocks 0–9 marked used), and inode table (all
  inodes set `used = 0`).

**`fs_mount`** — Loads an existing filesystem:
- Opens the virtual disk file.
- Reads and validates the superblock.
- Loads necessary metadata (superblock, bitmap, inode table) into memory.

**`fs_unmount`** — Flushes any cached data to disk and closes the file descriptor.

### File operations

| Function | Returns |
|---|---|
| `fs_create(const char *filename)` | 0 / -1 (already exists) / -2 (no free inodes) / -3 (other) |
| `fs_delete(const char *filename)` | 0 / -1 (not found) / -2 (other) |
| `fs_list(char filenames[][MAX_FILENAME], int max_files)` | count (0–max_files) / -1 on error |
| `fs_write(const char *filename, const void *data, int size)` | 0 / -1 (not found) / -2 (no space) / -3 (other) |
| `fs_read(const char *filename, void *buffer, int size)` | bytes read / -1 (not found) / -3 (other) |

**`fs_write`** — Overwrites the entire file content:
- Frees previously allocated blocks.
- Allocates new blocks as needed.
- Writes data to allocated blocks.
- Updates the inode (`size` and `blocks[]`), bitmap, and superblock.

**`fs_read`** — Reads `min(file_size, size)` bytes from the file's blocks into `buffer`.

## Implementation Requirements

### System calls only — no stdio, no dynamic allocation
```c
// Opening the virtual disk
int disk_fd = open(disk_path, O_RDWR | O_CREAT, 0644);

// Reading a block
lseek(disk_fd, block_num * BLOCK_SIZE, SEEK_SET);
read(disk_fd, buffer, BLOCK_SIZE);

// Writing a block
lseek(disk_fd, block_num * BLOCK_SIZE, SEEK_SET);
write(disk_fd, buffer, BLOCK_SIZE);

// Closing the disk
close(disk_fd);
```

**Do NOT use:** `fopen`, `fread`, `fwrite`, `fprintf` (for data), `mmap`, `malloc`, `calloc`,
`free`.

### Block access pattern
For every file operation:
1. Find the target file's inode (scan inode table).
2. Calculate which blocks are involved.
3. Read or write those blocks using `lseek` + `read`/`write`.
4. Update metadata (inode, bitmap, superblock) as needed.
5. Write all changes back to disk.

### Simplifying assumptions
- Only a single-threaded process will use the library — no mutex needed.
- All virtual disk files passed to `fs_mount` are valid filesystems previously created by
  `fs_format`. No need to handle corrupted metadata.

## Recommended Implementation Order
1. `fs_format` — create and initialize the disk file
2. `fs_mount` + `fs_unmount` — open, validate, close
3. `fs_create` + `fs_list` — inode management
4. `fs_write` + `fs_read` — data block I/O
5. `fs_delete` — resource cleanup

## Theory Part (`theory.ipynb`)
| Q | Topic | Key concepts |
|---|---|---|
| TQ1 | MLFQ scheduling variant | Steady-state queue placement, CPU share, masquerade flaw, demotion rule |
| TQ2 | Demand paging + page replacement | LRU vs Clock simulation, fault counts, dirty vs clean eviction |
| TQ3 | File systems: links, partitions, indirect blocks | Hard vs symbolic links, cross-partition mv, indirect block arithmetic |

## Submission Checklist
- [ ] Theory: `theory.ipynb` with typed answers to TQ1–TQ3, names + IDs per the INGInious format.
- [ ] Programming: a ZIP with `fs.c` (+ any private helper `.c/.h`) — **no** `fs.h`, `test.c`,
      binaries, or `main()`.
- [ ] No leftover debug prints.
- [ ] Verified on INGInious (not only locally).
