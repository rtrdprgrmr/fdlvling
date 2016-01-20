#include "headers.h"

int trace_lvl;
int trace_lvl_function;
int trace_buffer_size;

#define VDBGPRINT(fmt, argl) vDbgPrintEx(DPFLTR_DEFAULT_ID, 0, fmt, argl)

static int indent = 0;
static char* prevname = 0;

static char* copy_string(char* p, char* bufend, const char* s) {
	while(bufend - p > 0) {
		char a = *s++;
		if(a == 0) {
			break;
		}
		*p++ = a;
	}
	return p;
}

static char* make_prefix(char* p, char* bufend, int ind) {
	while(bufend - p > 0) {
		if(ind < 0) {
			*p++ = '-';
			break;
		}
		if(ind > 10) {
			*p++ = '+';
			ind = 10;
		} else {
			*p++ = ' ';
		}
		if(ind == 0) {
			break;
		}
		ind--;
	}
	return p;
}

void trace_printf(int lvl, const char* name, const char* fmt, ...) {
	char newfmt[128];
	char* bufend = newfmt + sizeof(newfmt) - 2;
	char* p = newfmt;
	va_list argl;
	if(0 == (lvl & trace_lvl)) {
		return;
	}
	p = make_prefix(p, bufend, indent);
	p = copy_string(p, bufend, name);
	p = copy_string(p, bufend, ":");
	p = copy_string(p, bufend, fmt);
	*p++ = '\n';
	*p++ = 0;
#if DBG
	va_start(argl, fmt);
	VDBGPRINT(newfmt, argl);
	va_end(argl);
#endif
	va_start(argl, fmt);
	trace_vprintf(newfmt, argl);
	va_end(argl);
}

void enter_function(int lvl, const char* name, const char* fmt, ...) {
	char newfmt[128];
	char* bufend = newfmt + sizeof(newfmt) - 2;
	char* p = newfmt;
	va_list argl;
	int ind = InterlockedIncrement(&indent) - 1;
	if(0 == (lvl & trace_lvl_function)) {
		return;
	}
	p = make_prefix(p, bufend, ind);
	p = copy_string(p, bufend, name);
	p = copy_string(p, bufend, "(");
	p = copy_string(p, bufend, fmt);
	p = copy_string(p, bufend, "){");
	*p++ = '\n';
	*p++ = 0;
	InterlockedExchangePointer(&prevname, (PVOID)name);
#if DBG
	va_start(argl, fmt);
	VDBGPRINT(newfmt, argl);
	va_end(argl);
#endif
	va_start(argl, fmt);
	trace_vprintf(newfmt, argl);
	va_end(argl);
}

void leave_function(int lvl, const char* name, const char* fmt, ...) {
	char newfmt[128];
	char* bufend = newfmt + sizeof(newfmt) - 2;
	char* p = newfmt;
	va_list argl;
	int ind = InterlockedDecrement(&indent);
	if(0 == (lvl & trace_lvl_function)) {
		return;
	}
	p = make_prefix(p, bufend, ind);
	p = copy_string(p, bufend, "} ");
	if(name != InterlockedExchangePointer(&prevname, NULL)) {
		p = copy_string(p, bufend, name);
	}
	if(fmt[0]) {
		p = copy_string(p, bufend, "=");
		p = copy_string(p, bufend, fmt);
	}
	*p++ = '\n';
	*p++ = 0;
#if DBG
	va_start(argl, fmt);
	VDBGPRINT(newfmt, argl);
	va_end(argl);
#endif
	va_start(argl, fmt);
	trace_vprintf(newfmt, argl);
	va_end(argl);
}

static KSPIN_LOCK trace_lock;
static char* trace_buffer = NULL;
static int trace_buffer_begin;
static int trace_buffer_end;

void trace_start() {
	KeInitializeSpinLock(&trace_lock);
	if(trace_buffer_size == 0) {
		return;
	}
	trace_buffer = ExAllocatePoolWithTag(NonPagedPool, trace_buffer_size, 'VLTL');
	TRACE(LVL_SYSTEM, "trace level=%x", trace_lvl);
	TRACE(LVL_SYSTEM, "function trace level=%x", trace_lvl_function);
}

