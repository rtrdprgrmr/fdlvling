#include "headers.h"

IO_COMPLETION_ROUTINE CheckCompletion1;
REQUEST_COMPLETION CheckCompletion2;
REQUEST_COMPLETION CheckCompletion3;

NTSTATUS LevelingCheck(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	TRACE(LVL_CHECK, "Irp=%p", Irp);
	IoMarkIrpPending(Irp);
	IoCopyCurrentIrpStackLocationToNext(Irp);
	IoSetCompletionRoutine(Irp, CheckCompletion1, NULL, TRUE, TRUE, TRUE);
	IoCallDriver(DeviceExtension->NextLowerDriver, Irp);
	return STATUS_PENDING;
}

NTSTATUS CheckCompletion1(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	NTSTATUS status = Irp->IoStatus.Status;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
	PREQUEST Req;
	TRACE(LVL_CHECK, "Irp=%p", Irp);
	if(!NT_SUCCESS(status) || !SRB_SUCCESS(Srb)) {
		TRACE(LVL_ERROR, "SrbStatus=%s, ScsiStatus=%s, NtStatus=%s", SrbStatusName(Srb->SrbStatus),
		      ScsiStatusName(Srb->ScsiStatus), NtStatusName(status));
		return STATUS_CONTINUE_COMPLETION;
	}
	Req = CreateRequest(DeviceObject, CheckCompletion2, Irp, 512);
	if(NULL == Req) {
		return STATUS_CONTINUE_COMPLETION;
	}
	ScsiCallReadCapacity(Req);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

VOID CheckCompletion2(PREQUEST Req) {
	PIRP Irp = Req->Context;
	PDEVICE_OBJECT DeviceObject = Req->DeviceObject;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	ULONG LastLBA = RtlUlongByteSwap(*(ULONG*)(Req->AssociatedMem + 0));
	ULONG BlockLength = RtlUlongByteSwap(*(ULONG*)(Req->AssociatedMem + 4));
	TRACE(LVL_CHECK, "Irp=%p", Irp);
	if(!REQ_SUCCESS(Req)) {
		goto finish;
	}
	DeviceExtension->LastLBA = LastLBA;
	if(BlockLength != 512 || LastLBA < 0x1bffff) {
		ChangeModeAtomic(DeviceObject, VLTL_MODE_INITIAL, VLTL_MODE_DIRECT);
		goto finish;
	}
	Req->Completion = CheckCompletion3;
	ScsiCallRW_AssociatedMem(Req, SCSIOP_READ, 0, 0, 1, 0);
	return;
finish:
	FreeRequest(Req);
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return;
}

VOID CheckCompletion3(PREQUEST Req) {
	PIRP Irp = Req->Context;
	PDEVICE_OBJECT DeviceObject = Req->DeviceObject;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
	TRACE(LVL_CHECK, "Irp=%p", Irp);
	if(!REQ_SUCCESS(Req)) {
		goto finish;
	}
	DeviceExtension->LevelingCapable = TRUE;
	if(!InitializeLevelingCtx(DeviceObject, Req->AssociatedMem)) {
		ChangeModeAtomic(DeviceObject, VLTL_MODE_INITIAL, VLTL_MODE_DIRECT);
		goto finish;
	}
	Srb->SrbStatus = SRB_STATUS_ERROR;
	Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
	Srb->SenseInfoBufferLength = 0;
	Irp->IoStatus.Information = Srb->DataTransferLength = 0;
	Irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
finish:
	FreeRequest(Req);
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
}
