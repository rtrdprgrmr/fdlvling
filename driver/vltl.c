#include "headers.h"

KSPIN_LOCK GlobalSpinLock;

VOID HookDeviceCapabilities(PDEVICE_OBJECT DeviceObject, PIRP Irp);
VOID HookStorageProperty(PDEVICE_OBJECT DeviceObject, PIRP Irp, PSTORAGE_PROPERTY_QUERY Query);

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
	int i;
	InitializeParameters(RegistryPath);
	trace_start();
	ENTER_FUNCTION(LVL_SYSTEM, "");
	TRACE(LVL_SYSTEM, "DriverEntry");
	if(trace_buffer_size > 0) {
		CreateControlDevice(DriverObject);
	}
	KeInitializeSpinLock(&GlobalSpinLock);
	StartRecoveryWorkerThread();
	DriverObject->DriverExtension->AddDevice = VltlAddDevice;
	for(i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
		DriverObject->MajorFunction[i] = VltlDispatchDefault;
	}
	DriverObject->MajorFunction[IRP_MJ_POWER] = VltlDispatchPower;
	DriverObject->MajorFunction[IRP_MJ_PNP] = VltlDispatchPnp;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = VltlDispatchIoControl;
	DriverObject->MajorFunction[IRP_MJ_SCSI] = VltlDispatchScsi;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = VltlDispatchPassThrough;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = VltlDispatchPassThrough;
	DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = VltlDispatchShutdown;
	DriverObject->DriverUnload = VltlDriverUnload;
	LEAVE_FUNCTION(LVL_SYSTEM, "");
	return STATUS_SUCCESS;
}

VOID VltlDriverUnload(PDRIVER_OBJECT DriverObject) {
	ENTER_FUNCTION(LVL_SYSTEM, "");
	TRACE(LVL_SYSTEM, "DriverUnload");
	StopRecoveryWorkerThread();
	LEAVE_FUNCTION(LVL_SYSTEM, "");
	trace_stop();
}

NTSTATUS VltlAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
	NTSTATUS status;
	PDEVICE_OBJECT DeviceObject = NULL;
	PDEVICE_OBJECT AnotherDeviceObject = NULL;
	PDEVICE_OBJECT NextLowerDriver;
	PDEVICE_EXTENSION DeviceExtension;
	BOOLEAN AllowAlways;
	BOOLEAN TobeRemovable;
	BOOLEAN TobeFixed;
	ENTER_FUNCTION(LVL_ADDDEVICE, "");
	if(!CheckDeviceConfig(PhysicalDeviceObject, &AllowAlways, &TobeRemovable, &TobeFixed)) {
		goto exit;
	}
	status = IoCreateDevice(DriverObject, sizeof(DEVICE_EXTENSION), NULL,
	                        FILE_DEVICE_DISK, FILE_DEVICE_SECURE_OPEN, FALSE, &DeviceObject);
	if(!NT_SUCCESS(status)) {
		TRACE(LVL_ERROR, "IoCreateDevice()=%s", NtStatusName(status));
		goto exit;
	}
	status = CreateAnotherDevice(DeviceObject, &AnotherDeviceObject);
	if(!NT_SUCCESS(status)) {
		IoDeleteDevice(DeviceObject);
		goto exit;
	}
	NextLowerDriver = IoAttachDeviceToDeviceStack(DeviceObject, PhysicalDeviceObject);
	if(NULL == NextLowerDriver) {
		IoDeleteDevice(AnotherDeviceObject);
		IoDeleteDevice(DeviceObject);
		goto exit;
	}
	TRACE(LVL_ADDDEVICE, "IoAttachDeviceToDeviceStack FDO=%p NDO=%p PDO=%p", DeviceObject,
	      NextLowerDriver, PhysicalDeviceObject);
	DeviceExtension = DeviceObject->DeviceExtension;
	RtlZeroMemory(DeviceExtension, sizeof(DEVICE_EXTENSION));
	DeviceExtension->DeviceObject = DeviceObject;
	DeviceExtension->NextLowerDriver = NextLowerDriver;
	DeviceExtension->AllowAlways = AllowAlways;
	DeviceExtension->TobeRemovable = TobeRemovable;
	DeviceExtension->TobeFixed = TobeFixed;
	DeviceExtension->PhysicalDeviceObject = PhysicalDeviceObject;
	DeviceExtension->AnotherDeviceObject = AnotherDeviceObject;
	DeviceObject->DeviceType = NextLowerDriver->DeviceType;
	DeviceObject->Characteristics = NextLowerDriver->Characteristics;
	DeviceObject->Flags |= NextLowerDriver->Flags & DO_DIRECT_IO;
	DeviceObject->Flags |= NextLowerDriver->Flags & DO_POWER_PAGABLE;
	DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
	ChangeMode(DeviceObject, VLTL_MODE_INITIAL);
