#include "headers.h"

ULONG GetMaxChunks(PDEVICE_OBJECT DeviceObject) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	ULONG MAX_CHUNKS = (DeviceExtension->LastLBA + 1) / SIZE_OF_CHUNK;
	//return 10; for test purpose
	return min(MAX_CHUNKS, 500);
}

BOOLEAN SetFormatMode(PDEVICE_OBJECT DeviceObject) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	BOOLEAN result = FALSE;
	KIRQL OldIrql;
	KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
	switch(DeviceExtension->Mode) {
	case VLTL_MODE_DIRECT:
	case VLTL_MODE_ACTIVE:
	case VLTL_MODE_ERROR:
		ChangeMode(DeviceObject, VLTL_MODE_FORMAT);
		result = TRUE;
		break;
	default:
		TRACE(LVL_ERROR, "Unexpected Mode = %d", DeviceExtension->Mode);
	}
	KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
	return result;
}

BOOLEAN GetNewCCN(PDEVICE_OBJECT DeviceObject, PREQUEST Req, PULONG ResultCCN) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	ULONG MAX_CHUNKS = GetMaxChunks(DeviceObject);
	PCHUNK_CTL ChunkCtl = (PVOID)Req->AssociatedMem;
	ULONG i, CCN = 0;
	BOOLEAN result = FALSE;
	PULONG PCCN = ExAllocatePoolWithTag(NonPagedPool, sizeof(ULONG) * MAX_CHUNKS, 'VLTL');
	if(NULL == PCCN) {
		goto finish;
	}
	for(i = 0; i < MAX_CHUNKS; i++) {
		// CAUTION: Read only the first sector of CHUNK_CTL for efficiency
		// so that checksum cannot be valid.
		SyncScsiCallRW_AssociatedMem(Req, SCSIOP_READ, 0,
		                             i * SIZE_OF_CHUNK + LAST_PURGE_POS, 1, DEFAULT_RETRY);
		if(!REQ_SUCCESS(Req)) {
			goto finish;
		}
		PCCN[i] = ChunkCtl->CCN;
		if(CCN < PCCN[i]) {
			CCN = PCCN[i];
		}
	}
retry2:
	CCN++;
	for(i = 0; i < MAX_CHUNKS; i++) {
		if(CCN == PCCN[i]) {
			goto retry2;
		}
	}
	*ResultCCN = CCN;
	result = TRUE;
finish:
	if(NULL != PCCN) {
		ExFreePoolWithTag(PCCN, 'VLTL');
	}
	return result;
}

