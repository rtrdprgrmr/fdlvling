#include "headers.h"

NTSTATUS ExecuteThread(PDEVICE_OBJECT DeviceObject, PTHREAD Thread, PIRP Irp);
VOID LevelingCleanAfterModeChanged(PDEVICE_OBJECT DeviceObject);
NTSTATUS ExecutePass(PDEVICE_OBJECT DeviceObject, PTHREAD Thread);
IO_COMPLETION_ROUTINE ExecutePassCompletion;
NTSTATUS ExecuteReadCapacity(PDEVICE_OBJECT DeviceObject, PTHREAD Thread);
NTSTATUS ExecuteModeSense(PDEVICE_OBJECT DeviceObject, PTHREAD Thread);

VOID FreeLevelingCtx1(PLEVELING_CTX Ctx) {
	int i;
	ENTER_FUNCTION(LVL_CTX, "");
	if(NULL != Ctx) {
		TRACE(LVL_CTX, "Remove Ctx=%p", Ctx);
		for(i = 0; i < READ_THREADS; i++) {
			PTHREAD Thread = &Ctx->ReadThread[i];
			FreeRequest(Thread->Req);
		}
		FreeRequest(Ctx->CacheReq);
		FreeRequest(Ctx->RelocateReq);
		if(NULL != Ctx->Map) {
			ExFreePoolWithTag(Ctx->Map, 'VLTL');
		}
		if(NULL != Ctx->ChunkInfo) {
			ExFreePoolWithTag(Ctx->ChunkInfo, 'VLTL');
		}
		ExFreePoolWithTag(Ctx, 'VLTL');
	}
	LEAVE_FUNCTION(LVL_CTX, "");
}