exit:
	status = STATUS_SUCCESS; // forced to be success for fail-safe
	LEAVE_FUNCTION(LVL_ADDDEVICE, "%s", NtStatusName(status));
	return status;
}

NTSTATUS VltlDispatchShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	NTSTATUS status;
	PDEVICE_EXTENSION_CONTROL DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	LPCSTR name = IrpStackName(IrpStack);
	if(!DeviceExtension->IsControlDevice) {
		return VltlDispatchPassThrough(DeviceObject, Irp);
	}
	ENTER_FUNCTION(LVL_SYSTEM, "");
	if(DeviceExtension->IsAnotherDevice) {
		status = LevelingDispatch(DeviceExtension->OriginalDeviceObject, Irp, FALSE);
		goto exit;
	}
	Irp->IoStatus.Status = status = STATUS_SUCCESS;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
exit:
	TRACE(LVL_SYSTEM, "%s %s", name, NtStatusName(status));
	LEAVE_FUNCTION(LVL_SYSTEM, "%s", NtStatusName(status));
	return status;
}

NTSTATUS VltlDispatchPassThrough(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	NTSTATUS status;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	if(DeviceExtension->IsControlDevice) {
		return ControlDispatch(DeviceObject, Irp);
	}
	IoSkipCurrentIrpStackLocation(Irp);
	status = IoCallDriver(DeviceExtension->NextLowerDriver, Irp);
	return status;
}

NTSTATUS VltlDispatchDefault(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	NTSTATUS status;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	LPCSTR name = IrpStackName(IrpStack);
	if(DeviceExtension->IsControlDevice) {
		return ControlDispatch(DeviceObject, Irp);
	}
	ENTER_FUNCTION(LVL_DEFAULT, "%s", name);
	if(VLTL_MODE_ACTIVE <= DeviceExtension->Mode) {
		Irp->IoStatus.Status = status = STATUS_NOT_SUPPORTED;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		goto exit;
	}
	IoSkipCurrentIrpStackLocation(Irp);
	status = IoCallDriver(DeviceExtension->NextLowerDriver, Irp);
exit:
	TRACE(LVL_DEFAULT, "%s %s", name, NtStatusName(status));
	LEAVE_FUNCTION(LVL_DEFAULT, "%s", NtStatusName(status));
	return status;
}

NTSTATUS VltlDispatchPower(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	NTSTATUS status;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	LPCSTR name = IrpStackName(IrpStack);
	POWER_STATE_TYPE Type = IrpStack->Parameters.Power.Type;
	POWER_STATE State = IrpStack->Parameters.Power.State;
	if(DeviceExtension->IsControlDevice) {
		return ControlDispatch(DeviceObject, Irp);
	}
	ENTER_FUNCTION(LVL_POWER, "%s", name);
	if(IRP_MN_SET_POWER == IrpStack->MinorFunction) {
		if(Type == DevicePowerState) {
			PoSetPowerState(DeviceObject, Type, State);
		}
	}
#if _NT_TARGET_VERSION >= 0x600
	IoSkipCurrentIrpStackLocation(Irp);
	status = IoCallDriver(DeviceExtension->NextLowerDriver, Irp);
#else
	PoStartNextPowerIrp(Irp);
	IoSkipCurrentIrpStackLocation(Irp);
	status = PoCallDriver(DeviceExtension->NextLowerDriver, Irp);
#endif
	TRACE(LVL_POWER, "%s %s", name, NtStatusName(status));
	LEAVE_FUNCTION(LVL_POWER, "%s", NtStatusName(status));
	return status;
}