static UCHAR MBR0[16] = {
	0x00, 0x00, 0x02, 0x00, 0x01, 0xfe, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00
};
static UCHAR MBR1[16] = {
	0x00, 0x01, 0x02, 0x00, 0x07, 0xfe, 0xff, 0xff, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static UCHAR FAT0[62] = {
	0xeb, 0x3c, 0x90, 'M', 'S', 'D', 'O', 'S', '5', '.', '0', 0x00, 0x02, 0x01, 0x06, 0x00,
	0x02, 0x00, 0x02, 0x40, 0x00, 0xf8, 0x01, 0x00, 0x3f, 0x00, 0xff, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x29, 0x33, 0x33, 0x33, 0x33, 'F', 'D', 0x20, 'L', 'E',
	'V', 'E', 'L', 'I', 'N', 'G', 'F', 'A', 'T', '1', '2', 0x20, 0x20, 0x20
};
static UCHAR FATe[3] = {0xf8, 0xff, 0xff};

BOOLEAN SetLevelingConfig(PDEVICE_OBJECT DeviceObject, PMODE_BUFFER ModeBuffer) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PREQUEST Req = NULL;
	BOOLEAN result = FALSE;
	ULONG i, CCN;
	TRACE(LVL_CONFIG, "Version=%d", ModeBuffer->Version);
	TRACE(LVL_CONFIG, "Revision=%d", ModeBuffer->Revision);
	TRACE(LVL_CONFIG, "Mode=%d", ModeBuffer->Mode);
	TRACE(LVL_CONFIG, "MAX_MAP=%d", ModeBuffer->MAX_MAP);
	TRACE(LVL_CONFIG, "SIZE_OF_MAP=%d", ModeBuffer->SIZE_OF_MAP);
	TRACE(LVL_CONFIG, "WEIGHT_LIFE=%d", ModeBuffer->WEIGHT_LIFE);
	TRACE(LVL_CONFIG, "WEIGHT_SPEED=%d", ModeBuffer->WEIGHT_SPEED);
	TRACE(LVL_CONFIG, "Signature=%d", ModeBuffer->Signature);
	TRACE(LVL_CONFIG, "Magic=%d", ModeBuffer->Magic);
	TRACE(LVL_CONFIG, "CacheTimeout=%d", ModeBuffer->CacheTimeout);
	TRACE(LVL_CONFIG, "FlushFaithfully=%d", ModeBuffer->FlushFaithfully);
	if(ModeBuffer->Version != DRIVER_VERSION || ModeBuffer->Revision != DRIVER_REVISION) {
		TRACE(LVL_ERROR, "Version/Revision = %x/%x", ModeBuffer->Version, ModeBuffer->Revision);
		goto finish;
	}
	Req = CreateRequest(DeviceObject, NULL, NULL, SIZE_OF_CACHE << 9);
	if(NULL == Req) {
		goto finish;
	}
	if(ModeBuffer->Mode == VLTL_MODE_CONFIG) {
		PLEVELING_CONF Conf = (PVOID)Req->AssociatedMem;
		PLEVELING_CTX Ctx;
		KIRQL OldIrql;
		SyncScsiCallRW_AssociatedMem(Req, SCSIOP_READ, 0, 0, 1, DEFAULT_RETRY);
		if(!REQ_SUCCESS(Req)) {
			TRACE(LVL_ERROR, "");
			goto finish;
		}
		if(!VerifyChecksum128(Conf, sizeof(LEVELING_CONF))) {
			TRACE(LVL_ERROR, "");
			goto finish;
		}
		KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
		Ctx = DeviceExtension->Ctx;
		if(NULL != Ctx) {
			Ctx->WEIGHT_LIFE = Conf->WEIGHT_LIFE = ModeBuffer->WEIGHT_LIFE;
			Ctx->WEIGHT_SPEED = Conf->WEIGHT_SPEED = ModeBuffer->WEIGHT_SPEED;
			Ctx->CacheTimeout = Conf->CacheTimeout = ModeBuffer->CacheTimeout;
			Ctx->FlushFaithfully = Conf->FlushFaithfully = ModeBuffer->FlushFaithfully;
		}
		KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
		if(NULL == Ctx) {
			TRACE(LVL_ERROR, "");
			goto finish;
		}
		ProduceChecksum128(Conf, sizeof(LEVELING_CONF));
		SyncScsiCallRW_AssociatedMem(Req, SCSIOP_WRITE, 0, 0, 1, DEFAULT_RETRY);
		if(!REQ_SUCCESS(Req)) {
			TRACE(LVL_ERROR, "");
			goto finish;
		}
		result = TRUE;
		goto finish;
	}
	if(ModeBuffer->Mode != VLTL_MODE_FORMAT) {
		TRACE(LVL_ERROR, "Mode = %d", ModeBuffer->Mode);
		goto finish;
	}
	if(!GetNewCCN(DeviceObject, Req, &CCN)) {
		goto finish;
	}
	if(!SetFormatMode(DeviceObject)) {
		goto finish;
	}
	if(0 < ModeBuffer->SIZE_OF_MAP) {
		PCHUNK_CTL ChunkCtl = (PVOID)Req->AssociatedMem;
		SyncScsiCallRW_AssociatedMem(Req, SCSIOP_READ, 0, SIZE_OF_CHUNK + LAST_PURGE_POS,
		                             SIZE_OF_CTL, DEFAULT_RETRY);
		if(!REQ_SUCCESS(Req)) {
			ChangeModeAtomic(DeviceObject, VLTL_MODE_FORMAT, VLTL_MODE_ERROR);
			goto finish;
		}
		if(!VerifyChecksum128(ChunkCtl, sizeof(CHUNK_CTL))) {
			ChunkCtl->WCN = 0;
		}
		ChunkCtl->Version = LEVELING_VERSION;
		ChunkCtl->CCN = CCN;
		ChunkCtl->SCN = 1;
		ChunkCtl->WCN++;
		ChunkCtl->NextChunkNo = 1;
		ChunkCtl->NextChunkWCN = 0;
		ChunkCtl->Start = 0;
		ChunkCtl->End = 0;
		if(!DeviceExtension->RemovableMedia) {
			for(i = 0; i < SIZE_OF_CACHE; i += 8) {
				ChunkCtl->LBA[i >> 3] = NOT_MAPPED;
			}
			ChunkCtl->LBA[0] = 0;
			ChunkCtl->End = SIZE_OF_CACHE;
		}
		ProduceChecksum128(ChunkCtl, sizeof(CHUNK_CTL));
		SyncScsiCallRW_AssociatedMem(Req, SCSIOP_WRITE, 0, SIZE_OF_CHUNK + LAST_PURGE_POS, SIZE_OF_CTL,
		                             DEFAULT_RETRY);
		if(!REQ_SUCCESS(Req)) {
			ChangeModeAtomic(DeviceObject, VLTL_MODE_FORMAT, VLTL_MODE_ERROR);
			goto finish;
		}
	}
	if(0 < ModeBuffer->SIZE_OF_MAP && !DeviceExtension->RemovableMedia) {
		PPURGE_CTL PurgeCtl = (PVOID)(Req->AssociatedMem + (LAST_CACHE_POS << 9));
		RtlZeroMemory(Req->AssociatedMem, SIZE_OF_CACHE << 9);
		RtlCopyMemory(Req->AssociatedMem + 0x1be, MBR1, sizeof(MBR1));
		*(ULONG*)(Req->AssociatedMem + 0x1ca) = (ModeBuffer->SIZE_OF_MAP << 3) - 64;
		RtlCopyMemory(Req->AssociatedMem + 0x1b8, &ModeBuffer->Signature, 4);
		Req->AssociatedMem[0x1fe] = 0x55;
		Req->AssociatedMem[0x1ff] = 0xaa;
		PurgeCtl->Version = LEVELING_VERSION;
		PurgeCtl->CCN = CCN;
		PurgeCtl->SCN = 1;
		PurgeCtl->ChunkNo = 1;
		PurgeCtl->PurgePos = 0;
		PurgeCtl->Magic = ModeBuffer->Magic;
		for(i = 0; i < SIZE_OF_CACHE; i += 8) {
			PurgeCtl->LBA[i >> 3] = NOT_MAPPED;
		}
		PurgeCtl->LBA[0] = 0;
		ProduceChecksum128(PurgeCtl, sizeof(PURGE_CTL));
		SyncScsiCallRW_AssociatedMem(Req, SCSIOP_WRITE, 0, SIZE_OF_CHUNK, SIZE_OF_CACHE, DEFAULT_RETRY);
		if(!REQ_SUCCESS(Req)) {
			ChangeModeAtomic(DeviceObject, VLTL_MODE_FORMAT, VLTL_MODE_ERROR);
			goto finish;
		}
	}
	RtlZeroMemory(Req->AssociatedMem, 65 << 9);
	if(0 < ModeBuffer->SIZE_OF_MAP) {
		PLEVELING_CONF Conf = (PVOID)Req->AssociatedMem;
		Conf->Version = LEVELING_VERSION;
		Conf->CCN = CCN;
		Conf->SIZE_OF_MAP = ModeBuffer->SIZE_OF_MAP;
		Conf->WEIGHT_LIFE = ModeBuffer->WEIGHT_LIFE;
		Conf->WEIGHT_SPEED = ModeBuffer->WEIGHT_SPEED;
		Conf->Magic = ModeBuffer->Magic;
		Conf->CacheTimeout = ModeBuffer->CacheTimeout;
		Conf->FlushFaithfully = ModeBuffer->FlushFaithfully;
		ProduceChecksum128(Conf, sizeof(LEVELING_CONF));
		RtlCopyMemory(Req->AssociatedMem + 0x1be, MBR0, sizeof(MBR0));
		RtlCopyMemory(Req->AssociatedMem + 0x200, FAT0, sizeof(FAT0));
		Req->AssociatedMem[0x3fe] = 0x55;
		Req->AssociatedMem[0x3ff] = 0xaa;
		RtlCopyMemory(Req->AssociatedMem + 0xe00, FATe, sizeof(FATe));
		RtlCopyMemory(Req->AssociatedMem + 0x1000, FATe, sizeof(FATe));
		RtlCopyMemory(Req->AssociatedMem + 0x1b8, &ModeBuffer->Signature, 4);
		Req->AssociatedMem[0x1fe] = 0x55;
		Req->AssociatedMem[0x1ff] = 0xaa;
	}
	if(0 == ModeBuffer->SIZE_OF_MAP && !DeviceExtension->RemovableMedia) {
		RtlCopyMemory(Req->AssociatedMem + 0x1be, MBR1, sizeof(MBR1));
		*(ULONG*)(Req->AssociatedMem + 0x1ca) = DeviceExtension->LastLBA - 63;
		RtlCopyMemory(Req->AssociatedMem + 0x1b8, &ModeBuffer->Signature, 4);
		Req->AssociatedMem[0x1fe] = 0x55;
		Req->AssociatedMem[0x1ff] = 0xaa;
	}
	SyncScsiCallRW_AssociatedMem(Req, SCSIOP_WRITE, 0, 0, 65, DEFAULT_RETRY);
	if(!REQ_SUCCESS(Req)) {
		ChangeModeAtomic(DeviceObject, VLTL_MODE_FORMAT, VLTL_MODE_ERROR);
		goto finish;
	}
	ChangeModeAtomic(DeviceObject, VLTL_MODE_FORMAT, VLTL_MODE_COMPLETED);
	result = TRUE;
finish:
	if(NULL != Req) {
		FreeRequest(Req);
	}
	return result;
}