BOOLEAN InitializeLevelingCtx(PDEVICE_OBJECT DeviceObject, PUCHAR MBR) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CONF Conf = (PVOID)MBR;
	PLEVELING_CTX Ctx = NULL;
	BOOLEAN result = FALSE;
	KIRQL OldIrql;
	ULONG i;
	ENTER_FUNCTION(LVL_CTX, "");
	if(!VerifyChecksum128(Conf, sizeof(LEVELING_CONF)) || Conf->Version != LEVELING_VERSION) {
		goto error;
	}
	if(Conf->SIZE_OF_MAP == 0) {
		goto error;
	}
	Ctx = ExAllocatePoolWithTag(NonPagedPool, sizeof(LEVELING_CTX), 'VLTL');
	if(NULL == Ctx) {
		goto error;
	}
	RtlZeroMemory(Ctx, sizeof(LEVELING_CTX));
	Ctx->DeviceObject = DeviceObject;
	Ctx->Version = Conf->Version;
	Ctx->CCN = Conf->CCN;
	Ctx->SIZE_OF_MAP = Conf->SIZE_OF_MAP;
	Ctx->MAX_CHUNKS = GetMaxChunks(DeviceObject);
	Ctx->WEIGHT_LIFE = Conf->WEIGHT_LIFE;
	Ctx->WEIGHT_SPEED = Conf->WEIGHT_SPEED;
	Ctx->Magic = Conf->Magic;
	Ctx->CacheTimeout = Conf->CacheTimeout;
	Ctx->FlushFaithfully = Conf->FlushFaithfully;
	if(Ctx->SIZE_OF_MAP * 10 > Ctx->MAX_CHUNKS * SIZE_OF_CHUNK) {
		KdBreakPoint();
		ChangeMode(DeviceObject, VLTL_MODE_ERROR);
		goto error;
	}
	KeInitializeSpinLock(&Ctx->SpinLock);
	KeInitializeTimer(&Ctx->Timer);
	KeInitializeDpc(&Ctx->TimerDpc, LevelingFlushDpc, DeviceObject);
	InitializeListHead(&Ctx->ReadIrpQueue);
	InitializeListHead(&Ctx->OtherIrpQueue);
	for(i = 0; i < READ_THREADS; i++) {
		PTHREAD Thread = &Ctx->ReadThread[i];
		Thread->Req = CreateRequest(DeviceObject, ExecuteReadCompletion, Thread, 0);
		Thread->IsRead = TRUE;
		Thread->Index = (UCHAR)i;
		Thread->FreeLink = (UCHAR)(i + 1);
	}
	Ctx->CacheReq = CreateRequest(DeviceObject, NULL, Ctx->OtherThread, SIZE_OF_CACHE << 9);
	if(NULL == Ctx->CacheReq) {
		goto error;
	}
	Ctx->CacheMem = Ctx->CacheReq->AssociatedMem;
	Ctx->RelocateReq = CreateRequest(DeviceObject, RelocateCompletion, Ctx->OtherThread,
	                                 SIZE_OF_CACHE << 9);
	if(NULL == Ctx->RelocateReq) {
		goto error;
	}
	Ctx->RelocateMem = Ctx->RelocateReq->AssociatedMem;
	Ctx->Map = ExAllocatePoolWithTag(NonPagedPool, sizeof(ULONG) * Ctx->SIZE_OF_MAP, 'VLTL');
	if(NULL == Ctx->Map) {
		goto error;
	}
	for(i = 0; i < Ctx->SIZE_OF_MAP; i++) {
		Ctx->Map[i] = NOT_MAPPED;
	}
	Ctx->ChunkInfo = ExAllocatePoolWithTag(NonPagedPool, sizeof(CHUNK_INFO) * Ctx->MAX_CHUNKS, 'VLTL');
	if(NULL == Ctx->ChunkInfo) {
		goto error;
	}
	RtlZeroMemory(Ctx->ChunkInfo, sizeof(CHUNK_INFO)*Ctx->MAX_CHUNKS);
	KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
	if(NULL == DeviceExtension->Ctx && DeviceExtension->Mode == VLTL_MODE_INITIAL) {
		TRACE(LVL_CTX, "New Ctx=%p", Ctx);
		DeviceExtension->Ctx = Ctx;
		ChangeMode(DeviceObject, VLTL_MODE_RECOVERY);
		InsertTailList(&RecoveryQueue, &DeviceExtension->ListEntry);
		KeSetEvent(&RecoveryEvent, IO_NO_INCREMENT, FALSE);
		result = TRUE;
	}
	KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
error:
	if(!result) {
		FreeLevelingCtx1(Ctx);
	}
	LEAVE_FUNCTION(LVL_CTX, "%d", result);
	return result;
}

VOID FreeLevelingCtx(PDEVICE_OBJECT DeviceObject, BOOLEAN Wait) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx;
	BOOLEAN StillActive = FALSE;
	KIRQL OldIrql;
	ENTER_FUNCTION(LVL_CTX, "");
loop:
	KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
	Ctx = DeviceExtension->Ctx;
	if(NULL != Ctx) {
		if(Ctx->ReadPending > 0 || Ctx->OtherPending > 0) {
			StillActive = TRUE;
		} else if(Ctx->TimerActive) {
			if(!KeCancelTimer(&Ctx->Timer)) {
				StillActive = TRUE;
			}
		}
		if(!StillActive) {
			DeviceExtension->Ctx = NULL;
		}
	}
	if(DeviceExtension->Ctx == NULL) {
		ChangeMode(DeviceObject, VLTL_MODE_INITIAL);
	}
	KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
	if(Wait && StillActive) {
		LARGE_INTEGER Timeout;
		Timeout.QuadPart = -100000L;
		KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
		goto loop;
	}
	if(!StillActive) {
		FreeLevelingCtx1(Ctx);
	}
	LEAVE_FUNCTION(LVL_CTX, "");
}

