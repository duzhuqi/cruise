/*
 * Copyright (c) 2014, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Written by
 *   Raghunath Rajachandrasekar <rajachan@cse.ohio-state.edu>
 *   Kathryn Mohror <kathryn@llnl.gov>
 *   Adam Moody <moody20@llnl.gov>
 * LLNL-CODE-642432.
 * All rights reserved.
 * This file is part of CRUISE.
 * For details, see https://github.com/hpc/cruise
 * Please also read this file COPYRIGHT
*/

#include "cruise-runtime-config.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <search.h>
#include <assert.h>
#include <libgen.h>
#include <limits.h>
#define __USE_GNU
#include <pthread.h>

#include "cruise-internal.h"

extern int cruise_spillover_max_chunks;
/* given a file id and logical chunk id, return pointer to meta data
 * for specified chunk, return NULL if not found */
static cruise_chunkmeta_t* cruise_get_chunkmeta(int fid, int cid)
{
    /* lookup file meta data for specified file id */
    cruise_filemeta_t* meta = cruise_get_meta_from_fid(fid);
    if (meta != NULL) {
        /* now lookup chunk meta data for specified chunk id */
        if (cid >= 0 && cid < cruise_max_chunks) {
           cruise_chunkmeta_t* chunk_meta = &(meta->chunk_meta[cid]);
           return chunk_meta;
        }
    }

    /* failed to find file or chunk id is out of range */
    return (cruise_chunkmeta_t *)NULL;
}

/* ---------------------------------------
 * Operations on file chunks
 * --------------------------------------- */

/* given a logical chunk id and an offset within that chunk, return the pointer
 * to the memory location corresponding to that location */
static inline void* cruise_compute_chunk_buf(
  const cruise_filemeta_t* meta,
  int logical_id,
  off_t logical_offset)
{
    /* get pointer to chunk meta */
    const cruise_chunkmeta_t* chunk_meta = &(meta->chunk_meta[logical_id]);

    /* identify physical chunk id */
    int physical_id = chunk_meta->id;

    /* compute the start of the chunk */
    char *start = NULL;
    if (physical_id < cruise_max_chunks) {
    	/* Teng: convert data type to handle larger workload */
        start = cruise_chunks + ((off_t)physical_id << cruise_chunk_bits);
    } else {
        /* chunk is in spill over */
        debug("wrong chunk ID\n");
        return NULL;
    }

    /* now add offset */
    char* buf = start + logical_offset;
    return (void*)buf;
}

/* given a chunk id and an offset within that chunk, return the offset
 * in the spillover file corresponding to that location */
static inline off_t cruise_compute_spill_offset(
  const cruise_filemeta_t* meta,
  int logical_id,
  off_t logical_offset)
{
    /* get pointer to chunk meta */
    const cruise_chunkmeta_t* chunk_meta = &(meta->chunk_meta[logical_id]);

    /* identify physical chunk id */
    int physical_id = chunk_meta->id;

    /* compute start of chunk in spill over device */
    off_t start = 0;
    if (physical_id < cruise_max_chunks) {
        debug("wrong spill-chunk ID\n");
        return -1;
    } else {
        /* compute buffer loc within spillover device chunk */
        /* account for the cruise_max_chunks added to identify location when
         * grabbing this chunk */
        start = ((off_t)(physical_id - cruise_max_chunks) << cruise_chunk_bits);
    }
    off_t buf = start + logical_offset;
    return buf;
}

/* allocate a new chunk for the specified file and logical chunk id */
static int cruise_chunk_alloc(int fid, cruise_filemeta_t* meta, int chunk_id)
{
    /* get pointer to chunk meta data */
    cruise_chunkmeta_t* chunk_meta = &(meta->chunk_meta[chunk_id]);
    
    /* allocate a chunk and record its location */
    if (cruise_use_memfs) {
        /* allocate a new chunk from memory */
        cruise_stack_lock();
        int id = cruise_stack_pop(free_chunk_stack);
        cruise_stack_unlock();

        /* if we got one return, otherwise try spill over */
        if (id >= 0) {
            /* got a chunk from memory */
            chunk_meta->location = CHUNK_LOCATION_MEMFS;
            chunk_meta->id = id;
        } else if (cruise_use_spillover) {
            /* shm segment out of space, grab a block from spill-over device */
            debug("getting blocks from spill-over device\n");

            /* TODO: missing lock calls? */
            /* add cruise_max_chunks to identify chunk location */
            cruise_stack_lock();
            id = cruise_stack_pop(free_spillchunk_stack) + cruise_max_chunks;
            cruise_stack_unlock();
            if (id < cruise_max_chunks) {
                debug("spill-over device out of space (%d)\n", id);
                return CRUISE_ERR_NOSPC;
            }

            /* got one from spill over */
            chunk_meta->location = CHUNK_LOCATION_SPILLOVER;
            chunk_meta->id = id;
        } else {
            /* spill over isn't available, so we're out of space */
            debug("memfs out of space (%d)\n", id);
            return CRUISE_ERR_NOSPC;
        }
    } else if (cruise_use_spillover) {
        /* memory file system is not enabled, but spill over is */

        /* shm segment out of space, grab a block from spill-over device */
        debug("getting blocks from spill-over device \n");

        /* TODO: missing lock calls? */
        /* add cruise_max_chunks to identify chunk location */
        cruise_stack_lock();
        int id = cruise_stack_pop(free_spillchunk_stack) + cruise_max_chunks;
        cruise_stack_unlock();
        if (id < cruise_max_chunks) {
            debug("spill-over device out of space (%d)\n", id);
            return CRUISE_ERR_NOSPC;
        }

        /* got one from spill over */
        chunk_meta->location = CHUNK_LOCATION_SPILLOVER;
        chunk_meta->id = id;
    } else {
        /* don't know how to allocate chunk */
        chunk_meta->location = CHUNK_LOCATION_NULL;
        return CRUISE_ERR_IO;
    }

    return CRUISE_SUCCESS;
}

