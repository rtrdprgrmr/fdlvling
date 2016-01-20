#define VLTL_MODE_DIRECT 0
#define VLTL_MODE_INITIAL 1
#define VLTL_MODE_ACTIVE 2
#define VLTL_MODE_RECOVERY 3
#define VLTL_MODE_FORMAT 4
#define VLTL_MODE_COMPLETED 5
#define VLTL_MODE_ERROR 6
#define VLTL_MODE_CONFIG 7

#define DRIVER_VERSION 1
#define DRIVER_REVISION 1

typedef struct _MODE_BUFFER MODE_BUFFER, *PMODE_BUFFER;

struct _MODE_BUFFER {
	UCHAR ModeDataLength; // == sizeof(MODE_BUFFER)-1
	UCHAR MediumType; // == 0
	UCHAR Reserved; // == 0
	UCHAR BlockDescriptorLength; // == 0
	UCHAR PageCode; // == 40
	UCHAR PageLength; // == sizeof(MODE_BUFFER)-6
	UCHAR Version;
	UCHAR Revision;
	UCHAR Mode;
	UCHAR CacheTimeout;
	BOOLEAN FlushFaithfully;
	UCHAR Padding[1];
	ULONG MAX_MAP;
	ULONG SIZE_OF_MAP;
	ULONG WEIGHT_LIFE;
	ULONG WEIGHT_SPEED;
	ULONG Signature;
	ULONG Magic;
	ULONG Checksum[4];
};

void ProduceChecksum128(void* buf, int len);
int VerifyChecksum128(void* buf, int len);