NTSTATUS CompleteNotReadyNoMedia(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	NTSTATUS status;
	if(IRP_MJ_SCSI == IrpStack->MajorFunction) {
		PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
		Srb->SrbStatus = SRB_STATUS_ERROR;
		if(SRB_FUNCTION_EXECUTE_SCSI == Srb->Function) {
			if(VLTL_MODE_RECOVERY == DeviceExtension->Mode) {
				SetCheckCondition(Srb, SCSI_SENSE_NOT_READY, SCSI_ADSENSE_LUN_NOT_READY, 0x05);
			} else {
				SetCheckCondition(Srb, SCSI_SENSE_NOT_READY, SCSI_ADSENSE_NO_MEDIA_IN_DEVICE, 0x00);
			}
		}
		Irp->IoStatus.Information = Srb->DataTransferLength = 0;
		Irp->IoStatus.Status = status = STATUS_IO_DEVICE_ERROR;
	} else {
		Irp->IoStatus.Information = 0;
		Irp->IoStatus.Status = status = STATUS_UNSUCCESSFUL;
	}
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;
}

NTSTATUS LevelingDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp, BOOLEAN IsRead) {
	NTSTATUS status;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx;
	BOOLEAN IsModeChanged = FALSE;
	BOOLEAN DoNow = FALSE;
	PTHREAD Thread;
	KIRQL OldIrql;
	ENTER_FUNCTION(LVL_GATE, "Irp=%p", Irp);
	KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
	Ctx = DeviceExtension->Ctx;
	if(NULL == Ctx || VLTL_MODE_ACTIVE != DeviceExtension->Mode) {
		IsModeChanged = TRUE;
	} else if(IsRead) {
		Ctx->ReadPending++;
		ASSERT(Ctx->ReadPending > 0);
		if(Ctx->ReadActive < READ_THREADS) {
			UCHAR i = Ctx->ReadFreeList;
			Thread = &Ctx->ReadThread[i];
			ASSERT(i < READ_THREADS);
			ASSERT(i == Thread->Index);
			Ctx->ReadFreeList = Thread->FreeLink;
			Thread->FreeLink = 255;
			Ctx->ReadActive++;
			DoNow = TRUE;
		} else {
			InsertTailList(&Ctx->ReadIrpQueue, &Irp->Tail.Overlay.ListEntry);
		}
	} else {
		Ctx->OtherPending++;
		ASSERT(Ctx->OtherPending > 0);
		if(Ctx->OtherActive == 0) {
			Ctx->OtherActive++;
			ASSERT(Ctx->OtherActive == 1);
			Thread = Ctx->OtherThread;
			DoNow = TRUE;
		} else {
			InsertTailList(&Ctx->OtherIrpQueue, &Irp->Tail.Overlay.ListEntry);
		}
	}
	KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
	if(IsModeChanged) {
		status = CompleteNotReadyNoMedia(DeviceObject, Irp);
	} else if(DoNow) {
		status = ExecuteThread(DeviceObject, Thread, Irp);
		if(status != STATUS_PENDING) {
			LevelingThreadCompletion(DeviceObject, Thread);
		}
	} else {
		IoMarkIrpPending(Irp);
		status = STATUS_PENDING;
	}
	LEAVE_FUNCTION(LVL_GATE, "%s", NtStatusName(status));
	return status;
}

VOID MarkReadingThreads(PLEVELING_CTX Ctx) {
	ULONG i;
	KIRQL OldIrql;
	KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
	for(i = 0; i < READ_THREADS; i++) {
		PTHREAD Thread = &Ctx->ReadThread[i];
		if(Thread->FreeLink == 255) {
			if(!Thread->Marked) {
				Thread->Marked = TRUE;
				Ctx->ThreadsMarked++;
			}
		}
	}
	KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
}

VOID WaitMarkedThreadsAndWriteCompleteHandler(PLEVELING_CTX Ctx) {
	BOOLEAN waiting;
	KIRQL OldIrql;
	KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
	if(Ctx->ThreadsMarked > 0) {
		ASSERT(!Ctx->WaitingMarkedThreads);
		Ctx->WaitingMarkedThreads = TRUE;
	}
	waiting = Ctx->WaitingMarkedThreads;
	KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
	if(!waiting) {
		WriteCompleteHandler(Ctx);
	}
}

