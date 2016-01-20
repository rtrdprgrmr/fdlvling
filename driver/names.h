#define IOCTL_STORAGE_518c 0x2d518c
#define IOCTL_STORAGE_5190 0x2d5190
#define IOCTL_MOUNTMGR_0004 0x4d0004
#define IOCTL_MOUNTMGR_0010 0x4d0010
#define IOCTL_MOUNTMGR_0014 0x4d0014
#define IOCTL_VOLSNAP_0018 0x530018

LPCSTR IrpStackName(PIO_STACK_LOCATION IrpStack);
LPCSTR PowerStateName(POWER_STATE_TYPE Type, POWER_STATE Stat);
LPCSTR PowerActionName(POWER_ACTION PowerAction);
LPCSTR DeviceControlName(ULONG IoControlCode);
LPCSTR DeviceTypeName(DEVICE_TYPE DeviceType);
LPCSTR SrbStatusName(UCHAR SrbStatus);
LPCSTR ScsiStatusName(UCHAR ScsiStatus);
LPCSTR ScsiSenseKeyName(UCHAR SenseKey);
LPCSTR ScsiASCName(UCHAR ASC);
LPCSTR NtStatusName(NTSTATUS status);
