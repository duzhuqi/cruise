#define SCRMFS_MAX_FILES        ( 128 )
#define SCRMFS_MAX_FILENAME     ( 128 )
#define SCRMFS_MAX_MEM          ( 1024 * 1024 * 1024 )
#define SCRMFS_CHUNK_SIZE       ( 1024 * 1024 * 1 )
#define SCRMFS_MAX_CHUNKS       ( SCRMFS_MAX_MEM / SCRMFS_CHUNK_SIZE )
#define SCRMFS_SUPERBLOCK_KEY   ( 1234 )