VOID LevelingFlushDpc(struct _KDPC* Dpc, PVOID DeferredContext, PVOID SystemArgument1,
                      PVOID SystemArgument2) {
	PDEVICE_OBJECT DeviceObject = DeferredContext;
	NTSTATUS status;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx;
	BOOLEAN DoFlush = FALSE;
	PTHREAD Thread;
	KIRQL OldIrql;
	ENTER_FUNCTION(LVL_GATE, "");
	KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
	Ctx = DeviceExtension->Ctx;
	ASSERT(Ctx != NULL);
	if(Ctx->OtherActive == 0) {
		if(Ctx->CacheDirty && VLTL_MODE_ACTIVE == DeviceExtension->Mode) {
			Ctx->OtherPending++;
			ASSERT(Ctx->OtherPending > 0);
			Ctx->OtherActive++;
			ASSERT(Ctx->OtherActive == 1);
			Thread = Ctx->OtherThread;
			DoFlush = TRUE;
		}
		Ctx->FlushPending = FALSE;
		Ctx->TimerActive = FALSE;
	} else {
		Ctx->FlushPending = TRUE;
	}
	KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
	if(DoFlush) {
		TRACE(LVL_INFO, "FLUSH(TIMEOUT)");
		status = ExecuteFlush(DeviceObject, Thread, FALSE);
		if(status != STATUS_PENDING) {
			LevelingThreadCompletion(DeviceObject, Thread);
		}
	}
	LEAVE_FUNCTION(LVL_GATE, "");
}

VOID LevelingThreadCompletion(PDEVICE_OBJECT DeviceObject, PTHREAD Thread) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	BOOLEAN IsRead;
	BOOLEAN IsError;
	BOOLEAN DoClean;
	BOOLEAN DoNow;
	BOOLEAN DoWriteComplete;
	BOOLEAN DoFlush;
	PIRP Irp;
	KIRQL OldIrql;
	ENTER_FUNCTION(LVL_GATE, "Thread=%p", Thread);
