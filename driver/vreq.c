#include "headers.h"

IO_COMPLETION_ROUTINE ScsiCallCompletion;
REQUEST_COMPLETION ScsiCallDefaultHandler;
REQUEST_COMPLETION ScsiCallRWHandler;
REQUEST_COMPLETION SyncScsiCallRWCompletion;

PREQUEST CreateRequest(PDEVICE_OBJECT DeviceObject, PREQUEST_COMPLETION Completion, PVOID Context,
                       ULONG MemSize) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PREQUEST Req;
	Req = ExAllocatePoolWithTag(NonPagedPool, sizeof(REQUEST), 'VLTL');
	if(NULL == Req) {
		KdBreakPoint();
		goto insufficient_resources;
	}
	RtlZeroMemory(Req, sizeof(REQUEST));
	Req->Completion = Completion;
	Req->Context = Context;
	Req->DeviceObject = DeviceObject;
	Req->NextLowerDriver = DeviceExtension->NextLowerDriver;
	Req->MaximumTransferLength = DeviceExtension->MaximumTransferLength;
	Req->Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
	if(NULL == Req->Irp) {
		KdBreakPoint();
		goto insufficient_resources;
	}
	if(0 < MemSize) {
		Req->AssociatedMem = ExAllocatePoolWithTag(NonPagedPool, MemSize, 'VLTL');
		if(NULL == Req->AssociatedMem) {
			KdBreakPoint();
			goto insufficient_resources;
		}
		Req->AssociatedMdl = IoAllocateMdl(Req->AssociatedMem, MemSize, FALSE, FALSE, NULL);
		if(NULL == Req->AssociatedMdl) {
			KdBreakPoint();
			goto insufficient_resources;
		}
		MmBuildMdlForNonPagedPool(Req->AssociatedMdl);
	}
	return Req;
insufficient_resources:
	FreeRequest(Req);
	return NULL;
}

VOID FreeRequest(PREQUEST Req) {
	if(NULL != Req) {
		if(NULL != Req->AssociatedMdl) {
			IoFreeMdl(Req->AssociatedMdl);
		}
		if(NULL != Req->AssociatedMem) {
			ExFreePoolWithTag(Req->AssociatedMem, 'VLTL');
		}
		if(NULL != Req->Irp) {
			IoFreeIrp(Req->Irp);
		}
		ExFreePoolWithTag(Req, 'VLTL');
	}
}

VOID ReuseRequest(PREQUEST Req) {
	ASSERT(Req->Used == FALSE);
	Req->Used = TRUE;
	Req->Transferred = 0;
	Req->ScsiStatus = SCSISTAT_GOOD;
	Req->SrbStatus = SRB_STATUS_SUCCESS;
	Req->Status = STATUS_SUCCESS;
	Req->SenseTransferred = 0;
}

NTSTATUS ScsiCallCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
	PREQUEST Req = Context;
	PSCSI_REQUEST_BLOCK Srb = Req->Srb;
	ENTER_FUNCTION(LVL_VREQ, "%p", Req);
	ASSERT(Irp == Req->Irp);
	ASSERT(Req->Busy == TRUE);
	Req->Busy = FALSE;
	Req->Status = Irp->IoStatus.Status;
	Req->SrbStatus = SRB_STATUS(Srb->SrbStatus);
	Req->ScsiStatus = Srb->ScsiStatus;
	Req->SenseTransferred = 0;
	if(!REQ_SUCCESS(Req)) {
		TRACE(LVL_ERROR, "SrbStatus=%s ScsiStatus=%s NtStatus=%s",
		      SrbStatusName(Req->SrbStatus), ScsiStatusName(Req->ScsiStatus), NtStatusName(Req->Status));
	}
	if(Srb->Function != SRB_FUNCTION_EXECUTE_SCSI) {
		goto skip;
	}
	if(REQ_SUCCESS(Req)) {
		if(Irp->IoStatus.Information == Srb->DataTransferLength) {
			Req->Transferred += Srb->DataTransferLength >> 9;
		} else {
			TRACE(LVL_ERROR, "Irp->IoStatus.Information=%x Srb->DataTransferLength=%x",
			      Irp->IoStatus.Information ,
			      Srb->DataTransferLength);
			KdBreakPoint();
			Req->Status = STATUS_UNSUCCESSFUL;
		}
	}
	if(Srb->SrbStatus & SRB_STATUS_AUTOSENSE_VALID) {
		Req->SenseTransferred = Srb->SenseInfoBufferLength;
		if(Req->SenseBuffer != Srb->SenseInfoBuffer) {
			RtlCopyMemory(Req->SenseBuffer, Srb->SenseInfoBuffer, Req->SenseTransferred);
		}
		if(Srb->SenseInfoBufferLength > 13) {
			PUCHAR SenseInfoBuffer = Srb->SenseInfoBuffer;
			TRACE(LVL_ERROR, "%s %s ASCQ=%x", ScsiSenseKeyName(SenseInfoBuffer[2]),
			      ScsiASCName(SenseInfoBuffer[12]),
			      SenseInfoBuffer[13]);
		}
	}
