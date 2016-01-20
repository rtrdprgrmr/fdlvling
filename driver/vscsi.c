#include "headers.h"

NTSTATUS ExecuteReadHandler(PDEVICE_OBJECT DeviceObject, PTHREAD Thread);
NTSTATUS ExecuteWriteHandler(PDEVICE_OBJECT DeviceObject, PTHREAD Thread);
VOID CacheFillReadCompletion(PREQUEST Req);
VOID PurgeCacheCompletion(PREQUEST Req);
VOID PurgeChunkCompletion(PREQUEST Req);
VOID FindNextChunk(PLEVELING_CTX Ctx);
VOID PrepareRelocate(PLEVELING_CTX Ctx);
VOID PrepareRelocateCompletion(PREQUEST Req);

NTSTATUS ExecuteRW(PDEVICE_OBJECT DeviceObject, PTHREAD Thread) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	PIRP Irp = Thread->Irp;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
	UCHAR OPC = Srb->Cdb[0];
	ULONG LBA = RtlUlongByteSwap(*(ULONG*)(Srb->Cdb + 2));
	ULONG LEN = RtlUshortByteSwap(*(USHORT*)(Srb->Cdb + 7));
	NTSTATUS status;
	ENTER_FUNCTION(LVL_RW, "Thread=%p", Thread);
	if(LBA + LEN > Ctx->SIZE_OF_MAP << 3 || !GetDataSystemAddress(Thread, LEN << 9, TRUE)) {
		SetCheckCondition(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
		Irp->IoStatus.Information = Srb->DataTransferLength = 0;
		Irp->IoStatus.Status = status = STATUS_IO_DEVICE_ERROR;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		Thread->Irp = NULL;
		goto exit;
	}
	Thread->DataBuffer = Srb->DataBuffer;
	Thread->LBA = LBA;
	Thread->LEN = LEN;
	Thread->Transferred = 0;
	if(OPC == SCSIOP_READ) {
		ASSERT(Thread->IsRead);
		TRACE(LVL_RW, "R(%p) LBA=%x LEN=%x", Thread->Req, LBA, LEN);
		status = ExecuteReadHandler(DeviceObject, Thread);
	} else if(OPC == SCSIOP_WRITE) {
		ASSERT(!Thread->IsRead);
		TRACE(LVL_RW, "W(%p) LBA=%x LEN=%x", Ctx->CacheReq, LBA, LEN);
		status = ExecuteWriteHandler(DeviceObject, Thread);
	}
exit:
	LEAVE_FUNCTION(LVL_RW, "%s", NtStatusName(status));
	return status;
}

ULONG GetCachedPos(PLEVELING_CTX Ctx, ULONG LBA) {
	ULONG i;
	for(i = 0; i < Ctx->CachePos; i += 8) {
		if(Ctx->CacheLBA[i] == (LBA & -8)) {
			ULONG pos = i + (LBA & 7);
			if(pos < Ctx->CachePos) {
				return pos;
			}
		}
	}
	return NOT_MAPPED;
}