loop:
	ASSERT(NULL == Thread->Irp);
	IsRead = Thread->IsRead;
	IsError = FALSE;
	DoClean = FALSE;
	DoNow = FALSE;
	DoWriteComplete = FALSE;
	DoFlush = FALSE;
	KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
	if(VLTL_MODE_ACTIVE != DeviceExtension->Mode) {
		IsError = TRUE;
	}
	if(IsRead) {
		ASSERT(Ctx->ReadPending > 0);
		Ctx->ReadPending--;
		if(!IsError && !IsListEmpty(&Ctx->ReadIrpQueue)) {
			PLIST_ENTRY Entry = RemoveHeadList(&Ctx->ReadIrpQueue);
			Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
			DoNow = TRUE;
		} else {
			ASSERT(Thread->FreeLink == 255);
			Thread->FreeLink = Ctx->ReadFreeList;
			Ctx->ReadFreeList = Thread->Index;
			Ctx->ReadActive--;
			ASSERT(Ctx->ReadActive < READ_THREADS);
		}
	} else {
		ASSERT(Ctx->OtherPending > 0);
		Ctx->OtherPending--;
		Ctx->OtherActive--;
		ASSERT(Ctx->OtherActive == 0);
		if(!IsError && !IsListEmpty(&Ctx->OtherIrpQueue)) {
			PLIST_ENTRY Entry = RemoveHeadList(&Ctx->OtherIrpQueue);
			Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
			Ctx->OtherActive++;
			ASSERT(Ctx->OtherActive == 1);
			DoNow = TRUE;
		} else {
			if(Ctx->FlushPending) {
				if(Ctx->CacheDirty && !IsError) {
					Ctx->OtherPending++;
					ASSERT(Ctx->OtherPending > 0);
					Ctx->OtherActive++;
					ASSERT(Ctx->OtherActive == 1);
					DoFlush = TRUE;
				}
				Ctx->FlushPending = FALSE;
				Ctx->TimerActive = FALSE;
			}
			if(!DoFlush) {
				UCHAR CacheTimeout = Ctx->CacheTimeout;
				ASSERT(Ctx->OtherActive == 0);
				if(Ctx->CacheDirty && !Ctx->TimerActive && CacheTimeout > 0) {
					LARGE_INTEGER DueTime;
					DueTime.QuadPart = ((LONGLONG)CacheTimeout) * -10000000L;
					Ctx->TimerActive = TRUE;
					KeSetTimer(&Ctx->Timer, DueTime, &Ctx->TimerDpc);
				}
			}
		}
	}
	if(Thread->Marked) {
		Thread->Marked = FALSE;
		Ctx->ThreadsMarked--;
	}
	if(Ctx->ThreadsMarked == 0 && Ctx->WaitingMarkedThreads) {
		ASSERT(Ctx->OtherActive == 1);
		Ctx->WaitingMarkedThreads = FALSE;
		DoWriteComplete = TRUE;
	}
	if(IsError && Ctx->ReadActive == 0 && Ctx->OtherActive == 0) {
		ASSERT(!DoNow);
		ASSERT(!DoWriteComplete);
		ASSERT(!DoFlush);
		DoClean = TRUE;
	}
	KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
	if(DoClean) {
		LevelingCleanAfterModeChanged(DeviceObject);
	}
	if(DoWriteComplete) {
		WriteCompleteHandler(Ctx);
	}
	if(DoNow) {
		if(STATUS_PENDING != ExecuteThread(DeviceObject, Thread, Irp)) {
			goto loop;
		}
	}
	if(DoFlush) {
		TRACE(LVL_INFO, "FLUSH(TIMEOUT,PENDING)");
		if(STATUS_PENDING != ExecuteFlush(DeviceObject, Thread, FALSE)) {
			goto loop;
		}
	}
	LEAVE_FUNCTION(LVL_GATE, "");
}

VOID LevelingCleanAfterModeChanged(PDEVICE_OBJECT DeviceObject) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx;
	ENTER_FUNCTION(LVL_CTX, "");
	for(;;) {
		PIRP Irp = NULL;
		KIRQL OldIrql;
		KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
		Ctx = DeviceExtension->Ctx;
		if(NULL != Ctx && VLTL_MODE_ACTIVE != DeviceExtension->Mode) {
			if(!IsListEmpty(&Ctx->ReadIrpQueue)) {
				PLIST_ENTRY Entry = RemoveHeadList(&Ctx->ReadIrpQueue);
				Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
				ASSERT(Ctx->ReadPending > 0);
				Ctx->ReadPending--;
			} else if(!IsListEmpty(&Ctx->OtherIrpQueue)) {
				PLIST_ENTRY Entry = RemoveHeadList(&Ctx->OtherIrpQueue);
				Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
				ASSERT(Ctx->OtherPending > 0);
				Ctx->OtherPending--;
			}
		}
		KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
		if(NULL == Irp) {
			break;
		}
		CompleteNotReadyNoMedia(DeviceObject, Irp);
	}
	LEAVE_FUNCTION(LVL_CTX, "");
}