NTSTATUS VltlDispatchPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	NTSTATUS status;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	LPCSTR name = IrpStackName(IrpStack);
	PDEVICE_OBJECT AttachedDevice;
	if(DeviceExtension->IsControlDevice) {
		return ControlDispatch(DeviceObject, Irp);
	}
	ENTER_FUNCTION(LVL_PNP, "%s Irp=%p", name, Irp);
	if(VLTL_MODE_ACTIVE == DeviceExtension->Mode) {
		if(IRP_MN_QUERY_REMOVE_DEVICE == IrpStack->MinorFunction) {
			status = LevelingDispatch(DeviceObject, Irp, FALSE);
			goto exit;
		}
	}
	switch(IrpStack->MinorFunction) {
	case IRP_MN_REMOVE_DEVICE:
		IoSkipCurrentIrpStackLocation(Irp);
		status = IoCallDriver(DeviceExtension->NextLowerDriver, Irp);
		IoDetachDevice(DeviceExtension->NextLowerDriver);
		FreeLevelingCtx(DeviceObject, TRUE);
		IoDeleteDevice(DeviceExtension->AnotherDeviceObject);
		IoDeleteDevice(DeviceObject);
		TRACE(LVL_PNP, "%s %s", name, NtStatusName(status));
		goto exit;
	case IRP_MN_DEVICE_USAGE_NOTIFICATION:
		AttachedDevice = IoGetAttachedDeviceReference(DeviceObject);
		if(AttachedDevice->Flags & DO_POWER_PAGABLE) {
			DeviceObject->Flags |= DO_POWER_PAGABLE;
		}
		ObDereferenceObject(AttachedDevice);
	default:
		break;
	}
	IoCopyCurrentIrpStackLocationToNext(Irp);
	IoSetCompletionRoutine(Irp, VltlDispatchPnpCompletion, NULL, TRUE, TRUE, TRUE);
	status = IoCallDriver(DeviceExtension->NextLowerDriver, Irp);
exit:
	LEAVE_FUNCTION(LVL_PNP, "%s", NtStatusName(status));
	return status;
}

NTSTATUS VltlDispatchPnpCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                                   PVOID Context) {
	NTSTATUS status = Irp->IoStatus.Status;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	LPCSTR name = IrpStackName(IrpStack);
	ENTER_FUNCTION(LVL_PNP, "Irp=%p", Irp);
	TRACE(LVL_PNP, "%s %s", name, NtStatusName(status));
	if(Irp->PendingReturned) {
		IoMarkIrpPending(Irp);
	}
	switch(IrpStack->MinorFunction) {
	case IRP_MN_DEVICE_USAGE_NOTIFICATION:
		if(!(DeviceExtension->NextLowerDriver->Flags & DO_POWER_PAGABLE)) {
			DeviceObject->Flags &= ~DO_POWER_PAGABLE;
		}
		break;
	case IRP_MN_QUERY_CAPABILITIES:
		HookDeviceCapabilities(DeviceObject, Irp);
		break;
	default:
		break;
	}
	LEAVE_FUNCTION(LVL_PNP, "%s", NtStatusName(status));
	return STATUS_CONTINUE_COMPLETION;
}

