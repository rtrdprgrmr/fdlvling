#include "headers.h"

VOID PrepareRelocateCompletion(PREQUEST Req) ;

VOID PrepareRelocate(PLEVELING_CTX Ctx) {
	ENTER_FUNCTION(LVL_CHUNK, "Ctx=%p", Ctx);
	TRACE(LVL_CHUNK, "Ctx=%p ChunkNo=%d PurgePos=0x%x", Ctx, Ctx->ChunkNo, Ctx->PurgePos);
	Ctx->CacheReq->Completion = PrepareRelocateCompletion;
	ScsiCallRW_AssociatedMem(Ctx->CacheReq, SCSIOP_READ, 0,
	                         Ctx->NextChunkNo * SIZE_OF_CHUNK + LAST_PURGE_POS, SIZE_OF_CTL, DEFAULT_RETRY);
	LEAVE_FUNCTION(LVL_CHUNK, "");
}

VOID PrepareRelocateCompletion(PREQUEST Req) {
	PDEVICE_OBJECT DeviceObject = Req->DeviceObject;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	ULONG i, Used;
	ENTER_FUNCTION(LVL_CHUNK, "Ctx=%p", Ctx);
	TRACE(LVL_CHUNK, "Ctx=%p", Ctx);
	ASSERT(Ctx->CacheReq == Req);
	if(!REQ_SUCCESS(Req)) {
		ChangeModeAtomic(DeviceObject, VLTL_MODE_ACTIVE, VLTL_MODE_ERROR);
		WriteCompleteHandler(Ctx);
		goto exit;
	}
	ASSERT(Req->Transferred == SIZE_OF_CTL);
	RtlCopyMemory(Ctx->NextChunkCtl, Ctx->CacheMem, sizeof(CHUNK_CTL));
#if DBG
	Used = 0;
	for(i = 0; i < LAST_PURGE_POS; i += 8) {
		ULONG LBA = Ctx->NextChunkCtl->LBA[i >> 3];
		ULONG PLBA = Ctx->NextChunkNo * SIZE_OF_CHUNK + i;
		if(Ctx->SIZE_OF_MAP <= (LBA >> 3)) {
			continue;
		}
		if(Ctx->Map[LBA >> 3] == PLBA) {
			Used++;
		}
	}
	ASSERT(Used == Ctx->ChunkInfo[Ctx->NextChunkNo].Used);
#endif
	Used = Ctx->ChunkInfo[Ctx->NextChunkNo].Used;
	TRACE(LVL_CHUNK, "Ctx=%p NextChunkNo=%d Used=%d", Ctx, Ctx->NextChunkNo, Used);
	Ctx->RelocateTarget = (LAST_PURGE_POS - Ctx->PurgePos) >> 3;
	Ctx->RelocateSource = Used;
	Ctx->RelocateWindowStart = Ctx->RelocateWindowEnd = Ctx->RelocatePos = 0;
	Ctx->RelocationPrepared = TRUE;
	WaitMarkedThreadsAndWriteCompleteHandler(Ctx);
exit:
	LEAVE_FUNCTION(LVL_CHUNK, "");
}