NTSTATUS ExecuteThread(PDEVICE_OBJECT DeviceObject, PTHREAD Thread, PIRP Irp) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	NTSTATUS status;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
	ENTER_FUNCTION(LVL_GATE, "Thread=%p Irp=%p", Thread, Irp);
	ASSERT(NULL == Thread->Irp);
	Thread->Irp = Irp;
	Thread->Mdl = Irp->MdlAddress;
	Thread->DataSystemAddress = NULL;
	if(IRP_MJ_SHUTDOWN == IrpStack->MajorFunction) {
		TRACE(LVL_SYSTEM, "IRP_MJ_SHUTDOWN");
		status = ExecuteFlush(DeviceObject, Thread, TRUE);
		goto exit;
	}
	if(IRP_MJ_PNP == IrpStack->MajorFunction) {
		if(IRP_MN_QUERY_REMOVE_DEVICE == IrpStack->MinorFunction) {
			TRACE(LVL_SYSTEM, "IRP_MN_QUERY_REMOVE_DEVICE");
			if(!Ctx->CacheDirty) {
				status = ExecutePass(DeviceObject, Thread);
				goto exit;
			}
		}
	}
	if(IRP_MJ_SCSI != IrpStack->MajorFunction) {
		Irp->IoStatus.Information = 0;
		Irp->IoStatus.Status = status = STATUS_UNSUCCESSFUL;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		Thread->Irp = NULL;
		goto exit;
	}
	switch(Srb->Function) {
	case SRB_FUNCTION_EXECUTE_SCSI:
		switch(Srb->Cdb[0]) {
		case SCSIOP_VERIFY:
			Srb->SrbStatus = SRB_STATUS_SUCCESS;
			Srb->ScsiStatus = SCSISTAT_GOOD;
			Srb->SenseInfoBufferLength = 0;
			Irp->IoStatus.Information = Srb->DataTransferLength = RtlUshortByteSwap(*(USHORT*)(
			                                Srb->Cdb + 7)) << 9;
			Irp->IoStatus.Status = status = STATUS_SUCCESS;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
			Thread->Irp = NULL;
			goto exit;
		case SCSIOP_TEST_UNIT_READY:
		case SCSIOP_MEDIUM_REMOVAL:
			status = ExecutePass(DeviceObject, Thread);
			goto exit;
		case SCSIOP_READ_CAPACITY:
			status = ExecuteReadCapacity(DeviceObject, Thread);
			goto exit;
		case SCSIOP_READ:
		case SCSIOP_WRITE:
			status = ExecuteRW(DeviceObject, Thread);
			goto exit;
		case SCSIOP_MODE_SENSE:
			status = ExecuteModeSense(DeviceObject, Thread);
			goto exit;
		case SCSIOP_SYNCHRONIZE_CACHE:
			TRACE(LVL_INFO, "SCSIOP_SYNCHRONIZE_CACHE");
			if(Ctx->FlushFaithfully) {
				status = ExecuteFlush(DeviceObject, Thread, FALSE);
			} else {
				status = ExecutePass(DeviceObject, Thread);
			}
			goto exit;
		case SCSIOP_START_STOP_UNIT:
			if((Srb->Cdb[4] & 3) == 2) {
				TRACE(LVL_INFO, "STOP UNIT");
				status = ExecuteFlush(DeviceObject, Thread, TRUE);
			} else {
				status = ExecutePass(DeviceObject, Thread);
			}
			goto exit;
		}
		SetCheckCondition(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0x00);
		Irp->IoStatus.Information = Srb->DataTransferLength = 0;
		Irp->IoStatus.Status = status = STATUS_IO_DEVICE_ERROR;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		Thread->Irp = NULL;
		goto exit;
	case SRB_FUNCTION_SHUTDOWN:
		TRACE(LVL_SYSTEM, "SRB_FUNCTION_SHUTDOWN");
		status = ExecuteFlush(DeviceObject, Thread, TRUE);
		goto exit;
	case SRB_FUNCTION_FLUSH:
		TRACE(LVL_INFO, "SRB_FUNCTION_FLUSH");
		if(Ctx->FlushFaithfully) {
			status = ExecuteFlush(DeviceObject, Thread, FALSE);
		} else {
			status = ExecutePass(DeviceObject, Thread);
		}
		goto exit;
	}
	Srb->SrbStatus = SRB_STATUS_ERROR;
	Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
	Srb->SenseInfoBufferLength = 0;
	Irp->IoStatus.Information = Srb->DataTransferLength = 0;
	Irp->IoStatus.Status = status = STATUS_IO_DEVICE_ERROR;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	Thread->Irp = NULL;