VOID HookDeviceCapabilities(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	NTSTATUS status = Irp->IoStatus.Status;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	PDEVICE_CAPABILITIES Capabilities =
	    IrpStack->Parameters.DeviceCapabilities.Capabilities;
	TRACE(LVL_INFO, "DeviceD1=%d", Capabilities->DeviceD1);
	TRACE(LVL_INFO, "DeviceD2=%d", Capabilities->DeviceD2);
	TRACE(LVL_INFO, "LockSupported=%d", Capabilities->LockSupported);
	TRACE(LVL_INFO, "EjectSupported=%d", Capabilities->EjectSupported);
	TRACE(LVL_INFO, "Removable=%d", Capabilities->Removable);
	TRACE(LVL_INFO, "DockDevice=%d", Capabilities->DockDevice);
	TRACE(LVL_INFO, "UniqueID=%d", Capabilities->UniqueID);
	TRACE(LVL_INFO, "SilentInstall=%d", Capabilities->SilentInstall);
	TRACE(LVL_INFO, "RawDeviceOK=%d", Capabilities->RawDeviceOK);
	TRACE(LVL_INFO, "SurpriseRemovalOK=%d", Capabilities->SurpriseRemovalOK);
	TRACE(LVL_INFO, "WakeFromD0=%d", Capabilities->WakeFromD0);
	TRACE(LVL_INFO, "WakeFromD1=%d", Capabilities->WakeFromD1);
	TRACE(LVL_INFO, "WakeFromD2=%d", Capabilities->WakeFromD2);
	TRACE(LVL_INFO, "WakeFromD3=%d", Capabilities->WakeFromD3);
	TRACE(LVL_INFO, "HardwareDisabled=%d", Capabilities->HardwareDisabled);
	TRACE(LVL_INFO, "NonDynamic=%d", Capabilities->NonDynamic);
	TRACE(LVL_INFO, "WarmEjectSupported=%d", Capabilities->WarmEjectSupported);
	TRACE(LVL_INFO, "NoDisplayInUI=%d", Capabilities->NoDisplayInUI);
	TRACE(LVL_INFO, "Address=%d", Capabilities->Address);
	TRACE(LVL_INFO, "UINumber=%d", Capabilities->UINumber);
	TRACE(LVL_INFO, "DeviceState[PowerSystemWorking]=%d",
	      Capabilities->DeviceState[PowerSystemWorking]);
	TRACE(LVL_INFO, "DeviceState[PowerSystemSleeping1]=%d",
	      Capabilities->DeviceState[PowerSystemSleeping1]);
	TRACE(LVL_INFO, "DeviceState[PowerSystemSleeping2]=%d",
	      Capabilities->DeviceState[PowerSystemSleeping2]);
	TRACE(LVL_INFO, "DeviceState[PowerSystemSleeping3]=%d",
	      Capabilities->DeviceState[PowerSystemSleeping3]);
	TRACE(LVL_INFO, "DeviceState[PowerSystemHibernate]=%d",
	      Capabilities->DeviceState[PowerSystemHibernate]);
	TRACE(LVL_INFO, "DeviceState[PowerSystemShutdown]=%d",
	      Capabilities->DeviceState[PowerSystemShutdown]);
	TRACE(LVL_INFO, "SystemWake=%d", Capabilities->SystemWake);
	TRACE(LVL_INFO, "DeviceWake=%d", Capabilities->DeviceWake);
	TRACE(LVL_INFO, "D1Latency=%d", Capabilities->D1Latency);
	TRACE(LVL_INFO, "D2Latency=%d", Capabilities->D2Latency);
	TRACE(LVL_INFO, "D3Latency=%d", Capabilities->D3Latency);
}

NTSTATUS VltlDispatchIoControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	NTSTATUS status;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	LPCSTR name = IrpStackName(IrpStack);
	ULONG IoControlCode = IrpStack->Parameters.DeviceIoControl.IoControlCode;
	ULONG InputBufferLength = IrpStack->Parameters.DeviceIoControl.InputBufferLength;
	PVOID Context = NULL;
	if(DeviceExtension->IsControlDevice) {
		return ControlDispatch(DeviceObject, Irp);
	}
	ENTER_FUNCTION(LVL_IOCONTROL, "%s Irp=%p", name, Irp);
	if(IOCTL_SCSI_PASS_THROUGH == IoControlCode && InputBufferLength >= sizeof(SCSI_PASS_THROUGH)) {
		PSCSI_PASS_THROUGH Spt = Irp->AssociatedIrp.SystemBuffer;
		if(Spt->Cdb[0] == SCSIOP_INQUIRY) {
			goto pass;
		}
		if(Spt->Cdb[0] == SCSIOP_MODE_SENSE || Spt->Cdb[0] == SCSIOP_MODE_SELECT) {
			if(DeviceExtension->LevelingCapable) {
				status = LevelingControl(DeviceObject, Irp);
				if(status != STATUS_NOT_SUPPORTED) {
					goto exit;
				}
			}
		}
	}
	if(VLTL_MODE_ACTIVE <= DeviceExtension->Mode) {
		if(IOCTL_STORAGE_QUERY_PROPERTY == IoControlCode) {
			goto pass;
		}
		if(IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES == IoControlCode) {
			// TBD TRIM support
		}
		Irp->IoStatus.Status = status = STATUS_NOT_SUPPORTED;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		goto exit;
	}
