#include "uthash.h"
#include "scrmfs-defs.h"

#ifdef HAVE_CONTAINER_LIB
#include<container.h>
#endif /* HAVE_CONTAINER_LIB */

#define SCRMFS_SUCCESS     0
#define SCRMFS_FAILURE    -1
#define SCRMFS_ERR_NOSPC  -2
#define SCRMFS_ERR_IO     -3
#define SCRMFS_ERR_NAMETOOLONG -4
#define SCRMFS_ERR_NOENT  -5
#define SCRMFS_ERR_EXIST  -6
#define SCRMFS_ERR_NOTDIR -7
#define SCRMFS_ERR_NFILE  -8
#define SCRMFS_ERR_INVAL  -9
#define SCRMFS_ERR_OVERFLOW -10
#define SCRMFS_ERR_FBIG   -11
#define SCRMFS_ERR_BADF   -12
#define SCRMFS_ERR_ISDIR  -13
#define SCRMFS_ERR_NOMEM  -14

enum flock_enum {
    UNLOCKED,
    EX_LOCKED,
    SH_LOCKED
};

/* structure to represent file descriptors */
typedef struct {
    off_t pos;   /* current file pointer */
    int   read;  /* whether file is opened for read */
    int   write; /* whether file is opened for write */
} scrmfs_fd_t;

enum scrmfs_stream_orientation {
    SCRMFS_STREAM_ORIENTATION_NULL = 0,
    SCRMFS_STREAM_ORIENTATION_BYTE,
    SCRMFS_STREAM_ORIENTATION_WIDE,
};

/* structure to represent FILE* streams */
typedef struct {
    int    err;      /* stream error indicator flag */
    int    eof;      /* stream end-of-file indicator flag */
    int    fd;       /* file descriptor associated with stream */
    int    append;   /* whether file is opened in append mode */
    int    orient;   /* stream orientation, SCRMFS_STREAM_ORIENTATION_{NULL,BYTE,WIDE} */

    void*  buf;      /* pointer to buffer */
    int    buffree;  /* whether we need to free buffer */
    int    buftype;  /* _IOFBF fully buffered, _IOLBF line buffered, _IONBF unbuffered */
    size_t bufsize;  /* size of buffer in bytes */
    off_t  bufpos;   /* byte offset in file corresponding to start of buffer */
    size_t buflen;   /* number of bytes active in buffer */
    size_t bufdirty; /* whether data in buffer needs to be flushed */

    unsigned char* ubuf; /* ungetc buffer (we store bytes from end) */
    size_t ubufsize;     /* size of ungetc buffer in bytes */
    size_t ubuflen;      /* number of active bytes in buffer */

    unsigned char* _p; /* pointer to character in buffer */
    size_t         _r; /* number of bytes left at pointer */
} scrmfs_stream_t;

/* linked list of chunk information given to an external library wanting
 * to RDMA out a file from SCRMFS */
typedef struct {
    off_t chunk_id;
    int location;
    void *chunk_mr;
    off_t spillover_offset;
    struct chunk_list_t *next;
} chunk_list_t;

#ifdef HAVE_CONTAINER_LIB
typedef struct {
     cs_container_handle_t*  cs_container_handle;
     off_t container_size;
} scrmfs_container_t;
#endif /* HAVE_CONTAINER_LIB */

#define CHUNK_LOCATION_NULL      0
#define CHUNK_LOCATION_MEMFS     1
#define CHUNK_LOCATION_CONTAINER 2
#define CHUNK_LOCATION_SPILLOVER 3

typedef struct {
    int location;
    off_t id;
} scrmfs_chunkmeta_t;

typedef struct {
    off_t size;   /* current file size */
    off_t chunks; /* number of chunks currently allocated to file */
    scrmfs_chunkmeta_t* chunk_meta; /* meta data for chunks */
    int is_dir;  /* is this file a directory */
    pthread_spinlock_t fspinlock;
    enum flock_enum flock_status;
    #ifdef HAVE_CONTAINER_LIB
    scrmfs_container_t container_data;
    char * filename;
    #endif /* HAVE_CONTAINER_LIB */
} scrmfs_filemeta_t;

/* path to fid lookup struct */
typedef struct {
    int in_use;
    const char filename[SCRMFS_MAX_FILENAME];
} scrmfs_filename_t;