VOID ExecuteRelocateCached(PLEVELING_CTX Ctx) {
	PREQUEST Req = Ctx->RelocateReq;
	ENTER_FUNCTION(LVL_RELOCATION, "Ctx=%p", Ctx);
	ASSERT(Ctx->RelocationPrepared);
	ASSERT((Ctx->CachePos & 7) == 0);
	ASSERT((Ctx->RelocatePos & 7) == 0);
	ASSERT((Ctx->RelocateWindowStart & 7) == 0);
	ASSERT((Ctx->RelocateWindowEnd & 7) == 0);
	ASSERT(Ctx->ChunkInfo[Ctx->NextChunkNo].Used >= (Ctx->CachePos >> 3));
	ASSERT(Ctx->PurgePos <= (LAST_PURGE_POS - SIZE_OF_CACHE));
	if(Ctx->RelocateSource * ((LAST_PURGE_POS - SIZE_OF_CACHE - Ctx->PurgePos) >> 3)
	    >= Ctx->RelocateTarget * (Ctx->ChunkInfo[Ctx->NextChunkNo].Used - (Ctx->CachePos >> 3))) {
		goto done;
	}
	for(; Ctx->RelocatePos < LAST_PURGE_POS; Ctx->RelocatePos += 8) {
		ULONG i;
		ULONG LBA = Ctx->NextChunkCtl->LBA[Ctx->RelocatePos >> 3];
		ULONG PLBA = Ctx->NextChunkNo * SIZE_OF_CHUNK + Ctx->RelocatePos;
		KIRQL OldIrql;
		if(Ctx->SIZE_OF_MAP <= (LBA >> 3)) {
			continue;
		}
		if(Ctx->Map[LBA >> 3] != PLBA) {
			continue;
		}
		if(Ctx->CachePos >= LAST_CACHE_POS) {
			goto done;
		}
		if(!(Ctx->RelocateWindowStart <= Ctx->RelocatePos && Ctx->RelocatePos < Ctx->RelocateWindowEnd)) {
			goto exit;
		}
		for(i = 0; i < 8; i++) {
			Ctx->CacheLBA[Ctx->CachePos + i] = LBA + i;
		}
		RtlCopyMemory(Ctx->CacheMem + Ctx->CachePos * 512,
		              Ctx->RelocateMem + (Ctx->RelocatePos - Ctx->RelocateWindowStart) * 512, 4096);
		KeAcquireSpinLock(&Ctx->SpinLock, &OldIrql);
		Ctx->CachePos += 8;
		KeReleaseSpinLock(&Ctx->SpinLock, OldIrql);
	}
done:
	Ctx->RelocationDone = TRUE;
exit:
	LEAVE_FUNCTION(LVL_RELOCATION, "%p", Ctx);
}

VOID ExecuteRelocate(PLEVELING_CTX Ctx) {
	PREQUEST Req = Ctx->RelocateReq;
	ULONG i, l;
	ENTER_FUNCTION(LVL_RELOCATION, "Ctx=%p", Ctx);
	ASSERT(Ctx->RelocationPrepared);
	ASSERT((Ctx->RelocatePos & 7) == 0);
	ASSERT(!(Ctx->RelocateWindowStart <= Ctx->RelocatePos
	         && Ctx->RelocatePos < Ctx->RelocateWindowEnd));
	Ctx->RelocateWindowStart = Ctx->RelocateWindowEnd = Ctx->RelocatePos;
	for(i = Ctx->RelocatePos; i < LAST_PURGE_POS && i < Ctx->RelocateWindowStart + SIZE_OF_CACHE
	    && i < Ctx->RelocateWindowEnd + RELOCATION_READ_MAX_GAP; i += 8) {
		ULONG LBA = Ctx->NextChunkCtl->LBA[i >> 3];
		ULONG PLBA = Ctx->NextChunkNo * SIZE_OF_CHUNK + i;
		if(Ctx->SIZE_OF_MAP <= (LBA >> 3)) {
			continue;
		}
		ASSERT((LBA >> 3) < Ctx->SIZE_OF_MAP);
		if(Ctx->Map[LBA >> 3] == PLBA) {
			Ctx->RelocateWindowEnd = i + 8;
		}
	}
	TRACE(LVL_RELOCATION, "RelocateWindow [%x,%x]", Ctx->RelocateWindowStart, Ctx->RelocateWindowEnd);
	ASSERT(Ctx->RelocateWindowEnd - Ctx->RelocateWindowStart <= SIZE_OF_CACHE);
	ASSERT(Ctx->RelocateWindowEnd <= LAST_PURGE_POS);
	ScsiCallRW_AssociatedMem(Req, SCSIOP_READ, 0,
	                         Ctx->NextChunkNo * SIZE_OF_CHUNK + Ctx->RelocateWindowStart,
	                         Ctx->RelocateWindowEnd - Ctx->RelocateWindowStart, DEFAULT_RETRY);
	LEAVE_FUNCTION(LVL_RELOCATION, "%p", Ctx);
}

VOID RelocateCompletion(PREQUEST Req) {
	PDEVICE_OBJECT DeviceObject = Req->DeviceObject;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	ENTER_FUNCTION(LVL_RELOCATION, "Ctx=%p", Ctx);
	ASSERT(Ctx->RelocateReq == Req);
	if(!REQ_SUCCESS(Req)) {
		ChangeModeAtomic(DeviceObject, VLTL_MODE_ACTIVE, VLTL_MODE_ERROR);
	}
	WriteCompleteHandler(Ctx);
	LEAVE_FUNCTION(LVL_RELOCATION, "%p", Ctx);
}
