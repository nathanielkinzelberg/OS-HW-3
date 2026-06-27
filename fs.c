/*
 * ============================================================================
 * fs.c
 * ============================================================================
 *
 * OnlyFiles — Block-Based Filesystem
 *
 * Implements a simplified flat filesystem stored entirely inside a single
 * virtual disk file (10 MB).  The disk is divided into 4 KB blocks:
 *
 *   Block 0        — Superblock
 *   Block 1        — Block Bitmap
 *   Blocks 2–9    — Inode Table (256 inodes × 128 bytes)
 *   Blocks 10–2559 — Data Blocks
 *
 * IMPORTANT:
 * Use only low-level syscalls: open, lseek, read, write, close.
 * No fopen/fread/fwrite, no mmap, no malloc/calloc/free.
 *
 * ============================================================================
 */

#include "fs.h"

/*
 * ============================================================================
 * GLOBAL STATE
 * ============================================================================
 */

/*
 * File descriptor for the open virtual disk.
 * -1 means no disk is currently mounted.
 */
static int disk_fd = -1;

/*
 * In-memory copy of the superblock (block 0).
 * Kept in sync with disk after every mutating operation.
 */
static superblock sb;

/*
 * In-memory copy of the block bitmap (block 1).
 * 1 bit per block: 0 = free, 1 = used.
 */
static unsigned char bitmap[MAX_BLOCKS / 8];

/*
 * In-memory copy of the entire inode table (blocks 2–9).
 * 256 inodes, each 128 bytes.
 */
static inode inode_table[MAX_FILES];


/*
 * ============================================================================
 * LOW-LEVEL BLOCK I/O HELPERS
 * ============================================================================
 */

/*
 * Read one 4 KB block from disk into buf.
 *
 * Returns 0 on success, -1 on failure.
 */
static int read_block(int block_num, void *buf)
{
    if (lseek(disk_fd, (off_t)block_num * BLOCK_SIZE, SEEK_SET) < 0)
        return -1;
    if (read(disk_fd, buf, BLOCK_SIZE) != BLOCK_SIZE)
        return -1;
    return 0;
}


/*
 * Write one 4 KB block from buf to disk.
 *
 * Returns 0 on success, -1 on failure.
 */
static int write_block(int block_num, const void *buf)
{
    if (lseek(disk_fd, (off_t)block_num * BLOCK_SIZE, SEEK_SET) < 0)
        return -1;
    if (write(disk_fd, buf, BLOCK_SIZE) != BLOCK_SIZE)
        return -1;
    return 0;
}


/*
 * ============================================================================
 * METADATA FLUSH HELPERS
 * ============================================================================
 */

/*
 * Write the in-memory superblock back to block 0.
 *
 * superblock is smaller than BLOCK_SIZE, so we zero-pad to a full block
 * to avoid writing garbage bytes to disk.
 */
static int flush_superblock(void)
{
    char buf[BLOCK_SIZE];
    memset(buf, 0, BLOCK_SIZE);
    memcpy(buf, &sb, sizeof(superblock));
    return write_block(0, buf);
}


/*
 * Write the in-memory bitmap back to block 1.
 *
 * bitmap is 320 bytes (2560 bits), zero-padded to a full block.
 */
static int flush_bitmap(void)
{
    char buf[BLOCK_SIZE];
    memset(buf, 0, BLOCK_SIZE);
    memcpy(buf, bitmap, sizeof(bitmap));
    return write_block(1, buf);
}


/*
 * Write the entire in-memory inode table back to disk (blocks 2–9).
 *
 * sizeof(inode) = 88, which does not evenly divide BLOCK_SIZE, so inodes
 * can straddle block boundaries. Writing the table as a raw byte blob one
 * block at a time avoids any cross-block patching complexity.
 *
 * The last partial block is zero-padded before writing.
 */
