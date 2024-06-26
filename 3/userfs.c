#include "userfs.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <memory.h>
#include <stdio.h>

typedef struct filedesc filedesc;
typedef struct block block;
typedef struct file file;

enum
{
    BLOCK_SIZE = 512,
    MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block
{
    /** Block memory. */
    char *memory;
    /** How many bytes are occupied. */
    int occupied;
    /** Next block in the file. */
    block *next;
    /** Previous block in the file. */
    block *prev;
};

struct file
{
    /** Double-linked list of file blocks. */
    block *block_list;
    int blocks_count;
    /**
     * Last block in the list above for fast access to the end
     * of file.
     */
    block *last_block;
    /** How many file descriptors are opened on the file. */
    int refs;
    /** File name. */
    char *name;
    /** Files are stored in a double-linked list. */
    file *next;
    file *prev;

    bool is_removed;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc
{
    file *file;

    block *current_block;

    int offset;
    int current_block_index;

    int permissions;
    int fd;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

#define min(a, b) (a) < (b) ? (a) : (b)

#define max(a, b) (a) < (b) ? (b) : (a)

#define handle_err_no_file(fd)                                                              \
    do {                                                                                    \
        if ((fd) < 0 || (fd) > file_descriptor_capacity || !file_descriptors[(fd)])         \
        {                                                                                   \
            ufs_error_code = UFS_ERR_NO_FILE;                                               \
            return -1;                                                                      \
        }                                                                                   \
    } while (false)

#define handle_err_no_permissions(desc, permission1, permission2)                           \
    do {                                                                                    \
        if (!((desc)->permissions & ((permission1) | (permission2))))                       \
        {                                                                                   \
            ufs_error_code = UFS_ERR_NO_PERMISSION;                                         \
            return -1;                                                                      \
        }                                                                                   \
    } while (false)

#define handle_err_no_mem(new_size)                                                         \
    do {                                                                                    \
        if ((new_size) > MAX_FILE_SIZE)                                                     \
        {                                                                                   \
            ufs_error_code = UFS_ERR_NO_MEM;                                                \
            return -1;                                                                      \
        }                                                                                   \
    } while (false)

static void
assert_true(bool condition, const char *error_message)
{
    if (!condition)
    {
        perror(error_message);
        exit(EXIT_FAILURE);
    }
}

static void
*checked_realloc(void *ptr, size_t new_capacity)
{
    void *updated_ptr = realloc(ptr, new_capacity * sizeof(void *));
    assert_true(updated_ptr != NULL, "Error reallocating ptr");
    return updated_ptr;
}

static int
create_filedesc(file *f, int permissions)
{
    if (file_descriptor_count == file_descriptor_capacity)
    {
        file_descriptor_capacity += 4;
        file_descriptors = checked_realloc(file_descriptors, file_descriptor_capacity);
        memset(file_descriptors + file_descriptor_count, 0, 4 * sizeof(filedesc *));
    }

    int fd;
    for (fd = 0; fd <= file_descriptor_capacity; ++fd)
        if (!file_descriptors[fd])
            break;

    ++f->refs;
    file_descriptors[fd] = calloc(1, sizeof(filedesc));

    file_descriptors[fd]->fd = fd;
    file_descriptors[fd]->file = f;
    file_descriptors[fd]->current_block = f->block_list;

    // A newly created file does not have any allocated blocks
    // (lazy allocation approach), so their filedesc must have
    // current_block_index equal -1; otherwise - 0
    file_descriptors[fd]->current_block_index = file_descriptors[fd]->current_block ? 0 : -1;

    file_descriptors[fd]->permissions = permissions;
    file_descriptors[fd]->offset = 0;

    ++file_descriptor_count;
    return fd;
}

static void
init_filedesc_current_block(filedesc *desc)
{
    if (!desc->current_block && desc->file->block_list)
    {
        desc->current_block = desc->file->block_list;
        ++desc->current_block_index;
    }
}

static block
*add_block(file *f)
{
    block *new_block = calloc(1, sizeof(block));
    new_block->memory = calloc(BLOCK_SIZE, sizeof(char));

    if (!f->block_list)
        f->block_list = new_block;

    if (f->last_block)
        f->last_block->next = new_block;

    new_block->prev = f->last_block;
    f->last_block = new_block;

    ++f->blocks_count;
    return new_block;
}

#define RETURN_NEXT true
#define RETURN_PREV false

static block
*free_block(block *b, bool return_next)
{
    free(b->memory);
    block *to_return = return_next ? b->next : b->prev;

    if (to_return)
    {
        if (return_next) to_return->prev = NULL;
        else to_return->next = NULL;
    }

    free(b);
    return to_return;
}

static file
*add_file(const char *filename)
{
    file *new_file = calloc(1, sizeof(file));
    new_file->name = strdup(filename);

    if (!file_list)
        file_list = new_file;
    else
    {
        file *last_file;
        for (last_file = file_list; last_file->next; last_file = last_file->next);
        last_file->next = new_file;
        new_file->prev = last_file;
    }

    return new_file;
}

static void
free_file(file *f)
{
    for (block *block = f->block_list; block; block = free_block(block, RETURN_NEXT));
    free(f->name);
    free(f);
}

enum ufs_error_code
ufs_errno()
{
    return ufs_error_code;
}

int
ufs_open(const char *filename, int flags)
{
    bool needs_file_creation = flags % 2;
    file *current_file;

    for (current_file = file_list; current_file; current_file = current_file->next)
        if (strcmp(current_file->name, filename) == 0)
            goto filedesc_init;

    if (!needs_file_creation)
    {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    } else
        current_file = add_file(filename);


    filedesc_init:
    {
        int permissions = flags - needs_file_creation;

        if (!permissions)
            permissions = UFS_READ_WRITE;

        ufs_error_code = UFS_ERR_NO_ERR;
        return create_filedesc(current_file, permissions);
    }
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
    handle_err_no_file(fd);
    filedesc *desc = file_descriptors[fd];

    init_filedesc_current_block(desc);
    handle_err_no_permissions(desc, UFS_READ_WRITE, UFS_WRITE_ONLY);

    ssize_t write_size = 0, block_write_size;

    for (; write_size < (ssize_t) size;)
    {
        if (!desc->current_block)
        {
            if (desc->file->blocks_count >= MAX_FILE_SIZE / BLOCK_SIZE)
            {
                ufs_error_code = UFS_ERR_NO_MEM;
                return -1;
            }

            desc->current_block = add_block(desc->file);
            desc->current_block_index = desc->file->blocks_count - 1;
            desc->offset = 0;
        }

        block_write_size = BLOCK_SIZE - desc->offset;

        if (block_write_size <= 0)
        {
            desc->current_block = desc->current_block->next;
            ++desc->current_block_index;
            desc->offset = 0;

            continue;
        }

        block_write_size = min(block_write_size, (ssize_t) size - write_size);
        memcpy(desc->current_block->memory + desc->offset, buf + write_size, block_write_size);

        write_size += block_write_size;
        desc->offset += (int) block_write_size;

        desc->current_block->occupied = max(desc->current_block->occupied, desc->offset);
    }

    ufs_error_code = UFS_ERR_NO_ERR;
    return write_size;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
    handle_err_no_file(fd);
    filedesc *desc = file_descriptors[fd];

    init_filedesc_current_block(desc);
    handle_err_no_permissions(desc, UFS_READ_WRITE, UFS_READ_ONLY);

    ssize_t read_size = 0, block_read_size = BLOCK_SIZE - desc->offset;

    for (; read_size < (ssize_t) size && desc->current_block;
           block_read_size = BLOCK_SIZE - desc->offset)
    {
        if (block_read_size <= 0)
        {
            desc->current_block = desc->current_block->next;
            ++desc->current_block_index;
            desc->offset = 0;

            continue;
        }

        block_read_size = min(block_read_size, (ssize_t) size - read_size);
        block_read_size = min(block_read_size, desc->current_block->occupied - desc->offset);

        if (block_read_size <= 0)
            break;

        memcpy(buf + read_size, desc->current_block->memory + desc->offset, block_read_size);

        desc->offset += (int) block_read_size;
        read_size += block_read_size;
    }

    ufs_error_code = UFS_ERR_NO_ERR;
    return read_size;
}

int
ufs_close(int fd)
{
    handle_err_no_file(fd);
    filedesc *desc = file_descriptors[fd];

    --desc->file->refs;
    if (desc->file->is_removed && !desc->file->refs)
        free_file(desc->file);

    free(desc);
    file_descriptors[fd] = NULL;

    ufs_error_code = UFS_ERR_NO_ERR;
    return 0;
}

int
ufs_delete(const char *filename)
{
    for (file *current_file = file_list; current_file; current_file = current_file->next)
    {
        if (strcmp(current_file->name, filename) == 0)
        {
            if (current_file->prev)
                current_file->prev->next = current_file->next;
            else
                file_list = current_file->next;

            if (current_file->next)
                current_file->next->prev = current_file->prev;

            if (!current_file->refs)
                free_file(current_file);
            else
                current_file->is_removed = true;

            ufs_error_code = UFS_ERR_NO_ERR;
            return 0;
        }
    }

    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
}

void
ufs_destroy(void)
{
    for (file *current_file = file_list; current_file;)
    {
        file *to_remove = current_file;
        current_file = current_file->next;
        free_file(to_remove);
    }

    for (int fd = 0; fd < file_descriptor_capacity; ++fd)
        if (file_descriptors[fd])
            free(file_descriptors[fd]);

    free(file_descriptors);
}

int
ufs_resize(int fd, size_t new_size)
{
    handle_err_no_file(fd);
    handle_err_no_mem(new_size);

    file *f = file_descriptors[fd]->file;
    int i;

    int new_blocks_count = (int) new_size / BLOCK_SIZE;

    // Ceiling in case when we need additional block
    // for storing some portion of data with size less than
    // BLOCK_SIZE
    int initial_blocks_count = f->blocks_count;
    if (new_size % BLOCK_SIZE != 0)
        ++new_blocks_count;

    if (new_blocks_count >= initial_blocks_count)
    {
        for (i = f->blocks_count; i < new_blocks_count; ++i)
            add_block(f);

        ufs_error_code = UFS_ERR_NO_ERR;
        return 0;
    }

    for (i = new_blocks_count; i < initial_blocks_count; ++i)
    {
        if (f->last_block->prev)
        {
            f->last_block = free_block(f->last_block, RETURN_PREV);
            --f->blocks_count;
        }

        if (!f->last_block)
            f->block_list = NULL;
    }

    int occupied_remainder = (int) new_size % BLOCK_SIZE;
    if (new_size > 0 && !occupied_remainder)
        occupied_remainder += BLOCK_SIZE;

    if (f->last_block)
        f->last_block->occupied = occupied_remainder;

    filedesc *desc;
    int new_offset;

    for (i = 0; i < file_descriptor_capacity; ++i)
    {
        desc = file_descriptors[i];
        if (desc && desc->file == f)
        {
            if (desc->current_block_index >= f->blocks_count)
            {
                desc->current_block_index = f->blocks_count - 1;
                desc->current_block = f->last_block;
            }

            if (desc->current_block_index == f->blocks_count - 1)
            {
                new_offset = f->last_block ? f->last_block->occupied : 0;
                desc->offset = min(desc->offset, new_offset);
            }
        }
    }

    ufs_error_code = UFS_ERR_NO_ERR;
    return 0;
}
