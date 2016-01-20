#define SRB_SUCCESS(Srb) ((SRB_STATUS_SUCCESS == SRB_STATUS(Srb->SrbStatus) \
                           || SRB_STATUS_DATA_OVERRUN == SRB_STATUS(Srb->SrbStatus)) \
                          && SCSISTAT_GOOD == Srb->ScsiStatus)

typedef struct _DEVICE_EXTENSION DEVICE_EXTENSION, *PDEVICE_EXTENSION;
struct _DEVICE_EXTENSION {
	BOOLEAN IsControlDevice;
	BOOLEAN IsAnotherDevice;
	PDEVICE_OBJECT AnotherDeviceObject;
	PDEVICE_OBJECT DeviceObject;
	PDEVICE_OBJECT PhysicalDeviceObject;
	PDEVICE_OBJECT NextLowerDriver;
	BOOLEAN AllowAlways;
	BOOLEAN TobeRemovable;
	BOOLEAN TobeFixed;
	volatile UCHAR Mode;
	volatile BOOLEAN RemovableMedia;
	volatile BOOLEAN LevelingCapable;
	volatile DWORD MaximumTransferLength;
	volatile ULONG LastLBA;
	volatile ULONG TURErrors;
	LIST_ENTRY ListEntry;
	PLEVELING_CTX Ctx;
};

extern KSPIN_LOCK GlobalSpinLock;

DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE VltlAddDevice;
__drv_dispatchType(IRP_MJ_POWER) DRIVER_DISPATCH VltlDispatchPower;
__drv_dispatchType(IRP_MJ_PNP) DRIVER_DISPATCH VltlDispatchPnp;
__drv_dispatchType(IRP_MJ_SCSI) DRIVER_DISPATCH VltlDispatchScsi;
__drv_dispatchType(IRP_MJ_DEVICE_CONTROL) DRIVER_DISPATCH VltlDispatchIoControl;
__drv_dispatchType(IRP_MJ_CREATE)
__drv_dispatchType(IRP_MJ_CLOSE) DRIVER_DISPATCH VltlDispatchPassThrough;
__drv_dispatchType(IRP_MJ_SHUTDOWN) DRIVER_DISPATCH VltlDispatchShutdown;
__drv_dispatchType(IRP_MJ_SYSTEM_CONTROL) DRIVER_DISPATCH VltlDispatchDefault;
IO_COMPLETION_ROUTINE VltlDispatchPnpCompletion;
IO_COMPLETION_ROUTINE VltlDispatchIoControlCompletion;
IO_COMPLETION_ROUTINE VltlDispatchScsiTURCompletion;
DRIVER_UNLOAD VltlDriverUnload;

VOID SetCheckCondition(PSCSI_REQUEST_BLOCK Srb, UCHAR SenseKey, UCHAR ASC, UCHAR ASCQ);
VOID ChangeMode(PDEVICE_OBJECT DeviceObject, UCHAR Mode);
VOID ChangeModeAtomic(PDEVICE_OBJECT DeviceObject, UCHAR FromMode, UCHAR Mode);

VOID InitializeParameters(PUNICODE_STRING RegistryPath);
BOOLEAN CheckDeviceConfig(PDEVICE_OBJECT PhysicalDeviceObject, PBOOLEAN AllowAlways,
                          PBOOLEAN TobeRemovable, PBOOLEAN TobeFixed);