skip:
	if(Srb->SrbFlags & SRB_FLAGS_FREE_SENSE_BUFFER) {
		KdBreakPoint();
		ExFreePool(Srb->SenseInfoBuffer);
	}
	IoReuseIrp(Irp, STATUS_UNSUCCESSFUL);
	Req->Handler(Req);
	LEAVE_FUNCTION(LVL_VREQ, "");
	return STATUS_MORE_PROCESSING_REQUIRED;
}

VOID ScsiCallDefaultHandler(PREQUEST Req) {
	ENTER_FUNCTION(LVL_VREQ, "%p", Req);
	ASSERT(Req->Used == TRUE);
	Req->Used = FALSE;
	Req->Completion(Req);
	LEAVE_FUNCTION(LVL_VREQ, "%p", Req);
}

VOID ScsiCallRW(PREQUEST Req, UCHAR OPC, PMDL Mdl, PUCHAR DataBuffer, ULONG PLBA, ULONG LEN,
                UCHAR RetryLimit) {
	ReuseRequest(Req);
	Req->Handler = ScsiCallRWHandler;
	Req->OPC = OPC;
	Req->Mdl = Mdl;
	Req->DataBuffer = DataBuffer;
	Req->PLBA = PLBA;
	Req->LEN = LEN;
	Req->RetryLimit = RetryLimit;
	Req->Retry = 0;
	ScsiCallRWHandler(Req);
}

VOID ScsiCallRWHandler(PREQUEST Req) {
	PIRP Irp = Req->Irp;
	PSCSI_REQUEST_BLOCK Srb = Req->Srb;
	PIO_STACK_LOCATION IrpStack;
	PUCHAR DataBuffer = Req->DataBuffer + (Req->Transferred << 9);
	ULONG PLBA = Req->PLBA + Req->Transferred;
	ULONG LEN = Req->LEN - Req->Transferred;
	ENTER_FUNCTION(LVL_VREQ, "%p", Req);
	ASSERT(Req->Used == TRUE);
	if(Req->LEN <= Req->Transferred) {
		Req->Used = FALSE;
		Req->Completion(Req);
		goto exit;
	}
	if(!REQ_SUCCESS(Req)) {
		if(Req->Retry >= Req->RetryLimit) {
			Req->Used = FALSE;
			Req->Completion(Req);
			goto exit;
		}
		Req->Retry++;
	} else {
		Req->Retry = 0;
	}
	LEN = min(LEN, Req->MaximumTransferLength >> 9);
	RtlZeroMemory(Srb, sizeof(SCSI_REQUEST_BLOCK));
	Srb->Length = sizeof(SCSI_REQUEST_BLOCK);
	Srb->Function = SRB_FUNCTION_EXECUTE_SCSI;
	Srb->QueueTag = 0;
	Srb->QueueAction = SRB_SIMPLE_TAG_REQUEST;
	Srb->SenseInfoBufferLength = 255;
	Srb->SenseInfoBuffer = Req->SenseBuffer;
	Srb->OriginalRequest = Irp;
	Srb->TimeOutValue = 60;
	Srb->SrbFlags |= SRB_FLAGS_PORT_DRIVER_ALLOCSENSE;
	Srb->SrbFlags |= SRB_FLAGS_NO_QUEUE_FREEZE;
	Srb->SrbFlags |= SRB_FLAGS_ADAPTER_CACHE_ENABLE;
	Srb->SrbFlags |= SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
	Srb->SrbFlags |= SRB_FLAGS_QUEUE_ACTION_ENABLE;
	if(Req->OPC == SCSIOP_READ) {
		TRACE(LVL_VREQ, "r(%p) PLBA=%x LEN=%x", Req, PLBA, LEN);
		Srb->SrbFlags |= SRB_FLAGS_DATA_IN;
	} else if(Req->OPC == SCSIOP_WRITE) {
		TRACE(LVL_VREQ, "w(%p) PLBA=%x LEN=%x", Req, PLBA, LEN);
		Srb->SrbFlags |= SRB_FLAGS_DATA_OUT;
	} else {
		KdBreakPoint();
	}
	Srb->CdbLength = 10;
	Srb->Cdb[0] = Req->OPC;
	(*(ULONG*)(Srb->Cdb + 2)) = RtlUlongByteSwap(PLBA);
	(*(USHORT*)(Srb->Cdb + 7)) = RtlUshortByteSwap(LEN);
	Srb->DataBuffer = DataBuffer;
	Srb->DataTransferLength = LEN * 512;
	IrpStack = IoGetNextIrpStackLocation(Irp);
	IrpStack->MajorFunction = IRP_MJ_SCSI;
	IrpStack->Parameters.Scsi.Srb = Srb;
	Irp->MdlAddress = Req->Mdl;
	ASSERT(Req->Busy == FALSE);
	Req->Busy = TRUE;
	IoSetCompletionRoutine(Irp, ScsiCallCompletion, Req, TRUE, TRUE, TRUE);
	IoCallDriver(Req->NextLowerDriver, Irp);
exit:
	LEAVE_FUNCTION(LVL_VREQ, "", 0);
}

