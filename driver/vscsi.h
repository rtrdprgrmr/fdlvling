#define READ_THREADS 10
#define RELOCATION_LIMIT_PERCENTAGE 90
#define RELOCATION_READ_MAX_GAP 32

typedef struct _CHUNK_INFO CHUNK_INFO, *PCHUNK_INFO;
typedef struct _THREAD THREAD, *PTHREAD;
typedef struct _LEVELING_CTX LEVELING_CTX, *PLEVELING_CTX;

struct _CHUNK_INFO {
	ULONG SCN;
	ULONG WCN;
	ULONG Used;
};

struct _THREAD {
	PREQUEST Req;
	PIRP Irp;
	PMDL Mdl;
	PUCHAR DataSystemAddress;
	PUCHAR DataBuffer;
	ULONG LBA;
	ULONG LEN;
	ULONG Transferred;
	BOOLEAN Marked;
	UCHAR IsRead;
	UCHAR Index;
	UCHAR FreeLink;
};

struct _LEVELING_CTX {
	PDEVICE_OBJECT DeviceObject;
	ULONG Version;
	ULONG CCN;
	ULONG SIZE_OF_MAP;
	ULONG MAX_CHUNKS;
	ULONG WEIGHT_LIFE;
	ULONG WEIGHT_SPEED;
	ULONG Magic;
	UCHAR CacheTimeout;
	BOOLEAN FlushFaithfully;
	KSPIN_LOCK SpinLock;
	KTIMER Timer;
	KDPC TimerDpc;
	LIST_ENTRY ReadIrpQueue;
	LIST_ENTRY OtherIrpQueue;
	ULONG ReadPending;
	ULONG OtherPending;
	UCHAR ReadActive;
	UCHAR OtherActive;
	UCHAR ThreadsMarked;
	BOOLEAN WaitingMarkedThreads;
	BOOLEAN Flushing;
	BOOLEAN Finalizing;
	BOOLEAN RelocationPrepared;
	BOOLEAN RelocationDone;
	BOOLEAN CacheDirty;
	BOOLEAN FlushPending;
	BOOLEAN TimerActive;
	UCHAR ReadFreeList;
	THREAD ReadThread[READ_THREADS];
	THREAD OtherThread[1];
	PULONG Map; // array size == SIZE_OF_MAP
	PCHUNK_INFO ChunkInfo; // array size == MAX_CHUNKS
	ULONG ChunkNo;
	ULONG NextChunkNo;
	CHUNK_CTL ChunkCtl[1];
	CHUNK_CTL NextChunkCtl[1];
	PREQUEST CacheReq;
	PUCHAR CacheMem;
	ULONG FinalizedPos;
	ULONG PurgePos;
	ULONG CachePos;
	ULONG CacheLBA[SIZE_OF_CACHE];
	PREQUEST RelocateReq;
	PUCHAR RelocateMem;
	ULONG RelocatePos;
	ULONG RelocateWindowStart;
	ULONG RelocateWindowEnd;
	ULONG RelocateTarget;
	ULONG RelocateSource;
};

extern KEVENT RecoveryEvent;
extern LIST_ENTRY RecoveryQueue;
VOID StartRecoveryWorkerThread();
VOID StopRecoveryWorkerThread();

BOOLEAN InitializeLevelingCtx(PDEVICE_OBJECT DeviceObject, PUCHAR MBR);
VOID FreeLevelingCtx(PDEVICE_OBJECT DeviceObject, BOOLEAN Wait);

NTSTATUS LevelingDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp, BOOLEAN IsRead);
NTSTATUS LevelingCheck(PDEVICE_OBJECT DeviceObject, PIRP Irp);
KDEFERRED_ROUTINE LevelingFlushDpc;

NTSTATUS CompleteNotReadyNoMedia(PDEVICE_OBJECT DeviceObject, PIRP Irp);
BOOLEAN GetDataSystemAddress(PTHREAD Thread, ULONG Length, BOOLEAN CheckOnly);
VOID MarkReadingThreads(PLEVELING_CTX Ctx);
VOID WaitMarkedThreadsAndWriteCompleteHandler(PLEVELING_CTX Ctx);

NTSTATUS ExecutePass(PDEVICE_OBJECT DeviceObject, PTHREAD Thread);
NTSTATUS ExecuteFlush(PDEVICE_OBJECT DeviceObject, PTHREAD Thread, BOOLEAN Finalize);
NTSTATUS ExecuteRW(PDEVICE_OBJECT DeviceObject, PTHREAD Thread);
VOID WriteCompleteHandler(PLEVELING_CTX Ctx);
VOID ExecuteCacheFillRead(PLEVELING_CTX Ctx, ULONG LBA, ULONG LEN);
VOID CacheFillReadZero(PLEVELING_CTX Ctx, ULONG LBA, ULONG LEN);
VOID ExecutePurgeCache(PLEVELING_CTX Ctx);
VOID ExecutePurgeChunk(PLEVELING_CTX Ctx);
VOID LevelingThreadCompletion(PDEVICE_OBJECT DeviceObject, PTHREAD Thread);
VOID ExecuteReadCompletion(PREQUEST Req);
VOID UpdateMap(PLEVELING_CTX Ctx, PULONG LBA, ULONG Sectors, ULONG PLBA);
VOID FindNextChunk(PLEVELING_CTX Ctx);
NTSTATUS ExecuteFlushHandler(PDEVICE_OBJECT DeviceObject, PTHREAD Thread);
VOID ExecuteRelocateCached(PLEVELING_CTX Ctx);
VOID ExecuteRelocate(PLEVELING_CTX Ctx);
VOID RelocateCompletion(PREQUEST Req);