NTSTATUS ExecuteReadHandler(PDEVICE_OBJECT DeviceObject, PTHREAD Thread) {
	PIRP Irp = Thread->Irp;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	ULONG LBA, LEN, pos, PLBA, len;
	NTSTATUS status;
	KIRQL OldIrql;
	ENTER_FUNCTION(LVL_RW, "Thread=%p", Thread);
loop:
	if(VLTL_MODE_ACTIVE != DeviceExtension->Mode) {
		status = CompleteNotReadyNoMedia(DeviceObject, Irp);
		Thread->Irp = NULL;
		goto exit;
	}
	LBA = Thread->LBA + Thread->Transferred;
	LEN = Thread->LEN - Thread->Transferred;
	if(0 == LEN) {
		Srb->SrbStatus = SRB_STATUS_SUCCESS;
		Srb->ScsiStatus = SCSISTAT_GOOD;
		Srb->SenseInfoBufferLength = 0;
		Irp->IoStatus.Information = Srb->DataTransferLength = (Thread->Transferred << 9);
		Irp->IoStatus.Status = status = STATUS_SUCCESS;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		Thread->Irp = NULL;
		goto exit;
	}
	KeAcquireSpinLock(&Ctx->SpinLock, &OldIrql);
	pos = GetCachedPos(Ctx, LBA);
	if(pos != NOT_MAPPED) {
		if(NULL == Thread->DataSystemAddress) {
			KeReleaseSpinLock(&Ctx->SpinLock, OldIrql);
			if(!GetDataSystemAddress(Thread, Thread->LEN << 9, FALSE)) {
				Irp->IoStatus.Information = 0;
				Irp->IoStatus.Status = status = STATUS_INSUFFICIENT_RESOURCES;
				IoCompleteRequest(Irp, IO_NO_INCREMENT);
				Thread->Irp = NULL;
				goto exit;
			}
			goto loop;
		}
		ASSERT(Ctx->CacheLBA[pos] == LBA);
		for(len = 1; len < LEN && pos + len < Ctx->CachePos; len++) {
			if(Ctx->CacheLBA[pos + len] != LBA + len) {
				break;
			}
		}
		RtlCopyMemory(Thread->DataSystemAddress + (Thread->Transferred << 9), Ctx->CacheMem + (pos << 9),
		              len << 9);
		KeReleaseSpinLock(&Ctx->SpinLock, OldIrql);
		Thread->Transferred += len;
		goto loop;
	}
	ASSERT((LBA >> 3) < Ctx->SIZE_OF_MAP);
	if(Ctx->Map[LBA >> 3] == NOT_MAPPED) {
		KeReleaseSpinLock(&Ctx->SpinLock, OldIrql);
		if(NULL == Thread->DataSystemAddress) {
			if(!GetDataSystemAddress(Thread, Thread->LEN << 9, FALSE)) {
				Irp->IoStatus.Information = 0;
				Irp->IoStatus.Status = status = STATUS_INSUFFICIENT_RESOURCES;
				IoCompleteRequest(Irp, IO_NO_INCREMENT);
				Thread->Irp = NULL;
				goto exit;
			}
		}
		RtlZeroMemory(Thread->DataSystemAddress + (Thread->Transferred << 9), 512);
		Thread->Transferred++;
		goto loop;
	}
	ASSERT((LBA >> 3) < Ctx->SIZE_OF_MAP);
	PLBA = Ctx->Map[LBA >> 3] + (LBA & 7);
	for(len = 1; len < LEN; len++) {
		if((LBA + len) & 7) {
			continue;
		}
		ASSERT(((LBA + len) >> 3) < Ctx->SIZE_OF_MAP);
		if(PLBA + len != Ctx->Map[(LBA + len) >> 3]) {
			break;
		}
		if(GetCachedPos(Ctx, LBA + len) != NOT_MAPPED) {
			break;
		}
	}
	KeReleaseSpinLock(&Ctx->SpinLock, OldIrql);
	IoMarkIrpPending(Irp);
	ScsiCallRW(Thread->Req, SCSIOP_READ, Thread->Mdl, Thread->DataBuffer + (Thread->Transferred << 9),
	           PLBA, len, 0);
	status = STATUS_PENDING;
exit:
	LEAVE_FUNCTION(LVL_RW, "%s", NtStatusName(status));
	return status;
}

VOID ExecuteReadCompletion(PREQUEST Req) {
	PDEVICE_OBJECT DeviceObject = Req->DeviceObject;
	PTHREAD Thread = Req->Context;
	ENTER_FUNCTION(LVL_RW, "Thread=%p", Thread);
	if(!REQ_SUCCESS(Req)) {
		PIRP Irp = Thread->Irp;
		PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
		PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
		Srb->SrbStatus = Req->SrbStatus;
		Srb->ScsiStatus = Req->ScsiStatus;
		Srb->DataTransferLength = Req->Transferred << 9;
		Srb->SenseInfoBufferLength = min(Srb->SenseInfoBufferLength, Req->SenseTransferred);
		if(0 < Srb->SenseInfoBufferLength) {
			RtlCopyMemory(Srb->SenseInfoBuffer, Req->SenseBuffer, Srb->SenseInfoBufferLength);
			Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
		}
		Irp->IoStatus.Information = Srb->DataTransferLength;
		Irp->IoStatus.Status = Req->Status;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		Thread->Irp = NULL;
		LevelingThreadCompletion(DeviceObject, Thread);
		goto exit;
	}
	Thread->Transferred += Req->Transferred;
	if(STATUS_PENDING != ExecuteReadHandler(DeviceObject, Thread)) {
		LevelingThreadCompletion(DeviceObject, Thread);
	}
exit:
	LEAVE_FUNCTION(LVL_RW, "");
}

