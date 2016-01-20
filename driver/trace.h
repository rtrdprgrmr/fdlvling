#define TRACE(lvl, fmt, ...) \
	trace_printf(lvl, __FUNCTION__, fmt, __VA_ARGS__)

#define ENTER_FUNCTION(lvl, fmt, ...) \
	enter_function(lvl, __FUNCTION__, fmt, __VA_ARGS__)

#define LEAVE_FUNCTION(lvl, fmt, ...) \
	leave_function(lvl, __FUNCTION__, fmt, __VA_ARGS__)

#define STATIC_ASSERT(expr) extern char _STATIC_ASSERT_TMP[(expr)?1:-1]

extern int trace_lvl;
extern int trace_lvl_function;
extern int trace_buffer_size;

void trace_printf(int lvl, const char* name, const char* fmt, ...);
void enter_function(int lvl, const char* name, const char* fmt, ...);
void leave_function(int lvl, const char* name, const char* fmt, ...);
void trace_start();
void trace_stop();
void trace_vprintf(const char* fmt, va_list argl);
int trace_read(char* dst, int length);

#define LVL_ERROR 0x01
#define LVL_SYSTEM 0x02
#define LVL_ADDDEVICE 0x04
#define LVL_POWER 0x08

#define LVL_PNP 0x10
#define LVL_DEFAULT 0x20
#define LVL_IOCONTROL 0x40

#define LVL_CHECK 0x100
#define LVL_CONFIG 0x200
#define LVL_CTX 0x400
#define LVL_RECOVERY 0x800

#define LVL_INFO 0x1000
#define LVL_CHUNK 0x2000
#define LVL_FLUSH 0x4000
#define LVL_CACHE 0x8000

#define LVL_RELOCATION 0x10000
#define LVL_VREQ 0x20000
#define LVL_TURCOMPLETION 0x40000
#define LVL_GATE 0x80000

#define LVL_RW 0x100000
#define LVL_DISPATCHSCSI 0x200000
