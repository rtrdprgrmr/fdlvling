#include "headers.h"

PKTHREAD RecoveryThread;
KEVENT RecoveryEvent;
LIST_ENTRY RecoveryQueue;
BOOLEAN DriverStopped;

KSTART_ROUTINE RecoveryWorkerRoutine;
KSTART_ROUTINE DoRecovery;
VOID DoRecovery1(PDEVICE_EXTENSION DeviceExtension);

VOID StartRecoveryWorkerThread() {
	HANDLE ThreadHandle;
	NTSTATUS status;
	KeInitializeEvent(&RecoveryEvent, SynchronizationEvent, FALSE);
	InitializeListHead(&RecoveryQueue);
	status = PsCreateSystemThread(&ThreadHandle, THREAD_ALL_ACCESS,
	                              NULL, NULL, NULL, RecoveryWorkerRoutine, NULL);
	if(!NT_SUCCESS(status)) {
		TRACE(LVL_ERROR, "PsCreateSystemThread()=%s", NtStatusName(status));
		return;
	}
	ObReferenceObjectByHandle(ThreadHandle, THREAD_ALL_ACCESS, NULL, KernelMode,
	                          (PVOID*)&RecoveryThread, NULL);
	ZwClose(ThreadHandle);
}

VOID StopRecoveryWorkerThread() {
	KIRQL OldIrql;
	ENTER_FUNCTION(LVL_SYSTEM, "");
	DriverStopped = TRUE;
	KeSetEvent(&RecoveryEvent, IO_NO_INCREMENT, FALSE);
	if(NULL != RecoveryThread) {
		KeWaitForSingleObject(RecoveryThread, Executive, KernelMode, FALSE, NULL);
		ObDereferenceObject(RecoveryThread);
		RecoveryThread = NULL;
	}
	LEAVE_FUNCTION(LVL_SYSTEM, "");
}

VOID RecoveryWorkerRoutine(PVOID Context) {
	PDEVICE_EXTENSION DeviceExtension;
	PLIST_ENTRY Entry;
	KIRQL OldIrql;
	ENTER_FUNCTION(LVL_SYSTEM, "");
	while(TRUE) {
		if(DriverStopped) {
			break;
		}
		DeviceExtension = NULL;
		KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
		if(!IsListEmpty(&RecoveryQueue)) {
			Entry = RemoveHeadList(&RecoveryQueue);
			DeviceExtension = CONTAINING_RECORD(Entry, DEVICE_EXTENSION, ListEntry);
		}
		KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
		if(NULL == DeviceExtension) {
			KeWaitForSingleObject(&RecoveryEvent, Executive, KernelMode, FALSE, NULL);
		}
		else{
			HANDLE ThreadHandle;
			NTSTATUS status;
			status = PsCreateSystemThread(&ThreadHandle, THREAD_ALL_ACCESS,
						      NULL, NULL, NULL, DoRecovery, DeviceExtension);
			if(!NT_SUCCESS(status)) {
				TRACE(LVL_ERROR, "PsCreateSystemThread()=%s", NtStatusName(status));
				DoRecovery1(DeviceExtension);
				continue;
			}
			ZwClose(ThreadHandle);
		}
	}
	LEAVE_FUNCTION(LVL_SYSTEM, "");
	PsTerminateSystemThread(0);
}

VOID DoRecovery(PVOID Context) {
	DoRecovery1(Context);
	PsTerminateSystemThread(0);
}