NTSTATUS ExecuteWriteHandler(PDEVICE_OBJECT DeviceObject, PTHREAD Thread) {
	PIRP Irp = Thread->Irp;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	ULONG LBA, LEN, pos, len, i;
	NTSTATUS status;
	KIRQL OldIrql;
	ENTER_FUNCTION(LVL_RW, "Ctx=%p", Ctx);
	if(NULL == Thread->DataSystemAddress) {
		if(!GetDataSystemAddress(Thread, Thread->LEN << 9, FALSE)) {
			Irp->IoStatus.Information = 0;
			Irp->IoStatus.Status = status = STATUS_INSUFFICIENT_RESOURCES;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
			Thread->Irp = NULL;
			goto exit;
		}
	}
loop:
	if(VLTL_MODE_ACTIVE != DeviceExtension->Mode) {
		status = CompleteNotReadyNoMedia(DeviceObject, Irp);
		Thread->Irp = NULL;
		goto exit;
	}
	LBA = Thread->LBA + Thread->Transferred;
	LEN = Thread->LEN - Thread->Transferred;
	if(0 == LEN) {
		PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
		PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
		Srb->SrbStatus = SRB_STATUS_SUCCESS;
		Srb->ScsiStatus = SCSISTAT_GOOD;
		Srb->SenseInfoBufferLength = 0;
		Irp->IoStatus.Information = Srb->DataTransferLength = (Thread->Transferred << 9);
		Irp->IoStatus.Status = status = STATUS_SUCCESS;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		Thread->Irp = NULL;
		goto exit;
	}
	if(Ctx->PurgePos >= LAST_PURGE_POS) {
		ASSERT(Ctx->PurgePos == LAST_PURGE_POS);
		if(Ctx->FinalizedPos < LAST_PURGE_POS) {
			MarkReadingThreads(Ctx);
			IoMarkIrpPending(Irp);
			ExecutePurgeChunk(Ctx);
			status = STATUS_PENDING;
			goto exit;
		}
		ASSERT(Ctx->ChunkInfo[Ctx->NextChunkNo].Used == 0);
		ASSERT(Ctx->CachePos == 0);
		Ctx->ChunkInfo[Ctx->NextChunkNo].SCN = Ctx->ChunkInfo[Ctx->ChunkNo].SCN + 1;
		Ctx->ChunkNo = Ctx->NextChunkNo;
		Ctx->PurgePos = (0 == Ctx->ChunkNo) ? SIZE_OF_CACHE : 0;
		Ctx->FinalizedPos = Ctx->PurgePos;
		FindNextChunk(Ctx);
	}
	if(!Ctx->RelocationPrepared) {
		IoMarkIrpPending(Irp);
		PrepareRelocate(Ctx);
		status = STATUS_PENDING;
		goto exit;
	}
	if(!Ctx->RelocationDone) {
		ExecuteRelocateCached(Ctx);
		if(!Ctx->RelocationDone) {
			IoMarkIrpPending(Irp);
			ExecuteRelocate(Ctx);
			status = STATUS_PENDING;
			goto exit;
		}
	}
	pos = GetCachedPos(Ctx, LBA);
	if(pos != NOT_MAPPED) {
		ASSERT(Ctx->CacheLBA[pos] == LBA);
		for(len = 1; len < LEN && pos + len < Ctx->CachePos; len++) {
			if(Ctx->CacheLBA[pos + len] != LBA + len) {
				break;
			}
		}
		RtlCopyMemory(Ctx->CacheMem + (pos << 9), Thread->DataSystemAddress + (Thread->Transferred << 9),
		              len << 9);
		Thread->Transferred += len;
		goto loop;
	}
	pos = Ctx->CachePos;
	if(pos >= LAST_CACHE_POS) {
		ASSERT(pos == LAST_CACHE_POS);
		IoMarkIrpPending(Irp);
		ExecutePurgeCache(Ctx);
		status = STATUS_PENDING;
		goto exit;
	}
	if((pos & 7) != 0) {
		ULONG NextLBA = Ctx->CacheLBA[pos - 1] + 1;
		if(NextLBA == LBA) {
			goto cache_write;
		}
		LBA = NextLBA;
		len = 8 - (pos & 7);
		goto cache_fill_read;
	}
	if((LBA & 7) != 0) {
		len = LBA & 7;
		if((LBA & -8) == ((LBA + LEN) & -8)) {
			len = 8;
		}
		LBA = (LBA & -8);
		goto cache_fill_read;
	}
cache_write:
	ASSERT((pos & 7) == (LBA & 7));
	for(len = 1; len < LEN && pos + len < LAST_CACHE_POS; len++) {
		if((LBA + len) & 7) {
			continue;
		}
		if(GetCachedPos(Ctx, LBA + len) != NOT_MAPPED) {
			break;
		}
	}
	for(i = 0; i < len; i++) {
		ASSERT(((LBA + i) >> 3) < Ctx->SIZE_OF_MAP);
		Ctx->CacheLBA[pos + i] = LBA + i;
	}
	RtlCopyMemory(Ctx->CacheMem + (pos << 9), Thread->DataSystemAddress + (Thread->Transferred << 9),
	              len << 9);
	KeAcquireSpinLock(&Ctx->SpinLock, &OldIrql);
	Ctx->CachePos += len;
	KeReleaseSpinLock(&Ctx->SpinLock, OldIrql);
	Thread->Transferred += len;
	Ctx->CacheDirty = TRUE;
	goto loop;
cache_fill_read:
	ASSERT((LBA >> 3) < Ctx->SIZE_OF_MAP);
	if(Ctx->Map[LBA >> 3] != NOT_MAPPED) {
		IoMarkIrpPending(Irp);
		ExecuteCacheFillRead(Ctx, LBA, len);
		status = STATUS_PENDING;
		goto exit;
	}
	CacheFillReadZero(Ctx, LBA, len);
	goto loop;
exit:
	LEAVE_FUNCTION(LVL_RW, "%s", NtStatusName(status));
	return status;
}

