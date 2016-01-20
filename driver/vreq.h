#define DEFAULT_RETRY 5

#define REQ_SUCCESS(Req) (NT_SUCCESS(Req->Status) && \
                          (SRB_STATUS_SUCCESS == SRB_STATUS(Req->SrbStatus) \
                           || SRB_STATUS_DATA_OVERRUN == SRB_STATUS(Req->SrbStatus)) \
                          && SCSISTAT_GOOD == Req->ScsiStatus)

typedef struct _REQUEST REQUEST, *PREQUEST;
typedef VOID REQUEST_COMPLETION(PREQUEST Req);
typedef REQUEST_COMPLETION* PREQUEST_COMPLETION;

struct _REQUEST {
	PDEVICE_OBJECT DeviceObject;
	PDEVICE_OBJECT NextLowerDriver;
	ULONG MaximumTransferLength;
	PIRP Irp;
	PUCHAR AssociatedMem;
	PMDL AssociatedMdl;
	PREQUEST_COMPLETION Handler;
	PREQUEST_COMPLETION Completion;
	PVOID Context;
	BOOLEAN Used;
	BOOLEAN Busy;
	UCHAR RetryLimit;
	UCHAR Retry;
	UCHAR OPC;
	PMDL Mdl;
	PUCHAR DataBuffer;
	ULONG PLBA;
	ULONG LEN;
	ULONG Transferred;
	UCHAR ScsiStatus;
	UCHAR SrbStatus;
	NTSTATUS Status;
	UCHAR SenseTransferred;
	UCHAR SenseBuffer[256];
	SCSI_REQUEST_BLOCK Srb[1];
};

PREQUEST CreateRequest(PDEVICE_OBJECT DeviceObject, PREQUEST_COMPLETION Completion, PVOID Context,
                       ULONG MemSize);
VOID FreeRequest(PREQUEST Req);
VOID ScsiCallRW(PREQUEST Req, UCHAR OPC, PMDL MDL, PUCHAR DataBuffer, ULONG PLBA, ULONG LEN,
                UCHAR RetryLimit);
VOID SyncScsiCallRW(PREQUEST Req, UCHAR OPC, PMDL MDL, PUCHAR DataBuffer, ULONG PLBA, ULONG LEN,
                    UCHAR RetryLimit);
VOID ScsiCallRW_AssociatedMem(PREQUEST Req, UCHAR OPC, ULONG pos, ULONG PLBA, ULONG LEN,
                              UCHAR RetryLimit);
VOID SyncScsiCallRW_AssociatedMem(PREQUEST Req, UCHAR OPC, ULONG pos, ULONG PLBA, ULONG LEN,
                                  UCHAR RetryLimit);
VOID ScsiCallReadCapacity(PREQUEST Req);
VOID ScsiCallFlushCache(PREQUEST Req);
VOID ScsiCallSrbFlush(PREQUEST Req);