VOID DoRecovery1(PDEVICE_EXTENSION DeviceExtension) {
	PDEVICE_OBJECT DeviceObject = DeviceExtension->DeviceObject;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	PREQUEST Req = Ctx->CacheReq;
	PCHUNK_CTL ChunkCtl = (PVOID)Req->AssociatedMem;
	ULONG ChunkNo;
	ULONG i;
	ULONG MaxSCN = 0;
	ENTER_FUNCTION(LVL_CHUNK, "");
	Ctx->ChunkNo = 1;
	for(ChunkNo = 0; ChunkNo < Ctx->MAX_CHUNKS; ChunkNo++) {
		SyncScsiCallRW_AssociatedMem(Req, SCSIOP_READ, 0, ChunkNo * SIZE_OF_CHUNK + LAST_PURGE_POS,
		                             SIZE_OF_CTL, DEFAULT_RETRY);
		if(!REQ_SUCCESS(Req)) {
			ChangeModeAtomic(DeviceObject, VLTL_MODE_RECOVERY, VLTL_MODE_ERROR);
			return;
		}
		if(!VerifyChecksum128(ChunkCtl, sizeof(CHUNK_CTL)) || ChunkCtl->Version != Ctx->Version) {
			continue;
		}
		if(Ctx->ChunkInfo[ChunkNo].WCN < ChunkCtl->WCN) {
			Ctx->ChunkInfo[ChunkNo].WCN = ChunkCtl->WCN;
		}
		if(ChunkCtl->CCN != Ctx->CCN) {
			continue;
		}
		if(!((ChunkCtl->Start & (SIZE_OF_CACHE - 1)) == 0
		     && (ChunkCtl->End & (SIZE_OF_CACHE - 1)) == 0
		     && ChunkCtl->Start <= ChunkCtl->End
		     && ChunkCtl->End <= LAST_PURGE_POS
		     && ChunkCtl->NextChunkNo < Ctx->MAX_CHUNKS)) {
			TRACE(LVL_ERROR, "Invalid ChunkCtl");
			KdBreakPoint();
			continue;
		}
		TRACE(LVL_RECOVERY, "#%d SCN=%d WCN=%d Start=0x%x End=0x%x Next=%d NextWCN=%d"
		      , ChunkNo , ChunkCtl->SCN, ChunkCtl->WCN , ChunkCtl->Start, ChunkCtl->End
		      , ChunkCtl->NextChunkNo, ChunkCtl->NextChunkWCN);
		Ctx->ChunkInfo[ChunkNo].SCN = ChunkCtl->SCN;
		UpdateMap(Ctx, ChunkCtl->LBA + (ChunkCtl->Start >> 3), ChunkCtl->End - ChunkCtl->Start,
		          ChunkNo * SIZE_OF_CHUNK + ChunkCtl->Start);
		if(MaxSCN <= ChunkCtl->SCN) {
			MaxSCN = ChunkCtl->SCN;
			Ctx->ChunkNo = ChunkNo;
			RtlCopyMemory(Ctx->ChunkCtl, Ctx->CacheMem, sizeof(CHUNK_CTL));
		}
	}
	ChunkCtl = Ctx->ChunkCtl;
	Ctx->CachePos = 0;
	Ctx->PurgePos = ChunkCtl->End;
	Ctx->FinalizedPos = Ctx->PurgePos;
	if(Ctx->PurgePos == LAST_PURGE_POS) {
		ULONG NextChunkNo = ChunkCtl->NextChunkNo;
		ASSERT(NextChunkNo < Ctx->MAX_CHUNKS);
		if(Ctx->ChunkInfo[NextChunkNo].Used > 0) {
			TRACE(LVL_ERROR, "Broken relocation consistency");
			KdBreakPoint();
		}
		if(Ctx->ChunkInfo[NextChunkNo].WCN < ChunkCtl->NextChunkWCN) {
			Ctx->ChunkInfo[NextChunkNo].WCN = ChunkCtl->NextChunkWCN;
		}
		Ctx->ChunkInfo[NextChunkNo].SCN = Ctx->ChunkInfo[Ctx->ChunkNo].SCN + 1;
		Ctx->ChunkNo = NextChunkNo;
		Ctx->PurgePos = (0 == Ctx->ChunkNo) ? SIZE_OF_CACHE : 0;
		Ctx->FinalizedPos = Ctx->PurgePos;
	}
	while(Ctx->PurgePos < LAST_PURGE_POS) {
		PPURGE_CTL PurgeCtl = (PVOID)Ctx->CacheMem;
		SyncScsiCallRW_AssociatedMem(Req, SCSIOP_READ, 0,
		                             Ctx->ChunkNo * SIZE_OF_CHUNK + Ctx->PurgePos + LAST_CACHE_POS, 1, DEFAULT_RETRY);
		if(!REQ_SUCCESS(Req)) {
			ChangeModeAtomic(DeviceObject, VLTL_MODE_RECOVERY, VLTL_MODE_ERROR);
			return;
		}
		if(!VerifyChecksum128(PurgeCtl, sizeof(PURGE_CTL)) || PurgeCtl->Version != Ctx->Version) {
			break;
		}
		if(PurgeCtl->CCN != Ctx->CCN || PurgeCtl->Magic != Ctx->Magic) {
			break;
		}
		if(PurgeCtl->SCN != Ctx->ChunkInfo[Ctx->ChunkNo].SCN) {
			break;
		}
		if(!(PurgeCtl->ChunkNo == Ctx->ChunkNo
		     && PurgeCtl->PurgePos == Ctx->PurgePos)) {
			TRACE(LVL_ERROR, "Invalid PurgeCtl");
			KdBreakPoint();
			break;
		}
		for(i = 0; i < SIZE_OF_CACHE; i += 8) {
			ULONG LBA = PurgeCtl->LBA[i >> 3];
			if((LBA >> 3) >= Ctx->SIZE_OF_MAP) {
				LBA = NOT_MAPPED;
			}
			Ctx->ChunkCtl->LBA[(Ctx->PurgePos + i) >> 3] = LBA;
		}
		UpdateMap(Ctx, PurgeCtl->LBA, SIZE_OF_CACHE, Ctx->ChunkNo * SIZE_OF_CHUNK + Ctx->PurgePos);
		Ctx->PurgePos += SIZE_OF_CACHE;
	}
	for(ChunkNo = 0; ChunkNo < Ctx->MAX_CHUNKS; ChunkNo++) {
		if(Ctx->ChunkInfo[ChunkNo].Used) {
			TRACE(LVL_RECOVERY, "ChunkNo=%d Used=%d Wear=%d SCN=%d", ChunkNo, Ctx->ChunkInfo[ChunkNo].Used,
			      Ctx->ChunkInfo[ChunkNo].WCN, Ctx->ChunkInfo[ChunkNo].SCN);
		}
	}
	TRACE(LVL_RECOVERY, "Recovered ChunkNo=%d PurgePos=0x%x", Ctx->ChunkNo, Ctx->PurgePos);
	FindNextChunk(Ctx);
	ChangeModeAtomic(DeviceObject, VLTL_MODE_RECOVERY, VLTL_MODE_ACTIVE);
	LEAVE_FUNCTION(LVL_CHUNK, "");
}