exit:
	LEAVE_FUNCTION(LVL_GATE, "%s", NtStatusName(status));
	return status;
}

NTSTATUS ExecutePass(PDEVICE_OBJECT DeviceObject, PTHREAD Thread) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIRP Irp = Thread->Irp;
	NTSTATUS status = STATUS_SUCCESS;
	ENTER_FUNCTION(LVL_INFO, "Thread=%p", Thread);
	if(Irp != NULL) {
		IoCopyCurrentIrpStackLocationToNext(Irp);
		IoSetCompletionRoutine(Irp, ExecutePassCompletion, Thread, TRUE, TRUE, TRUE);
		status = IoCallDriver(DeviceExtension->NextLowerDriver, Irp);
	}
	LEAVE_FUNCTION(LVL_INFO, "");
	return status;
}

NTSTATUS ExecutePassCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	NTSTATUS status = Irp->IoStatus.Status;
	PTHREAD Thread = Context;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	LPCSTR name = IrpStackName(IrpStack);
	TRACE(LVL_INFO, "Thread=%p %s %s", Thread, name, NtStatusName(status));
	ASSERT(Irp == Thread->Irp);
	Thread->Irp = NULL;
	if(Irp->PendingReturned) {
		IoMarkIrpPending(Irp);
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		LevelingThreadCompletion(DeviceObject, Thread);
		return STATUS_MORE_PROCESSING_REQUIRED;
	}
	return STATUS_CONTINUE_COMPLETION;
}

BOOLEAN GetDataSystemAddress(PTHREAD Thread, ULONG Length, BOOLEAN CheckOnly) {
	PIRP Irp = Thread->Irp;
	PMDL Mdl = Thread->Mdl;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
	PUCHAR SrbDataBuffer = Srb->DataBuffer;
	PUCHAR MdlVirtualAddress;
	if(NULL == Mdl) {
		KdBreakPoint();
		return FALSE;
	}
	if(NULL != Mdl->Next) {
		KdBreakPoint();
		return FALSE;
	}
	switch(Srb->Cdb[0]) {
	case SCSIOP_READ:
	case SCSIOP_READ_CAPACITY:
	case SCSIOP_MODE_SENSE:
		if(0 == (SRB_FLAGS_DATA_IN & Srb->SrbFlags)) {
			KdBreakPoint();
			return FALSE;
		}
		break;
	case SCSIOP_WRITE:
		if(0 == (SRB_FLAGS_DATA_OUT & Srb->SrbFlags)) {
			KdBreakPoint();
			return FALSE;
		}
		break;
	default:
		KdBreakPoint();
		return FALSE;
	}
	if(Srb->DataTransferLength < Length) {
		KdBreakPoint();
		return FALSE;
	}
	MdlVirtualAddress = MmGetMdlVirtualAddress(Mdl);
	if(!(0 <= SrbDataBuffer - MdlVirtualAddress)) {
		KdBreakPoint();
		return FALSE;
	}
	if(!(0 <= (MdlVirtualAddress + MmGetMdlByteCount(Mdl)) - (SrbDataBuffer + Length))) {
		KdBreakPoint();
		return FALSE;
	}
	if(!CheckOnly) {
		PUCHAR MdlSystemAddress = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
		if(NULL == MdlSystemAddress) {
			KdBreakPoint();
			return FALSE;
		}
		Thread->DataSystemAddress = MdlSystemAddress + (SrbDataBuffer - MdlVirtualAddress);
	}
	return TRUE;
}