VOID SyncScsiCallRW(PREQUEST Req, UCHAR OPC, PMDL Mdl, PUCHAR DataBuffer, ULONG PLBA, ULONG LEN,
                    UCHAR RetryLimit) {
	PREQUEST_COMPLETION OldCompletion = Req->Completion;
	PVOID OldContext = Req->Context;
	KEVENT Event;
	KeInitializeEvent(&Event, NotificationEvent, FALSE);
	Req->Completion = SyncScsiCallRWCompletion;
	Req->Context = &Event;
	ScsiCallRW(Req, OPC, Mdl, DataBuffer, PLBA, LEN, RetryLimit);
	KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
	Req->Completion = OldCompletion;
	Req->Context = OldContext;
}

VOID SyncScsiCallRWCompletion(PREQUEST Req) {
	KeSetEvent((PKEVENT)Req->Context, IO_NO_INCREMENT, FALSE);
}

VOID ScsiCallReadCapacity(PREQUEST Req) {
	PIRP Irp = Req->Irp;
	PSCSI_REQUEST_BLOCK Srb = Req->Srb;
	PIO_STACK_LOCATION IrpStack;
	ENTER_FUNCTION(LVL_VREQ, "%p", Req);
	ReuseRequest(Req);
	Req->Handler = ScsiCallDefaultHandler;
	Req->OPC = SCSIOP_READ_CAPACITY;
	Req->Mdl = Req->AssociatedMdl;
	Req->DataBuffer = Req->AssociatedMem;
	RtlZeroMemory(Srb, sizeof(SCSI_REQUEST_BLOCK));
	Srb->Length = sizeof(SCSI_REQUEST_BLOCK);
	Srb->Function = SRB_FUNCTION_EXECUTE_SCSI;
	Srb->QueueTag = 0;
	Srb->QueueAction = SRB_SIMPLE_TAG_REQUEST;
	Srb->SenseInfoBufferLength = 255;
	Srb->SenseInfoBuffer = Req->SenseBuffer;
	Srb->OriginalRequest = Irp;
	Srb->TimeOutValue = 60;
	Srb->SrbFlags |= SRB_FLAGS_PORT_DRIVER_ALLOCSENSE;
	Srb->SrbFlags |= SRB_FLAGS_NO_QUEUE_FREEZE;
	Srb->SrbFlags |= SRB_FLAGS_ADAPTER_CACHE_ENABLE;
	Srb->SrbFlags |= SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
	Srb->SrbFlags |= SRB_FLAGS_QUEUE_ACTION_ENABLE;
	Srb->SrbFlags |= SRB_FLAGS_DATA_IN;
	Srb->CdbLength = 10;
	Srb->Cdb[0] = Req->OPC;
	Srb->DataBuffer = Req->DataBuffer;
	Srb->DataTransferLength = 8;
	IrpStack = IoGetNextIrpStackLocation(Irp);
	IrpStack->MajorFunction = IRP_MJ_SCSI;
	IrpStack->Parameters.Scsi.Srb = Srb;
	Irp->MdlAddress = Req->Mdl;
	ASSERT(Req->Busy == FALSE);
	Req->Busy = TRUE;
	IoSetCompletionRoutine(Irp, ScsiCallCompletion, Req, TRUE, TRUE, TRUE);
	IoCallDriver(Req->NextLowerDriver, Irp);
	LEAVE_FUNCTION(LVL_VREQ, "", 0);
}