BOOLEAN GetLevelingConfig(PDEVICE_OBJECT DeviceObject, PMODE_BUFFER ModeBuffer) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PLEVELING_CTX Ctx;
	KIRQL OldIrql;
	RtlZeroMemory(ModeBuffer, sizeof(MODE_BUFFER));
	ModeBuffer->ModeDataLength = sizeof(MODE_BUFFER) - 1;
	ModeBuffer->MediumType = 0;
	ModeBuffer->Reserved = 0;
	ModeBuffer->BlockDescriptorLength = 0;
	ModeBuffer->PageCode = 40;
	ModeBuffer->PageLength = sizeof(MODE_BUFFER) - 6;
	ModeBuffer->Version = DRIVER_VERSION;
	ModeBuffer->Revision = DRIVER_REVISION;
	ModeBuffer->Mode = DeviceExtension->Mode;
	KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
	Ctx = DeviceExtension->Ctx;
	if(NULL != Ctx) {
		ModeBuffer->MAX_MAP = Ctx->MAX_CHUNKS * SIZE_OF_CHUNK / 10;
		ModeBuffer->SIZE_OF_MAP = Ctx->SIZE_OF_MAP;
		ModeBuffer->WEIGHT_LIFE = Ctx->WEIGHT_LIFE;
		ModeBuffer->WEIGHT_SPEED = Ctx->WEIGHT_SPEED;
		ModeBuffer->CacheTimeout = Ctx->CacheTimeout;
		ModeBuffer->FlushFaithfully = Ctx->FlushFaithfully;
	} else {
		ModeBuffer->MAX_MAP = GetMaxChunks(DeviceObject) * SIZE_OF_CHUNK / 10;
		ModeBuffer->SIZE_OF_MAP = ModeBuffer->MAX_MAP / 2;
		ModeBuffer->WEIGHT_LIFE = 50;
		ModeBuffer->WEIGHT_SPEED = 50;
		ModeBuffer->CacheTimeout = 30;
		ModeBuffer->FlushFaithfully = 0;
	}
	KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
	ProduceChecksum128(ModeBuffer, sizeof(MODE_BUFFER));
	TRACE(LVL_CONFIG, "Version=%d", ModeBuffer->Version);
	TRACE(LVL_CONFIG, "Revision=%d", ModeBuffer->Revision);
	TRACE(LVL_CONFIG, "Mode=%d", ModeBuffer->Mode);
	TRACE(LVL_CONFIG, "MAX_MAP=%d", ModeBuffer->MAX_MAP);
	TRACE(LVL_CONFIG, "SIZE_OF_MAP=%d", ModeBuffer->SIZE_OF_MAP);
	TRACE(LVL_CONFIG, "WEIGHT_LIFE=%d", ModeBuffer->WEIGHT_LIFE);
	TRACE(LVL_CONFIG, "WEIGHT_SPEED=%d", ModeBuffer->WEIGHT_SPEED);
	TRACE(LVL_CONFIG, "CacheTimeout=%d", ModeBuffer->CacheTimeout);
	TRACE(LVL_CONFIG, "FlushFaithfully=%d", ModeBuffer->FlushFaithfully);
	return TRUE;
}