pass:
	if(IOCTL_STORAGE_QUERY_PROPERTY == IoControlCode) {
		ULONG Length = IrpStack->Parameters.DeviceIoControl.InputBufferLength;
		PVOID Input = ExAllocatePoolWithTag(NonPagedPool, Length, 'VLTL');
		if(NULL != Input) {
			RtlCopyMemory(Input, Irp->AssociatedIrp.SystemBuffer, Length);
			Context = Input;
		}
	}
	IoCopyCurrentIrpStackLocationToNext(Irp);
	IoSetCompletionRoutine(Irp, VltlDispatchIoControlCompletion, Context, TRUE, TRUE, TRUE);
	status = IoCallDriver(DeviceExtension->NextLowerDriver, Irp);
exit:
	LEAVE_FUNCTION(LVL_IOCONTROL, "%s", NtStatusName(status));
	return status;
}

NTSTATUS VltlDispatchIoControlCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp,
        PVOID Context) {
	NTSTATUS status = Irp->IoStatus.Status;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	LPCSTR name = IrpStackName(IrpStack);
	ULONG IoControlCode = IrpStack->Parameters.DeviceIoControl.IoControlCode;
	ENTER_FUNCTION(LVL_IOCONTROL, "%s Irp=%p", name, Irp);
	if(Irp->PendingReturned) {
		IoMarkIrpPending(Irp);
	}
	switch(IoControlCode) {
	case IOCTL_STORAGE_QUERY_PROPERTY:
		if(NULL != Context) {
			HookStorageProperty(DeviceObject, Irp, Context);
		}
		break;
	}
	if(NULL != Context) {
		ExFreePoolWithTag(Context, 'VLTL');
	}
	TRACE(LVL_IOCONTROL, "%s(0x%x) %s", name, IoControlCode, NtStatusName(status));
	LEAVE_FUNCTION(LVL_IOCONTROL, "%s", NtStatusName(status));
	return STATUS_CONTINUE_COMPLETION;
}

VOID HookStorageProperty(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                         PSTORAGE_PROPERTY_QUERY Query) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	NTSTATUS status = Irp->IoStatus.Status;
	ULONG Length = (ULONG)Irp->IoStatus.Information;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	PSTORAGE_DESCRIPTOR_HEADER Property = Irp->AssociatedIrp.SystemBuffer;
	ULONG InputBufferLength = IrpStack->Parameters.DeviceIoControl.InputBufferLength;
	if(!NT_SUCCESS(status)) {
		return;
	}
	if(InputBufferLength < sizeof(STORAGE_PROPERTY_QUERY)) {
		KdBreakPoint();
		return;
	}
	if(Query->QueryType != PropertyStandardQuery) {
		return;
	}
	if(Length < sizeof(STORAGE_DESCRIPTOR_HEADER) || Length < Property->Size) {
		return;
	}
	switch(Query->PropertyId) {
	case StorageDeviceProperty:
		TRACE(LVL_INFO, "StorageDeviceProperty");
		if(Length < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
			KdBreakPoint();
		} else {
			PSTORAGE_DEVICE_DESCRIPTOR desc = (PVOID)Property;
			TRACE(LVL_INFO, "DeviceType=%d", desc->DeviceType);
			TRACE(LVL_INFO, "DeviceTypeModifier=%d", desc->DeviceTypeModifier);
			TRACE(LVL_INFO, "RemovableMedia=%d", desc->RemovableMedia);
			TRACE(LVL_INFO, "CommandQueueing=%d", desc->CommandQueueing);
			if(DeviceExtension->TobeRemovable) {
				desc->RemovableMedia = TRUE;
			}
			if(DeviceExtension->TobeFixed) {
				desc->RemovableMedia = FALSE;
			}
			DeviceExtension->RemovableMedia = desc->RemovableMedia;
		}
		break;
	case StorageAdapterProperty:
		TRACE(LVL_INFO, "StorageAdapterProperty");
		if(Length < sizeof(STORAGE_ADAPTER_DESCRIPTOR)) {
			KdBreakPoint();
		} else {
			PSTORAGE_ADAPTER_DESCRIPTOR desc = (PVOID)Property;
			TRACE(LVL_INFO, "MaximumTransferLength=%d", desc->MaximumTransferLength);
			TRACE(LVL_INFO, "MaximumPhysicalPages=%d", desc->MaximumPhysicalPages);
			TRACE(LVL_INFO, "AlignmentMask=%d", desc->AlignmentMask);
			TRACE(LVL_INFO, "AdapterUsesPio=%d", desc->AdapterUsesPio);
			TRACE(LVL_INFO, "AdapterScansDown=%d", desc->AdapterScansDown);
			TRACE(LVL_INFO, "CommandQueueing=%d", desc->CommandQueueing);
			TRACE(LVL_INFO, "AcceleratedTransfer=%d", desc->AcceleratedTransfer);
			TRACE(LVL_INFO, "BusType=%d", desc->BusType);
			TRACE(LVL_INFO, "BusMajorVersion=%d", desc->BusMajorVersion);
			TRACE(LVL_INFO, "BusMinorVersion=%d", desc->BusMinorVersion);
			DeviceExtension->MaximumTransferLength = desc->MaximumTransferLength;
		}
		break;
	}
}