VOID ScsiCallFlushCache(PREQUEST Req) {
	PIRP Irp = Req->Irp;
	PSCSI_REQUEST_BLOCK Srb = Req->Srb;
	PIO_STACK_LOCATION IrpStack;
	ENTER_FUNCTION(LVL_VREQ, "%p", Req);
	ReuseRequest(Req);
	Req->Handler = ScsiCallDefaultHandler;
	Req->OPC = SCSIOP_SYNCHRONIZE_CACHE;
	Req->Mdl = NULL;
	Req->DataBuffer = NULL;
	RtlZeroMemory(Srb, sizeof(SCSI_REQUEST_BLOCK));
	Srb->Length = sizeof(SCSI_REQUEST_BLOCK);
	Srb->Function = SRB_FUNCTION_EXECUTE_SCSI;
	Srb->QueueTag = 0xff;
	Srb->QueueAction = SRB_SIMPLE_TAG_REQUEST;
	Srb->SenseInfoBufferLength = 255;
	Srb->SenseInfoBuffer = Req->SenseBuffer;
	Srb->OriginalRequest = Irp;
	Srb->TimeOutValue = 60;
	Srb->SrbFlags |= SRB_FLAGS_PORT_DRIVER_ALLOCSENSE;
	Srb->SrbFlags |= SRB_FLAGS_NO_QUEUE_FREEZE;
	Srb->SrbFlags |= SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
	Srb->CdbLength = 10;
	Srb->Cdb[0] = Req->OPC;
	IrpStack = IoGetNextIrpStackLocation(Irp);
	IrpStack->MajorFunction = IRP_MJ_SCSI;
	IrpStack->Parameters.Scsi.Srb = Srb;
	Irp->MdlAddress = NULL;
	ASSERT(Req->Busy == FALSE);
	Req->Busy = TRUE;
	IoSetCompletionRoutine(Irp, ScsiCallCompletion, Req, TRUE, TRUE, TRUE);
	IoCallDriver(Req->NextLowerDriver, Irp);
	LEAVE_FUNCTION(LVL_VREQ, "", 0);
}

VOID ScsiCallSrbFlush(PREQUEST Req) {
	PIRP Irp = Req->Irp;
	PSCSI_REQUEST_BLOCK Srb = Req->Srb;
	PIO_STACK_LOCATION IrpStack;
	ENTER_FUNCTION(LVL_VREQ, "%p", Req);
	ReuseRequest(Req);
	Req->Handler = ScsiCallDefaultHandler;
	Req->OPC = SCSIOP_SYNCHRONIZE_CACHE;
	Req->Mdl = NULL;
	Req->DataBuffer = NULL;
	RtlZeroMemory(Srb, sizeof(SCSI_REQUEST_BLOCK));
	Srb->Length = sizeof(SCSI_REQUEST_BLOCK);
	Srb->Function = SRB_FUNCTION_FLUSH;
	Srb->QueueTag = 0xff;
	Srb->QueueAction = SRB_SIMPLE_TAG_REQUEST;
	Srb->SenseInfoBufferLength = 255;
	Srb->SenseInfoBuffer = Req->SenseBuffer;
	Srb->OriginalRequest = Irp;
	Srb->TimeOutValue = 60;
	Srb->SrbFlags |= SRB_FLAGS_PORT_DRIVER_ALLOCSENSE;
	Srb->SrbFlags |= SRB_FLAGS_NO_QUEUE_FREEZE;
	Srb->SrbFlags |= SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
	IrpStack = IoGetNextIrpStackLocation(Irp);
	IrpStack->MajorFunction = IRP_MJ_SCSI;
	IrpStack->Parameters.Scsi.Srb = Srb;
	Irp->MdlAddress = NULL;
	ASSERT(Req->Busy == FALSE);
	Req->Busy = TRUE;
	IoSetCompletionRoutine(Irp, ScsiCallCompletion, Req, TRUE, TRUE, TRUE);
	IoCallDriver(Req->NextLowerDriver, Irp);
	LEAVE_FUNCTION(LVL_VREQ, "", 0);
}

VOID ScsiCallRW_AssociatedMem(PREQUEST Req, UCHAR OPC, ULONG pos, ULONG PLBA, ULONG LEN,
                              UCHAR RetryLimit) {
	ScsiCallRW(Req, OPC, Req->AssociatedMdl, Req->AssociatedMem + pos, PLBA, LEN, RetryLimit);
}

VOID SyncScsiCallRW_AssociatedMem(PREQUEST Req, UCHAR OPC, ULONG pos, ULONG PLBA, ULONG LEN,
                                  UCHAR RetryLimit) {
	SyncScsiCallRW(Req, OPC, Req->AssociatedMdl, Req->AssociatedMem + pos, PLBA, LEN, RetryLimit);
}
