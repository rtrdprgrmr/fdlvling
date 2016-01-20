#include "headers.h"

NTSTATUS DwordQueryRoutine(PWSTR ValueName, ULONG ValueType, PVOID ValueData,
                           ULONG ValueLength, PVOID Context, PVOID EntryContext) {
	if(REG_DWORD != ValueType || 4 != ValueLength || NULL == EntryContext) {
		KdBreakPoint();
		return STATUS_SUCCESS;
	}
	*(ULONG*)EntryContext = *(ULONG*)ValueData;
	return STATUS_SUCCESS;
}

BOOLEAN PatternMatch(PWSTR Name, PWSTR Pattern, ULONG Length) {
	ULONG i = 0, mark = 0, j = 0;
	for(;;) {
		if(i < Length && Pattern[i] == '*') {
			i++;
			mark = i;
			if(i == Length || Pattern[i] == 0) {
				return TRUE;
			}
			continue;
		}
		if(Name[j] == 0) {
			if(i == Length || Pattern[i] == 0) {
				return TRUE;
			}
			return FALSE;
		}
		if(i == Length || Pattern[i] != Name[j]) {
			if(mark == 0) {
				return FALSE;
			}
			j = j - (i - mark) + 1;
			i = mark;
			continue;
		}
		i++;
		j++;
	}
}

NTSTATUS CompareQueryRoutine(PWSTR ValueName, ULONG ValueType, PVOID ValueData,
                             ULONG ValueLength, PVOID Context, PVOID EntryContext) {
	PWSTR Pattern = ValueData;
	PWSTR HardwareIDs = Context;
	if(REG_SZ != ValueType || NULL == EntryContext || NULL == Context) {
		KdBreakPoint();
		return STATUS_SUCCESS;
	}
	while(HardwareIDs[0]) {
		if(PatternMatch(HardwareIDs, Pattern, ValueLength)) {
			(*(ULONG*)EntryContext)++;
			return STATUS_SUCCESS;
		}
		while(*HardwareIDs++);
	}
	return STATUS_SUCCESS;
}

RTL_QUERY_REGISTRY_TABLE ParametersQueryTable[] = {
	{ NULL, RTL_QUERY_REGISTRY_SUBKEY, L"Parameters", NULL, REG_NONE },
	{ DwordQueryRoutine, 0, L"TraceBufferSize", &trace_buffer_size, REG_NONE },
	{ DwordQueryRoutine, 0, L"TraceLevel", &trace_lvl, REG_NONE },
	{ DwordQueryRoutine, 0, L"TraceFunctionLevel", &trace_lvl_function, REG_NONE },
	{ NULL, 0, NULL, NULL, REG_NONE }
};

WCHAR RegistryRoot[256];

VOID InitializeParameters(PUNICODE_STRING RegistryPath) {
	NTSTATUS status;
	if(RegistryPath->Length > 255) {
		return;
	}
	RtlCopyMemory(RegistryRoot, RegistryPath->Buffer, RegistryPath->Length);
	RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, RegistryRoot, ParametersQueryTable, NULL, NULL);
}

RTL_QUERY_REGISTRY_TABLE AddDeviceQueryTable[] = {
	{ NULL, RTL_QUERY_REGISTRY_SUBKEY, L"Parameters", NULL, REG_NONE },
	{ CompareQueryRoutine, 0, L"HardwareIdAllowAlways", NULL, REG_NONE },
	{ CompareQueryRoutine, 0, L"HardwareIdDeny", NULL, REG_NONE },
	{ CompareQueryRoutine, 0, L"HardwareIdAllow", NULL, REG_NONE },
	{ CompareQueryRoutine, 0, L"HardwareIdTobeRemovable", NULL, REG_NONE },
	{ CompareQueryRoutine, 0, L"HardwareIdTobeFixed", NULL, REG_NONE },
	{ NULL, 0, NULL, NULL, REG_NONE }
};

BOOLEAN CheckDeviceConfig(PDEVICE_OBJECT PhysicalDeviceObject, PBOOLEAN AllowAlways,
                          PBOOLEAN TobeRemovable, PBOOLEAN TobeFixed) {
	NTSTATUS status;
	PRTL_QUERY_REGISTRY_TABLE QueryTable;
	ULONG BufferLength = 256;
	PVOID Buffer;
	ULONG ResultLength = 0;
	ULONG HardwareIdAllowAlways = 0;
	ULONG HardwareIdDeny = 0;
	ULONG HardwareIdAllow = 0;
	ULONG HardwareIdTobeRemovable = 0;
	ULONG HardwareIdTobeFixed = 0;
retry:
	Buffer = ExAllocatePoolWithTag(NonPagedPool, BufferLength, 'VLTL');
	if(NULL == Buffer) {
		TRACE(LVL_ERROR, "Insufficient Memory");
		return FALSE;
	}
	status = IoGetDeviceProperty(PhysicalDeviceObject, DevicePropertyHardwareID,
	                             BufferLength, Buffer, &ResultLength);
	if(status == STATUS_BUFFER_TOO_SMALL) {
		ExFreePoolWithTag(Buffer, 'VLTL');
		BufferLength = ResultLength;
		goto retry;
	}
	if(!NT_SUCCESS(status)) {
		TRACE(LVL_ERROR, "IoGetDeviceProperty()=%s", NtStatusName(status));
		return FALSE;
	}
	QueryTable = ExAllocatePoolWithTag(NonPagedPool, sizeof(AddDeviceQueryTable),
	                                   'VLTL');
	if(NULL == QueryTable) {
		TRACE(LVL_ERROR, "Insufficient Memory");
		ExFreePoolWithTag(Buffer, 'VLTL');
		return FALSE;
	}
	RtlCopyMemory(QueryTable, AddDeviceQueryTable, sizeof(AddDeviceQueryTable));
	QueryTable[1].EntryContext = &HardwareIdAllowAlways;
	QueryTable[2].EntryContext = &HardwareIdDeny;
	QueryTable[3].EntryContext = &HardwareIdAllow;
	QueryTable[4].EntryContext = &HardwareIdTobeRemovable;
	QueryTable[5].EntryContext = &HardwareIdTobeFixed;
	RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, RegistryRoot, QueryTable, Buffer,
	                       NULL);
	ExFreePoolWithTag(Buffer, 'VLTL');
	ExFreePoolWithTag(QueryTable, 'VLTL');
	if(0 == HardwareIdAllowAlways) {
		if(0 < HardwareIdDeny) {
			return FALSE;
		}
		if(0 == HardwareIdAllow) {
			return FALSE;
		}
	}
	*AllowAlways = HardwareIdAllowAlways ? TRUE : FALSE;
	*TobeRemovable = HardwareIdTobeRemovable ? TRUE : FALSE;
	*TobeFixed = HardwareIdTobeFixed ? TRUE : FALSE;
	return TRUE;
}