NTSTATUS ExecuteReadCapacity(PDEVICE_OBJECT DeviceObject, PTHREAD Thread) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx = DeviceExtension->Ctx;
	PIRP Irp = Thread->Irp;
	NTSTATUS status;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
	ENTER_FUNCTION(LVL_INFO, "Thread=%p", Thread);
	if((Srb->Cdb[1] & 1) != 0) {
		goto error;
	}
	if((Srb->Cdb[1] & 0xe0) != 0) {
		goto error;
	}
	if((Srb->Cdb[8] & 1) != 0) {
		goto error;
	}
	if(!GetDataSystemAddress(Thread, 8, FALSE)) {
		goto error;
	}
	*(ULONG*)(Thread->DataSystemAddress + 0) = RtlUlongByteSwap(Ctx->SIZE_OF_MAP * 8 - 1);
	*(ULONG*)(Thread->DataSystemAddress + 4) = RtlUlongByteSwap(512);
	Srb->SrbStatus = SRB_STATUS_SUCCESS;
	Srb->ScsiStatus = SCSISTAT_GOOD;
	Srb->SenseInfoBufferLength = 0;
	Irp->IoStatus.Information = Srb->DataTransferLength = 8;
	Irp->IoStatus.Status =
	    status = STATUS_SUCCESS;
	goto exit;
error:
	KdBreakPoint();
	Srb->SrbStatus = SRB_STATUS_ERROR;
	Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
	Srb->SenseInfoBufferLength = 0;
	Irp->IoStatus.Information = Srb->DataTransferLength = 0;
	Irp->IoStatus.Status =
	    status = STATUS_UNSUCCESSFUL;
exit:
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	Thread->Irp = NULL;
	TRACE(LVL_INFO, "%s", NtStatusName(status));
	LEAVE_FUNCTION(LVL_INFO, "%s", NtStatusName(status));
	return status;
}

UCHAR ModeSenseData3f[] = {
	0x23, 0x00, 0x00, 0x00, 0x08, 0x12, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x0a, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

UCHAR ModeSenseData08[] = {
	0x17, 0x00, 0x00, 0x00, 0x08, 0x12, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

UCHAR ModeSenseData1c[] = {
	0x0f, 0x00, 0x00, 0x00, 0x1c, 0x0a, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

NTSTATUS ExecuteModeSense(PDEVICE_OBJECT DeviceObject, PTHREAD Thread) {
	PIRP Irp = Thread->Irp;
	NTSTATUS status;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
	PUCHAR Data;
	ULONG Length;
	ENTER_FUNCTION(LVL_INFO, "Thread=%p", Thread);
	switch(Srb->Cdb[2] & 0x3f) {
	case 0x3f:
		Data = ModeSenseData3f;
		Length = sizeof(ModeSenseData3f);
		break;
	case 0x08:
		Data = ModeSenseData08;
		Length = sizeof(ModeSenseData08);
		break;
	case 0x1c:
		Data = ModeSenseData1c;
		Length = sizeof(ModeSenseData1c);
		break;
	default:
		goto error;
	}
	Length = min(Length, Srb->DataTransferLength);
	Length = min(Length, Srb->Cdb[4]);
	if(!GetDataSystemAddress(Thread, Length, FALSE)) {
		goto error;
	}
	RtlCopyMemory(Thread->DataSystemAddress, Data, Length);
	Srb->SrbStatus = SRB_STATUS_SUCCESS;
	Srb->ScsiStatus = SCSISTAT_GOOD;
	Srb->SenseInfoBufferLength = 0;
	Irp->IoStatus.Information = Srb->DataTransferLength = Length;
	Irp->IoStatus.Status =
	    status = STATUS_SUCCESS;
	goto exit;
error:
	KdBreakPoint();
	Srb->SrbStatus = SRB_STATUS_ERROR;
	Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
	Srb->SenseInfoBufferLength = 0;
	Irp->IoStatus.Information = Srb->DataTransferLength = 0;
	Irp->IoStatus.Status = status = STATUS_UNSUCCESSFUL;
exit:
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	Thread->Irp = NULL;
	TRACE(LVL_INFO, "%s", NtStatusName(status));
	LEAVE_FUNCTION(LVL_INFO, "%s", NtStatusName(status));
	return status;
}
