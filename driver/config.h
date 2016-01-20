#define LEVELING_VERSION 1000

#define SIZE_OF_CTL 0x100
#define SIZE_OF_CACHE 0x200
#define SIZE_OF_CHUNK 0x40000
#define LAST_PURGE_POS (SIZE_OF_CHUNK-SIZE_OF_CACHE)
#define LAST_CACHE_POS (SIZE_OF_CACHE-8)
#define NOT_MAPPED 0xffffffff

typedef struct _LEVELING_CONF LEVELING_CONF, *PLEVELING_CONF;
typedef struct _CHUNK_CTL CHUNK_CTL, *PCHUNK_CTL;
typedef struct _PURGE_CTL PURGE_CTL, *PPURGE_CTL;

struct _LEVELING_CONF {
	ULONG Version;
	ULONG CCN;
	ULONG SIZE_OF_MAP;
	ULONG WEIGHT_LIFE;
	ULONG WEIGHT_SPEED;
	ULONG Magic;
	UCHAR CacheTimeout;
	BOOLEAN FlushFaithfully;
	UCHAR Padding[6];
	ULONG Checksum[4];
};

struct _CHUNK_CTL {
	ULONG Version;
	ULONG CCN;
	ULONG SCN;
	ULONG WCN;
	ULONG NextChunkNo;
	ULONG NextChunkWCN;
	ULONG Start;
	ULONG End;
	ULONG LBA[LAST_PURGE_POS >> 3];
	ULONG Checksum[4];
};
STATIC_ASSERT(sizeof(CHUNK_CTL) <= (SIZE_OF_CTL << 9));

struct _PURGE_CTL {
	ULONG Version;
	ULONG CCN;
	ULONG SCN;
	ULONG ChunkNo;
	ULONG PurgePos;
	ULONG LBA[SIZE_OF_CACHE >> 3];
	ULONG Padding[3];
	ULONG Magic;
	ULONG Checksum[4];
};
STATIC_ASSERT(sizeof(PURGE_CTL) <= (1 << 9));

ULONG GetMaxChunks(PDEVICE_OBJECT DeviceObject);
NTSTATUS LevelingControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