VOID WriteCompleteHandler(PLEVELING_CTX Ctx) {
	PDEVICE_OBJECT DeviceObject = Ctx->DeviceObject;
	PTHREAD Thread = Ctx->OtherThread ;
	ENTER_FUNCTION(LVL_RW, "Ctx=%p", Ctx);
	if(Ctx->Flushing) {
		if(STATUS_PENDING != ExecuteFlushHandler(DeviceObject, Thread)) {
			LevelingThreadCompletion(DeviceObject, Thread);
		}
	} else {
		if(STATUS_PENDING != ExecuteWriteHandler(DeviceObject, Thread)) {
			LevelingThreadCompletion(DeviceObject, Thread);
		}
	}
	LEAVE_FUNCTION(LVL_RW, "");
}

VOID CacheFillReadZero(PLEVELING_CTX Ctx, ULONG LBA, ULONG LEN) {
	ULONG pos = Ctx->CachePos;
	ULONG i;
	KIRQL OldIrql;
	ENTER_FUNCTION(LVL_RW, "Ctx=%p", Ctx);
	ASSERT(Ctx->Map[LBA >> 3] == NOT_MAPPED);
	ASSERT((LBA & -8) == ((LBA + LEN - 1) & -8));
	for(i = 0; i < LEN; i++) {
		ASSERT(((LBA + i) >> 3) < Ctx->SIZE_OF_MAP);
		Ctx->CacheLBA[pos + i] = LBA + i;
	}
	RtlZeroMemory(Ctx->CacheMem + (pos << 9), LEN << 9);
	KeAcquireSpinLock(&Ctx->SpinLock, &OldIrql);
	Ctx->CachePos += LEN;
	KeReleaseSpinLock(&Ctx->SpinLock, OldIrql);
	LEAVE_FUNCTION(LVL_RW, "");
}