static int cruise_chunk_free(int fid, cruise_filemeta_t* meta, int chunk_id)
{
    /* get pointer to chunk meta data */
    cruise_chunkmeta_t* chunk_meta = &(meta->chunk_meta[chunk_id]);

    /* get physical id of chunk */
    int id = chunk_meta->id;
    debug("free chunk %d from location %d\n", id, chunk_meta->location);

    /* determine location of chunk */
    if (chunk_meta->location == CHUNK_LOCATION_MEMFS) {
        cruise_stack_lock();
        cruise_stack_push(free_chunk_stack, id);
        cruise_stack_unlock();
    } else if (chunk_meta->location == CHUNK_LOCATION_SPILLOVER) {
        /* TODO: free spill over chunk */
    } else {
        /* unkwown chunk location */
        debug("unknown chunk location %d\n", chunk_meta->location);
        return CRUISE_ERR_IO;
    }

    /* update location of chunk */
    chunk_meta->location = CHUNK_LOCATION_NULL;

    return CRUISE_SUCCESS;
}

/* read data from specified chunk id, chunk offset, and count into user buffer,
 * count should fit within chunk starting from specified offset */
static int cruise_chunk_read(
  cruise_filemeta_t* meta, /* pointer to file meta data */
  int chunk_id,            /* logical chunk id to read data from */
  off_t chunk_offset,      /* logical offset within chunk to read from */
  void* buf,               /* buffer to store data to */
  size_t count)            /* number of bytes to read */
{
    /* get chunk meta data */
    cruise_chunkmeta_t* chunk_meta = &(meta->chunk_meta[chunk_id]);

    /* determine location of chunk */
    if (chunk_meta->location == CHUNK_LOCATION_MEMFS) {
        /* just need a memcpy to read data */
        void* chunk_buf = cruise_compute_chunk_buf(meta, chunk_id, chunk_offset);
        memcpy(buf, chunk_buf, count);
    } else if (chunk_meta->location == CHUNK_LOCATION_SPILLOVER) {
        /* spill over to a file, so read from file descriptor */
        //MAP_OR_FAIL(pread);
        off_t spill_offset = cruise_compute_spill_offset(meta, chunk_id, chunk_offset);
        ssize_t rc = pread(cruise_spilloverblock, buf, count, spill_offset);
        /* TODO: check return code for errors */
    } else {
        /* unknown chunk type */
        debug("unknown chunk type in read\n");
        return CRUISE_ERR_IO;
    }

    /* assume read was successful if we get to here */
    return CRUISE_SUCCESS;
}

/* read data from specified chunk id, chunk offset, and count into user buffer,
 * count should fit within chunk starting from specified offset */
static int cruise_chunk_write(
  cruise_filemeta_t* meta, /* pointer to file meta data */
  int chunk_id,            /* logical chunk id to write to */
  off_t chunk_offset,      /* logical offset within chunk to write to */
  const void* buf,         /* buffer holding data to be written */
  size_t count)            /* number of bytes to write */
{
    /* get chunk meta data */
    cruise_chunkmeta_t* chunk_meta = &(meta->chunk_meta[chunk_id]);

    /* determine location of chunk */
    if (chunk_meta->location == CHUNK_LOCATION_MEMFS) {
        /* just need a memcpy to write data */
        void* chunk_buf = cruise_compute_chunk_buf(meta, chunk_id, chunk_offset);
        memcpy(chunk_buf, buf, count);
//        _intel_fast_memcpy(chunk_buf, buf, count);
//        cruise_memcpy(chunk_buf, buf, count);
    } else if (chunk_meta->location == CHUNK_LOCATION_SPILLOVER) {
        /* spill over to a file, so write to file descriptor */
        //MAP_OR_FAIL(pwrite);
        off_t spill_offset = cruise_compute_spill_offset(meta, chunk_id, chunk_offset);
        ssize_t rc = pwrite(cruise_spilloverblock, buf, count, spill_offset);
        if (rc < 0)  {
            perror("pwrite failed");
        }
        /* TODO: check return code for errors */
    } else {
        /* unknown chunk type */
        debug("unknown chunk type in read\n");
        return CRUISE_ERR_IO;
    }

    /* assume read was successful if we get to here */
    return CRUISE_SUCCESS;
}