NTSTATUS VltlDispatchScsi(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	NTSTATUS status;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
	LPCSTR name = IrpStackName(IrpStack);
	if(DeviceExtension->IsControlDevice) {
		return ControlDispatch(DeviceObject, Irp);
	}
	ENTER_FUNCTION(LVL_DISPATCHSCSI, "%s Irp=%p", name, Irp);
	if(SRB_FUNCTION_EXECUTE_SCSI == Srb->Function
	    && SCSIOP_TEST_UNIT_READY == Srb->Cdb[0]) {
		IoCopyCurrentIrpStackLocationToNext(Irp);
		IoSetCompletionRoutine(Irp, VltlDispatchScsiTURCompletion, NULL, TRUE, TRUE, TRUE);
		status = IoCallDriver(DeviceExtension->NextLowerDriver, Irp);
		goto exit;
	}
	if(VLTL_MODE_ACTIVE <= DeviceExtension->Mode) {
		switch(Srb->Function) {
		case SRB_FUNCTION_CLAIM_DEVICE:
		case SRB_FUNCTION_LOCK_QUEUE:
		case SRB_FUNCTION_UNLOCK_QUEUE:
		case SRB_FUNCTION_FLUSH_QUEUE:
			goto pass;
		}
		if(SRB_FUNCTION_EXECUTE_SCSI == Srb->Function && SCSIOP_READ == Srb->Cdb[0]) {
			status = LevelingDispatch(DeviceObject, Irp, TRUE);
		} else {
			status = LevelingDispatch(DeviceObject, Irp, FALSE);
		}
		goto exit;
	}
	if(VLTL_MODE_INITIAL == DeviceExtension->Mode
	    && SRB_FUNCTION_EXECUTE_SCSI == Srb->Function
	    && SCSIOP_READ_CAPACITY == Srb->Cdb[0]
	    && (DeviceExtension->RemovableMedia || DeviceExtension->AllowAlways)
	    && 0x8000 <= DeviceExtension->MaximumTransferLength) {
		status = LevelingCheck(DeviceObject, Irp);
		goto exit;
	}
pass:
	IoSkipCurrentIrpStackLocation(Irp);
	status = IoCallDriver(DeviceExtension->NextLowerDriver, Irp);
	TRACE(LVL_DISPATCHSCSI, "%s %s", name, NtStatusName(status));
exit:
	LEAVE_FUNCTION(LVL_DISPATCHSCSI, "%s", NtStatusName(status));
	return status;
}

NTSTATUS VltlDispatchScsiTURCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                                       PVOID Context) {
	NTSTATUS status = Irp->IoStatus.Status;
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpStack = IoGetCurrentIrpStackLocation(Irp);
	PSCSI_REQUEST_BLOCK Srb = IrpStack->Parameters.Scsi.Srb;
	LPCSTR name = IrpStackName(IrpStack);
	ENTER_FUNCTION(LVL_TURCOMPLETION, "%s Irp=%p", name, Irp);
	TRACE(LVL_TURCOMPLETION, "%s %s", name, NtStatusName(status));
	if(Irp->PendingReturned) {
		IoMarkIrpPending(Irp);
	}
	if(STATUS_CANCELLED == status) {
		goto exit;
	} else if(NT_SUCCESS(status) && SRB_SUCCESS(Srb)) {
		DeviceExtension->TURErrors = 0;
	} else {
		KIRQL OldIrql;
		KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
		if(VLTL_MODE_DIRECT == DeviceExtension->Mode) {
			ChangeMode(DeviceObject, VLTL_MODE_INITIAL);
		}
		if(VLTL_MODE_ACTIVE == DeviceExtension->Mode) {
			DeviceExtension->TURErrors++;
			TRACE(LVL_ERROR, "TURErrors=%d", DeviceExtension->TURErrors);
			if(DeviceExtension->TURErrors > 5) {
				ChangeMode(DeviceObject, VLTL_MODE_ERROR);
			}
		}
		KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
		if(VLTL_MODE_ERROR == DeviceExtension->Mode) {
			FreeLevelingCtx(DeviceObject, FALSE);
		}
	}
	if(VLTL_MODE_ACTIVE < DeviceExtension->Mode) {
		SetCheckCondition(Srb, SCSI_SENSE_NOT_READY, SCSI_ADSENSE_NO_MEDIA_IN_DEVICE, 0x00);
		status = STATUS_IO_DEVICE_ERROR;
		Irp->IoStatus.Status = status;
	}
	if(VLTL_MODE_COMPLETED == DeviceExtension->Mode) {
		FreeLevelingCtx(DeviceObject, FALSE);
	}
exit:
	LEAVE_FUNCTION(LVL_TURCOMPLETION, "%s", NtStatusName(status));
	return STATUS_CONTINUE_COMPLETION;
}

VOID SetCheckCondition(PSCSI_REQUEST_BLOCK Srb, UCHAR SenseKey, UCHAR ASC, UCHAR ASCQ) {
	Srb->SrbStatus = SRB_STATUS_ERROR;
	Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
	Srb->DataTransferLength = 0;
	if(18 <= Srb->SenseInfoBufferLength) {
		PUCHAR SenseInfoBuffer = Srb->SenseInfoBuffer;
		Srb->SenseInfoBufferLength = 18;
		RtlZeroMemory(SenseInfoBuffer, Srb->SenseInfoBufferLength);
		SenseInfoBuffer[0] = 0x70;
		SenseInfoBuffer[2] = SenseKey;
		SenseInfoBuffer[7] = 0x0a;
		SenseInfoBuffer[12] = ASC;
		SenseInfoBuffer[13] = ASCQ;
		Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
	}
}

VOID ChangeMode(PDEVICE_OBJECT DeviceObject, UCHAR Mode) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	static char* names[] = {"DIRECT", "INITIAL", "ACTIVE", "RECOVERY", "FORMAT", "COMPLETED", "ERROR"};
	if(Mode <= 6) {
		TRACE(LVL_SYSTEM, "%s -> %s", names[DeviceExtension->Mode], names[Mode]);
	}
	if(Mode == VLTL_MODE_INITIAL) {
		DeviceExtension->LevelingCapable = FALSE;
	}
	DeviceExtension->Mode = Mode;
}

VOID ChangeModeAtomic(PDEVICE_OBJECT DeviceObject, UCHAR FromMode, UCHAR Mode) {
	PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	KIRQL OldIrql;
	KeAcquireSpinLock(&GlobalSpinLock, &OldIrql);
	if(DeviceExtension->Mode == FromMode) {
		ChangeMode(DeviceObject, Mode);
	}
	KeReleaseSpinLock(&GlobalSpinLock, OldIrql);
}