VOID ExecuteCacheFillRead(PLEVELING_CTX Ctx, ULONG LBA, ULONG LEN) {
	ULONG pos = Ctx->CachePos;
	ULONG i;
	ENTER_FUNCTION(LVL_RW, "Ctx=%p", Ctx);
	ASSERT(Ctx->Map[LBA >> 3] != NOT_MAPPED);
	ASSERT((LBA & -8) == ((LBA + LEN - 1) & -8));
	for(i = 0; i < LEN; i++) {
		ASSERT(((LBA + i) >> 3) < Ctx->SIZE_OF_MAP);
		Ctx->CacheLBA[pos + i] = LBA + i;
	}
	Ctx->CacheReq->Completion = CacheFillReadCompletion;
	ScsiCallRW_AssociatedMem(Ctx->CacheReq, SCSIOP_READ, pos << 9, Ctx->Map[LBA >> 3] + (LBA & 7), LEN,
	                         DEFAULT_RETRY);
	LEAVE_FUNCTION(LVL_RW, "");
}

VOID CacheFillReadCompletion(PREQUEST Req) {
	PDEVICE_OBJECT DeviceObject = Req->DeviceObject;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	KIRQL OldIrql;
	ENTER_FUNCTION(LVL_RW, "Ctx=%p", Ctx);
	ASSERT(Ctx->CacheReq == Req);
	if(!REQ_SUCCESS(Req)) {
		ChangeModeAtomic(DeviceObject, VLTL_MODE_ACTIVE, VLTL_MODE_ERROR);
		goto exit;
	}
	KeAcquireSpinLock(&Ctx->SpinLock, &OldIrql);
	Ctx->CachePos += Req->Transferred;
	KeReleaseSpinLock(&Ctx->SpinLock, OldIrql);
exit:
	WriteCompleteHandler(Ctx);
	LEAVE_FUNCTION(LVL_RW, "");
}

VOID UpdateMap(PLEVELING_CTX Ctx, PULONG LBA, ULONG Sectors, ULONG PLBA) {
	ULONG i, j,  OldPLBA;
	ULONG ChunkNo = PLBA / SIZE_OF_CHUNK;
	ASSERT(ChunkNo < Ctx->MAX_CHUNKS);
	for(i = 0; i < Sectors; i += 8, PLBA += 8) {
		j = LBA[i >> 3] >> 3;
		if(Ctx->SIZE_OF_MAP <= j) {
			continue;
		}
		OldPLBA = Ctx->Map[j];
		if(OldPLBA != NOT_MAPPED) {
			ULONG OldChunkNo = OldPLBA / SIZE_OF_CHUNK;
			ASSERT(OldChunkNo < Ctx->MAX_CHUNKS);
			if(Ctx->ChunkInfo[OldChunkNo].SCN > Ctx->ChunkInfo[ChunkNo].SCN) {
				continue;
			}
			ASSERT(0 != Ctx->ChunkInfo[OldChunkNo].Used);
			Ctx->ChunkInfo[OldChunkNo].Used--;
		}
		Ctx->Map[j] = PLBA;
		Ctx->ChunkInfo[ChunkNo].Used++;
	}
}