void trace_stop() {
	if(NULL == trace_buffer) {
		return;
	}
	ExFreePoolWithTag(trace_buffer, 'VLTL');
	trace_buffer = NULL;
}

static void print_char(char c) {
	if(NULL == trace_buffer) {
		return;
	}
	if(trace_buffer_size <= trace_buffer_end) {
		trace_buffer_end = 0;
	}
	trace_buffer[trace_buffer_end++] = c;
	if(trace_buffer_size <= trace_buffer_end) {
		trace_buffer_end = 0;
	}
	if(trace_buffer_end == trace_buffer_begin) {
		trace_buffer_begin++;
	}
	if(trace_buffer_size <= trace_buffer_begin) {
		trace_buffer_begin = 0;
	}
}

static void print_hex(int v) {
	static char* digit = "0123456789abcdef";
	char tmpbuf[8];
	int i;
	for(i = 0; i < 8;) {
		tmpbuf[i++] = digit[v & 15];
		v = (v >> 4);
		if(0 == v) {
			break;
		}
	}
	for(i--; 0 <= i; i--) {
		print_char(tmpbuf[i]);
	}
}

static void print_pointer(void* p) {
	static char* digit = "0123456789abcdef";
	char tmpbuf[16];
	ULONGLONG v;
	int i;
	if(sizeof(void*) == 4) {
		print_hex((int)p);
		return;
	}
	v = (ULONGLONG)p;
	for(i = 0; i < 16;) {
		tmpbuf[i++] = digit[v & 15];
		v = (v >> 4);
		if(0 == v) {
			break;
		}
	}
	for(i--; 0 <= i; i--) {
		print_char(tmpbuf[i]);
	}
}

static void print_dec(int v) {
	static char* digit = "0123456789abcdef";
	char tmpbuf[16];
	int i;
	if(v < 0) {
		v = -v;
		print_char('-');
	}
	for(i = 0; i < 16;) {
		tmpbuf[i++] = digit[v % 10];
		v = (v / 10);
		if(0 == v) {
			break;
		}
	}
	for(i--; 0 <= i; i--) {
		print_char(tmpbuf[i]);
	}
}

static void print_string(char* s) {
	int i;
	for(i = 0; s[i]; i++) {
		print_char(s[i]);
	}
}

void trace_vprintf(const char* fmt, va_list argl) {
	KIRQL OldIrql;
	if(NULL == trace_buffer) {
		return;
	}
	KeAcquireSpinLock(&trace_lock, &OldIrql);
	for(;;) {
		char c = *fmt++;
		if(0 == c) {
			break;
		}
		if('%' != c) {
			print_char(c);
			continue;
		}
		c = *fmt++;
		if('x' == c) {
			print_hex(va_arg(argl, int));
		} else if('d' == c) {
			print_dec(va_arg(argl, int));
		} else if('p' == c) {
			print_pointer(va_arg(argl, void*));
		} else if('s' == c) {
			print_string(va_arg(argl, char*));
		} else if('%' == c) {
			print_char(c);
		} else {
			print_char('%');
			print_char(c);
		}
	}
	KeReleaseSpinLock(&trace_lock, OldIrql);
}

int trace_read(char* dst, int length) {
	KIRQL OldIrql;
	int transfer;
	if(NULL == trace_buffer) {
		return 0;
	}
	KeAcquireSpinLock(&trace_lock, &OldIrql);
	transfer = trace_buffer_end - trace_buffer_begin;
	if(transfer < 0) {
		transfer = trace_buffer_size - trace_buffer_begin;
	}
	if(transfer > length) {
		transfer = length;
	}
	RtlCopyMemory(dst, trace_buffer + trace_buffer_begin, transfer);
	trace_buffer_begin += transfer;
	if(trace_buffer_size <= trace_buffer_begin) {
		trace_buffer_begin = 0;
	}
	KeReleaseSpinLock(&trace_lock, OldIrql);
	return transfer;
}