/* ---------------------------------------
 * Operations on file storage
 * --------------------------------------- */

/* if length is greater than reserved space, reserve space up to length */
int cruise_fid_store_fixed_extend(int fid, cruise_filemeta_t* meta, off_t length)
{
    /* determine whether we need to allocate more chunks */
    off_t maxsize = meta->chunks << cruise_chunk_bits;
    if (length > maxsize) {
        /* compute number of additional bytes we need */
        off_t additional = length - maxsize;
        while (additional > 0) {
            /* check that we don't overrun max number of chunks for file */
            if (meta->chunks == cruise_max_chunks + cruise_spillover_max_chunks) {
                debug("failed to allocate chunk\n");
                return CRUISE_ERR_NOSPC;
            }

            /* allocate a new chunk */
            int rc = cruise_chunk_alloc(fid, meta, meta->chunks);
            if (rc != CRUISE_SUCCESS) {
                debug("failed to allocate chunk\n");
                return CRUISE_ERR_NOSPC;
            }

            /* increase chunk count and subtract bytes from the number we need */
            meta->chunks++;
            additional -= cruise_chunk_size;
        }
    }

    return CRUISE_SUCCESS;
}

/* if length is shorter than reserved space, give back space down to length */
int cruise_fid_store_fixed_shrink(int fid, cruise_filemeta_t* meta, off_t length)
{
    /* determine the number of chunks to leave after truncating */
    off_t num_chunks = 0;
    if (length > 0) {
        num_chunks = (length >> cruise_chunk_bits) + 1;
    }

    /* clear off any extra chunks */
    while (meta->chunks > num_chunks) {
        meta->chunks--;
        cruise_chunk_free(fid, meta, meta->chunks);
    }

    return CRUISE_SUCCESS;
}

/* read data from file stored as fixed-size chunks */
int cruise_fid_store_fixed_read(int fid, cruise_filemeta_t* meta, off_t pos, void* buf, size_t count)
{
    int rc;

    /* get pointer to position within first chunk */
    int chunk_id = pos >> cruise_chunk_bits;
    off_t chunk_offset = pos & cruise_chunk_mask;

    /* determine how many bytes remain in the current chunk */
    size_t remaining = cruise_chunk_size - chunk_offset;
    if (count <= remaining) {
        /* all bytes for this read fit within the current chunk */
        rc = cruise_chunk_read(meta, chunk_id, chunk_offset, buf, count);
    } else {
        /* read what's left of current chunk */
        char* ptr = (char*) buf;
        rc = cruise_chunk_read(meta, chunk_id, chunk_offset, (void*)ptr, remaining);
        ptr += remaining;
   
        /* read from the next chunk */
        size_t processed = remaining;
        while (processed < count && rc == CRUISE_SUCCESS) {
            /* get pointer to start of next chunk */
            chunk_id++;

            /* compute size to read from this chunk */
            size_t num = count - processed;
            if (num > cruise_chunk_size) {
                num = cruise_chunk_size;
            }
   
            /* read data */
            rc = cruise_chunk_read(meta, chunk_id, 0, (void*)ptr, num);
            ptr += num;

            /* update number of bytes written */
            processed += num;
        }
    }

    return rc;
}

/* write data to file stored as fixed-size chunks */
int cruise_fid_store_fixed_write(int fid, cruise_filemeta_t* meta, off_t pos, const void* buf, size_t count)
{
    int rc;

    /* get pointer to position within first chunk */
    int chunk_id = pos >> cruise_chunk_bits;
    off_t chunk_offset = pos & cruise_chunk_mask;

    /* determine how many bytes remain in the current chunk */
    size_t remaining = cruise_chunk_size - chunk_offset;
    if (count <= remaining) {
        /* all bytes for this write fit within the current chunk */
        rc = cruise_chunk_write(meta, chunk_id, chunk_offset, buf, count);
    } else {
        /* otherwise, fill up the remainder of the current chunk */
        char* ptr = (char*) buf;
        rc = cruise_chunk_write(meta, chunk_id, chunk_offset, (void*)ptr, remaining);
        ptr += remaining;

        /* then write the rest of the bytes starting from beginning
         * of chunks */
        size_t processed = remaining;
        while (processed < count && rc == CRUISE_SUCCESS) {
            /* get pointer to start of next chunk */
            chunk_id++;

            /* compute size to write to this chunk */
            size_t num = count - processed;
            if (num > cruise_chunk_size) {
              num = cruise_chunk_size;
            }
   
            /* write data */
            rc = cruise_chunk_write(meta, chunk_id, 0, (void*)ptr, num);
            ptr += num;

            /* update number of bytes processed */
            processed += num;
        }
    }

    return rc;
}