VOID ExecutePurgeCache(PLEVELING_CTX Ctx) {
	PPURGE_CTL PurgeCtl = (PVOID)(Ctx->CacheMem + (LAST_CACHE_POS << 9));
	ULONG LBA, i, j;
	ENTER_FUNCTION(LVL_CACHE, "Ctx=%p", Ctx);
	ASSERT(Ctx->CachePos <= LAST_CACHE_POS);
	ASSERT(Ctx->PurgePos + SIZE_OF_CACHE <= LAST_PURGE_POS);
	if(Ctx->TimerActive) {
		if(KeCancelTimer(&Ctx->Timer)) {
			Ctx->TimerActive = FALSE;
		}
	}
	PurgeCtl->Version = LEVELING_VERSION;
	PurgeCtl->CCN = Ctx->CCN;
	PurgeCtl->SCN = Ctx->ChunkInfo[Ctx->ChunkNo].SCN;
	PurgeCtl->ChunkNo = Ctx->ChunkNo;
	PurgeCtl->PurgePos = Ctx->PurgePos;
	PurgeCtl->Magic = Ctx->Magic;
	for(i = 0; i < SIZE_OF_CACHE; i += 8) {
		if(i >= Ctx->CachePos) {
			Ctx->ChunkCtl->LBA[(Ctx->PurgePos + i) >> 3] = NOT_MAPPED;
			PurgeCtl->LBA[i >> 3] = NOT_MAPPED;
			continue;
		}
		LBA = Ctx->CacheLBA[i];
		for(j = 0; j < 8; j++) {
			ASSERT(Ctx->CacheLBA[i + j] == LBA + j);
		}
		Ctx->ChunkCtl->LBA[(Ctx->PurgePos + i) >> 3] = LBA;
		PurgeCtl->LBA[i >> 3] = LBA;
	}
	ProduceChecksum128(PurgeCtl, sizeof(PURGE_CTL));
	Ctx->CacheReq->Completion = PurgeCacheCompletion;
	ScsiCallRW_AssociatedMem(Ctx->CacheReq, SCSIOP_WRITE, 0,
	                         Ctx->ChunkNo * SIZE_OF_CHUNK + Ctx->PurgePos, SIZE_OF_CACHE, DEFAULT_RETRY);
	LEAVE_FUNCTION(LVL_CACHE, "");
}

VOID PurgeCacheCompletion(PREQUEST Req) {
	PDEVICE_OBJECT DeviceObject = Req->DeviceObject;
	PTHREAD Thread = Req->Context;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	PPURGE_CTL PurgeCtl = (PVOID)(Ctx->CacheMem + (LAST_CACHE_POS << 9));
	KIRQL OldIrql;
	ENTER_FUNCTION(LVL_CACHE, "Ctx=%p", Ctx);
	TRACE(LVL_CACHE, "Ctx=%p PurgePos=%x", Ctx, Ctx->PurgePos);
	ASSERT(Ctx->CacheReq == Req);
	ASSERT(Ctx->OtherThread == Thread);
	if(!REQ_SUCCESS(Req)) {
		ChangeModeAtomic(DeviceObject, VLTL_MODE_ACTIVE, VLTL_MODE_ERROR);
		goto exit;
	}
	ASSERT(Req->Transferred == SIZE_OF_CACHE);
	KeAcquireSpinLock(&Ctx->SpinLock, &OldIrql);
	UpdateMap(Ctx, PurgeCtl->LBA, Ctx->CachePos, Ctx->ChunkNo * SIZE_OF_CHUNK + Ctx->PurgePos);
	Ctx->CachePos = 0;
	Ctx->PurgePos += SIZE_OF_CACHE;
	Ctx->RelocationDone = FALSE;
	Ctx->CacheDirty = FALSE;
	KeReleaseSpinLock(&Ctx->SpinLock, OldIrql);
exit:
	WriteCompleteHandler(Ctx);
	LEAVE_FUNCTION(LVL_CACHE, "");
}