static int flush_inode_table(void)
{
    char buf[BLOCK_SIZE];
    size_t table_bytes = sizeof(inode_table);   /* 88 * 256 = 22528 */

    for (int i = 0; i < 8; i++)
    {
        memset(buf, 0, BLOCK_SIZE);
        size_t offset = (size_t)i * BLOCK_SIZE;

        if (offset < table_bytes)
        {
            size_t to_copy = BLOCK_SIZE;
            if (offset + BLOCK_SIZE > table_bytes)
                to_copy = table_bytes - offset;
            memcpy(buf, (char *)inode_table + offset, to_copy);
        }

        if (write_block(2 + i, buf) < 0)
            return -1;
    }
    return 0;
}


/*
 * ============================================================================
 * BITMAP HELPERS
 * ============================================================================
 */

/*
 * Find the first free data block (block number >= 10) and mark it used.
 *
 * Updates the in-memory bitmap and superblock free_blocks count.
 * Caller is responsible for flushing bitmap and superblock to disk.
 *
 * Returns the block number on success, -1 if the disk is full.
 */
static int alloc_block(void)
{
    for (int i = 10; i < MAX_BLOCKS; i++)
    {
        if (!(bitmap[i / 8] & (1 << (i % 8))))
        {
            bitmap[i / 8] |= (1 << (i % 8));
            sb.free_blocks--;
            return i;
        }
    }
    return -1;
}


/*
 * Mark a data block as free in the in-memory bitmap.
 *
 * Updates the in-memory bitmap and superblock free_blocks count.
 * Caller is responsible for flushing bitmap and superblock to disk.
 */
static void free_block(int block_num)
{
    bitmap[block_num / 8] &= ~(1 << (block_num % 8));
    sb.free_blocks++;
}


/*
 * ============================================================================
 * INODE HELPERS
 * ============================================================================
 */

/*
 * Scan the inode table for a file with the given name.
 *
 * Returns the inode index (0–255) on success, -1 if not found.
 */
static int find_inode(const char *filename)
{
    for (int i = 0; i < MAX_FILES; i++)
    {
        if (inode_table[i].used && strncmp(inode_table[i].name, filename, MAX_FILENAME) == 0)
            return i;
    }
    return -1;
}


/*
 * Find the first free inode slot (used == 0).
 *
 * Returns the inode index on success, -1 if all inodes are taken.
 */
static int alloc_inode(void)
{
    for (int i = 0; i < MAX_FILES; i++)
    {
        if (!inode_table[i].used)
            return i;
    }
    return -1;
}


/*
 * ============================================================================
 * PUBLIC API IMPLEMENTATION
 * ============================================================================
 */


/*
 * Create and initialize a virtual disk at disk_path.
 *
 * Responsibilities:
 * - Open (or overwrite) the file.
 * - Extend it to 10 MB.
 * - Initialize and write the superblock.
 * - Initialize and write the bitmap (metadata blocks 0–9 marked used).
 * - Initialize and write the inode table (all inodes free).
 */
