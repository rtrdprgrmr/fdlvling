#include "headers.h"

NTSTATUS ExecuteFlush(PDEVICE_OBJECT DeviceObject, PTHREAD Thread, BOOLEAN Finalize) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	NTSTATUS status;
	ENTER_FUNCTION(LVL_FLUSH, "Ctx=%p", Ctx);
	ASSERT(!Ctx->Flushing);
	Ctx->Flushing = TRUE;
	Ctx->Finalizing = Finalize;
	status = ExecuteFlushHandler(DeviceObject, Thread);
	LEAVE_FUNCTION(LVL_FLUSH, "%s", NtStatusName(status));
	return status;
}

NTSTATUS ExecuteFlushHandler(PDEVICE_OBJECT DeviceObject, PTHREAD Thread) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	PIRP Irp = Thread->Irp;
	NTSTATUS status;
	ENTER_FUNCTION(LVL_FLUSH, "Ctx=%p", Ctx);
	ASSERT(Ctx->Flushing);
	if((Ctx->CachePos & 7) != 0) {
		ULONG LBA = Ctx->CacheLBA[Ctx->CachePos - 1] + 1;
		ULONG LEN = 8 - (Ctx->CachePos & 7);
		TRACE(LVL_FLUSH, "Flush Cache Fill Read Ctx=%p", Ctx);
		ASSERT((LBA >> 3) < Ctx->SIZE_OF_MAP);
		if(Ctx->Map[LBA >> 3] != NOT_MAPPED) {
			if(NULL != Irp) {
				IoMarkIrpPending(Irp);
			}
			ExecuteCacheFillRead(Ctx, LBA, LEN);
			status = STATUS_PENDING;
			goto exit;
		}
		CacheFillReadZero(Ctx, LBA, LEN);
	}
	if(Ctx->CacheDirty) {
		TRACE(LVL_FLUSH, "Flush Purge Cache Ctx=%p", Ctx);
		if(NULL != Irp) {
			IoMarkIrpPending(Irp);
		}
		ExecutePurgeCache(Ctx);
		status = STATUS_PENDING;
		goto exit;
	}
	if(Ctx->Finalizing && Ctx->FinalizedPos < Ctx->PurgePos) {
		TRACE(LVL_FLUSH, "Flush Purge Chunk Ctx=%p", Ctx);
		if(NULL != Irp) {
			IoMarkIrpPending(Irp);
		}
		ExecutePurgeChunk(Ctx);
		status = STATUS_PENDING;
		goto exit;
	}
	Ctx->Flushing = FALSE;
	if(NULL != Irp && IRP_MJ_SHUTDOWN == IoGetCurrentIrpStackLocation(Irp)->MajorFunction) {
		TRACE(LVL_FLUSH, "Flush Shutdown Ctx=%p", Ctx);
		Irp->IoStatus.Status = status = STATUS_SUCCESS;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		Thread->Irp = NULL;
	} else {
		TRACE(LVL_FLUSH, "Flush Pass Ctx=%p", Ctx);
		status = ExecutePass(DeviceObject, Thread);
	}
exit:
	LEAVE_FUNCTION(LVL_FLUSH, "%s", NtStatusName(status));
	return status;
}