VOID ExecutePurgeChunk(PLEVELING_CTX Ctx) {
	PCHUNK_CTL ChunkCtl = Ctx->ChunkCtl;
	ENTER_FUNCTION(LVL_CHUNK, "Ctx=%p", Ctx);
	TRACE(LVL_CHUNK, "Ctx=%p", Ctx);
	ASSERT(Ctx->CachePos == 0);
	ChunkCtl->Version = LEVELING_VERSION;
	ChunkCtl->CCN = Ctx->CCN;
	ChunkCtl->SCN = Ctx->ChunkInfo[Ctx->ChunkNo].SCN;
	ChunkCtl->WCN = ++ Ctx->ChunkInfo[Ctx->ChunkNo].WCN;
	ChunkCtl->NextChunkNo = Ctx->NextChunkNo;
	ChunkCtl->NextChunkWCN = Ctx->ChunkInfo[Ctx->NextChunkNo].WCN;
	ChunkCtl->Start = (0 == Ctx->ChunkNo) ? SIZE_OF_CACHE : 0;
	ChunkCtl->End = Ctx->PurgePos;
	ProduceChecksum128(ChunkCtl, sizeof(CHUNK_CTL));
	RtlCopyMemory(Ctx->CacheMem, ChunkCtl, sizeof(CHUNK_CTL));
	Ctx->CacheReq->Completion = PurgeChunkCompletion;
	ScsiCallRW_AssociatedMem(Ctx->CacheReq, SCSIOP_WRITE, 0,
	                         Ctx->ChunkNo * SIZE_OF_CHUNK + LAST_PURGE_POS, SIZE_OF_CACHE, DEFAULT_RETRY);
	LEAVE_FUNCTION(LVL_CHUNK, "");
}

VOID PurgeChunkCompletion(PREQUEST Req) {
	PDEVICE_OBJECT DeviceObject = Req->DeviceObject;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	ENTER_FUNCTION(LVL_CHUNK, "Ctx=%p", Ctx);
	TRACE(LVL_CHUNK, "Ctx=%p", Ctx);
	ASSERT(Ctx->CacheReq == Req);
	if(!REQ_SUCCESS(Req)) {
		ChangeModeAtomic(DeviceObject, VLTL_MODE_ACTIVE, VLTL_MODE_ERROR);
		goto exit;
	}
	ASSERT(Req->Transferred == SIZE_OF_CACHE);
	ASSERT(Ctx->FinalizedPos < Ctx->PurgePos);
	Ctx->FinalizedPos = Ctx->PurgePos;
exit:
	WriteCompleteHandler(Ctx);
	LEAVE_FUNCTION(LVL_CHUNK, "");
}

VOID FindNextChunk(PLEVELING_CTX Ctx) {
	ULONG i, next, wcn, min_score;
	ENTER_FUNCTION(LVL_CHUNK, "Ctx=%p", Ctx);
	next = Ctx->ChunkNo;
	wcn = Ctx->ChunkInfo[Ctx->ChunkNo].WCN;
	for(i = 0; i < Ctx->MAX_CHUNKS; i++) {
		ULONG s, score_speed, score_life;
		if(i == Ctx->ChunkNo) {
			continue;
		}
		if(Ctx->ChunkInfo[i].Used * 100 >= ((LAST_PURGE_POS - Ctx->PurgePos) >> 3)
		    *RELOCATION_LIMIT_PERCENTAGE) {
			continue;
		}
		score_speed = Ctx->ChunkInfo[i].Used;
		if(Ctx->ChunkInfo[i].WCN < wcn) {
			score_life = 2 * Ctx->ChunkInfo[i].WCN;
		} else {
			score_life = wcn + Ctx->ChunkInfo[i].WCN;
		}
		s = (score_speed * (1 + Ctx->WEIGHT_SPEED)) / 32 + (score_life * (1 + Ctx->WEIGHT_LIFE));
		if(Ctx->ChunkNo == next || min_score > s) {
			min_score = s;
			next = i;
		}
	}
	if(Ctx->ChunkNo == next) {
		for(i = 0; i < Ctx->MAX_CHUNKS; i++) {
			ULONG s = Ctx->ChunkInfo[i].Used;
			if(i == Ctx->ChunkNo) {
				continue;
			}
			if(Ctx->ChunkNo == next || min_score > s) {
				min_score = s;
				next = i;
			}
		}
	}
	TRACE(LVL_CHUNK, "#%d NextChunkNo=%d score=%d used=%d wear=%d",
	      Ctx->ChunkNo, next, min_score, Ctx->ChunkInfo[next].Used, Ctx->ChunkInfo[next].WCN);
	Ctx->NextChunkNo = next;
	ASSERT(Ctx->NextChunkNo != Ctx->ChunkNo);
	Ctx->RelocationDone = FALSE;
	Ctx->RelocationPrepared = FALSE;
	LEAVE_FUNCTION(LVL_CHUNK, "");
}