int fs_format(const char *disk_path)
{
    disk_fd = open(disk_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (disk_fd < 0)
        return -1;

    /* Extend the file to exactly 10 MB by seeking to the last byte and
     * writing one zero. Everything in between becomes a zero-filled hole. */
    if (lseek(disk_fd, (off_t)MAX_BLOCKS * BLOCK_SIZE - 1, SEEK_SET) < 0)
        return -1;
    if (write(disk_fd, "\0", 1) != 1)
        return -1;

    /* Initialize superblock */
    sb.total_blocks = MAX_BLOCKS;
    sb.block_size   = BLOCK_SIZE;
    sb.free_blocks  = MAX_BLOCKS - 10;  /* blocks 0-9 reserved for metadata */
    sb.total_inodes = MAX_FILES;
    sb.free_inodes  = MAX_FILES;

    /* Initialize bitmap: mark metadata blocks 0-9 as used, rest free */
    memset(bitmap, 0, sizeof(bitmap));
    for (int i = 0; i < 10; i++)
        bitmap[i / 8] |= (1 << (i % 8));

    /* Initialize inode table: all slots free */
    memset(inode_table, 0, sizeof(inode_table));

    /* Write everything to disk */
    if (flush_superblock() < 0)  return -1;
    if (flush_bitmap() < 0)      return -1;
    if (flush_inode_table() < 0) return -1;

    /* Format is complete — close so fs_mount can open it fresh */
    close(disk_fd);
    disk_fd = -1;

    return 0;
}


/*
 * Load an existing filesystem from disk_path.
 *
 * Responsibilities:
 * - Open the file.
 * - Read and validate the superblock.
 * - Load the bitmap and inode table into memory.
 */
int fs_mount(const char *disk_path)
{
    disk_fd = open(disk_path, O_RDWR);
    if (disk_fd < 0)
        return -1;

    /* Read superblock from block 0 */
    char buf[BLOCK_SIZE];
    if (read_block(0, buf) < 0)
    {
        close(disk_fd);
        disk_fd = -1;
        return -1;
    }
    memcpy(&sb, buf, sizeof(superblock));

    /* Validate: if these don't match it's not our filesystem */
    if (sb.total_blocks != MAX_BLOCKS || sb.block_size != BLOCK_SIZE)
    {
        close(disk_fd);
        disk_fd = -1;
        return -1;
    }

    /* Load bitmap from block 1 */
    if (read_block(1, buf) < 0)
    {
        close(disk_fd);
        disk_fd = -1;
        return -1;
    }
    memcpy(bitmap, buf, sizeof(bitmap));

    /* Load inode table from blocks 2-9 */
    size_t table_bytes = sizeof(inode_table);
    for (int i = 0; i < 8; i++)
    {
        if (read_block(2 + i, buf) < 0)
        {
            close(disk_fd);
            disk_fd = -1;
            return -1;
        }
        size_t offset = (size_t)i * BLOCK_SIZE;
        if (offset >= table_bytes)
            break;
        size_t to_copy = BLOCK_SIZE;
        if (offset + BLOCK_SIZE > table_bytes)
            to_copy = table_bytes - offset;
        memcpy((char *)inode_table + offset, buf, to_copy);
    }

    return 0;
}


/*
 * Flush all cached metadata to disk and close the file descriptor.
 *
 * Since we flush after every mutating operation, there is nothing extra
 * to sync here — we just close the file descriptor cleanly.
 */
void fs_unmount(void)
{
    if (disk_fd >= 0)
    {
        close(disk_fd);
        disk_fd = -1;
    }
}


/*
 * Create a new empty file with the given filename.
 *
 * Responsibilities:
 * - Check for duplicate filename.
 * - Find a free inode and initialize it.
 * - Update the superblock (free_inodes--).
 * - Flush changes to disk.
 */
int fs_create(const char *filename)
{
    /* Duplicate check */
    if (find_inode(filename) >= 0)
        return -1;

    /* Find a free inode slot */
    int idx = alloc_inode();
    if (idx < 0)
        return -2;

    /* Initialize the inode */
    inode_table[idx].used = 1;
    strncpy(inode_table[idx].name, filename, MAX_FILENAME - 1);
    inode_table[idx].name[MAX_FILENAME - 1] = '\0';
    inode_table[idx].size = 0;
    memset(inode_table[idx].blocks, 0, sizeof(inode_table[idx].blocks));

    /* Update superblock */
    sb.free_inodes--;

    /* Flush to disk */
    if (flush_inode_table() < 0) return -3;
    if (flush_superblock() < 0)  return -3;

    return 0;
}


/*
 * Delete a file and free all its resources.
 *
 * Responsibilities:
 * - Find the file's inode.
 * - Free all data blocks in the bitmap.
 * - Mark the inode as free.
 * - Update the superblock (free_blocks++, free_inodes++).
 * - Flush changes to disk.
 */
int fs_delete(const char *filename)
{
    int idx = find_inode(filename);
    if (idx < 0)
        return -1;

    /* Free all data blocks (updates in-memory bitmap + superblock count) */
    int num_blocks = (inode_table[idx].size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (int i = 0; i < num_blocks; i++)
        free_block(inode_table[idx].blocks[i]);

    /* Clear the inode slot and give it back */
    memset(&inode_table[idx], 0, sizeof(inode));
    sb.free_inodes++;

    /* Flush to disk */
    if (flush_inode_table() < 0) return -2;
    if (flush_bitmap() < 0)      return -2;
    if (flush_superblock() < 0)  return -2;

    return 0;
}


/*
 * List up to max_files filenames from the inode table into filenames[][].
 *
 * Responsibilities:
 * - Scan inode_table for used inodes.
 * - Copy each filename into the provided array.
 * - Return the count of files found.
 */
int fs_list(char filenames[][MAX_FILENAME], int max_files)
{
    int count = 0;

    for (int i = 0; i < MAX_FILES && count < max_files; i++)
    {
        if (inode_table[i].used)
        {
            strncpy(filenames[count], inode_table[i].name, MAX_FILENAME);
            count++;
        }
    }

    return count;
}


/*
 * Overwrite a file's content with the provided data.
 *
 * Responsibilities:
 * - Find the file's inode.
 * - Free previously allocated blocks.
 * - Allocate new blocks as needed.
 * - Write data to the allocated blocks.
 * - Update the inode (size, blocks[]), bitmap, and superblock.
 * - Flush all changes to disk.
 */
int fs_write(const char *filename, const void *data, int size)
{
    if (size < 0 || size > MAX_DIRECT_BLOCKS * BLOCK_SIZE)
        return -2;

    int idx = find_inode(filename);
    if (idx < 0)
        return -1;

    int blocks_needed = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    /* Check there is enough free space before touching anything */
    int old_block_count = (inode_table[idx].size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    int net_new = blocks_needed - old_block_count;
    if (net_new > sb.free_blocks)
        return -2;

    /* Free old blocks (updates in-memory bitmap + superblock count) */
    for (int i = 0; i < old_block_count; i++)
        free_block(inode_table[idx].blocks[i]);
    memset(inode_table[idx].blocks, 0, sizeof(inode_table[idx].blocks));

    /* Allocate new blocks and write data block by block */
    const char *src = (const char *)data;
    int remaining = size;

    for (int i = 0; i < blocks_needed; i++)
    {
        int block_num = alloc_block();
        if (block_num < 0)
            return -2;

        inode_table[idx].blocks[i] = block_num;

        /* Zero-pad a full block buffer, copy in however much data we have */
        char buf[BLOCK_SIZE];
        memset(buf, 0, BLOCK_SIZE);
        int to_write = remaining < BLOCK_SIZE ? remaining : BLOCK_SIZE;
        memcpy(buf, src, to_write);

        if (write_block(block_num, buf) < 0)
            return -3;

        src      += to_write;
        remaining -= to_write;
    }

    /* Update inode size and flush everything to disk */
    inode_table[idx].size = size;

    if (flush_inode_table() < 0) return -3;
    if (flush_bitmap() < 0)      return -3;
    if (flush_superblock() < 0)  return -3;

    return 0;
}


/*
 * Read min(file_size, size) bytes from a file into buffer.
 *
 * Responsibilities:
 * - Find the file's inode.
 * - Determine how many bytes to read.
 * - Read data from the file's blocks into buffer.
 * - Return the number of bytes read.
 */
int fs_read(const char *filename, void *buffer, int size)
{
    int idx = find_inode(filename);
    if (idx < 0)
        return -1;

    /* Read only as much as the file holds or the buffer fits */
    int bytes_to_read = inode_table[idx].size < size
                        ? inode_table[idx].size : size;

    char *dst     = (char *)buffer;
    int remaining = bytes_to_read;
    int num_blocks = (inode_table[idx].size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    for (int i = 0; i < num_blocks && remaining > 0; i++)
    {
        char buf[BLOCK_SIZE];
        if (read_block(inode_table[idx].blocks[i], buf) < 0)
            return -3;

        int to_copy = remaining < BLOCK_SIZE ? remaining : BLOCK_SIZE;
        memcpy(dst, buf, to_copy);

        dst       += to_copy;
        remaining -= to_copy;
    }

    return bytes_to_read;
}