NTSTATUS LevelingControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	NTSTATUS status = STATUS_NOT_SUPPORTED;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	LPCSTR name = IrpStackName(IrpStack);
	ULONG InputBufferLength = IrpStack->Parameters.DeviceIoControl.InputBufferLength;
	ULONG OutputBufferLength = IrpStack->Parameters.DeviceIoControl.OutputBufferLength;
	PSCSI_PASS_THROUGH Spt = Irp->AssociatedIrp.SystemBuffer;
	PMODE_BUFFER ModeBuffer = (PVOID)(((PUCHAR)Spt) + Spt->DataBufferOffset);
	ENTER_FUNCTION(LVL_CONFIG, "%s", name);
	if(Spt->Cdb[0] == SCSIOP_MODE_SENSE) {
		if(Spt->DataTransferLength != sizeof(MODE_BUFFER)) {
			goto exit;
		}
		if(OutputBufferLength != Spt->DataBufferOffset + Spt->DataTransferLength) {
			goto exit;
		}
		if(!(Spt->Cdb[1] == 8 && Spt->Cdb[2] == 40 && Spt->Cdb[3] == 41)) {
			goto exit;
		}
		status = GetLevelingConfig(DeviceObject, ModeBuffer) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
		Spt->ScsiStatus = SCSISTAT_GOOD;
		Spt->SenseInfoLength = 0;
		Spt->DataTransferLength = sizeof(MODE_BUFFER);
		Irp->IoStatus.Status = status;
		Irp->IoStatus.Information = Spt->DataBufferOffset + sizeof(MODE_BUFFER);
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
	} else if(Spt->Cdb[0] == SCSIOP_MODE_SELECT) {
		if(Spt->DataTransferLength != sizeof(MODE_BUFFER)) {
			goto exit;
		}
		if(InputBufferLength < Spt->DataBufferOffset + Spt->DataTransferLength) {
			goto exit;
		}
		if(!(ModeBuffer->BlockDescriptorLength == 0 && ModeBuffer->PageCode == 40)) {
			goto exit;
		}
		if(!VerifyChecksum128(ModeBuffer, sizeof(MODE_BUFFER))) {
			goto exit;
		}
		status = SetLevelingConfig(DeviceObject, ModeBuffer) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
		Spt->ScsiStatus = SCSISTAT_GOOD;
		Spt->SenseInfoLength = 0;
		Spt->DataTransferLength = sizeof(MODE_BUFFER);
		Irp->IoStatus.Status = status;
		Irp->IoStatus.Information = sizeof(SCSI_PASS_THROUGH);
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
	}
exit:
	LEAVE_FUNCTION(LVL_CONFIG, "%s", NtStatusName(status));
	return status;
}
