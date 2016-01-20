#include <windows.h>
#include <stdio.h>

HANDLE hFile;
UCHAR Buffer[4096];
ULONG CmdBuffer[2];

void read_control(){
	for(;;) {
		DWORD Length=0;
		if(!ReadFile(hFile, Buffer, sizeof(Buffer), &Length, NULL)) {
			return;
		}
		if (Length == 0) {
			SleepEx(100, TRUE);
			continue;
		}
		WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), Buffer, Length, &Length, NULL);
	}
}

void write_control1(char* op) {
	DWORD Length=0;
	if(0==strcmp(op, "-stop")){
		CmdBuffer[0]=2;
		CmdBuffer[1]=1;
	}
	else{
		return;
	}
	WriteFile(hFile, CmdBuffer, sizeof(CmdBuffer), &Length, NULL);
}

void write_control2(char* op, char* v) {
	DWORD Length=0;
	int x=0;
	sscanf(v,"%x",&x);
	if(0==strcmp(op, "-trace")){
		CmdBuffer[0]=0;
		CmdBuffer[1]=x;
	}
	else if(0==strcmp(op, "-ftrace")){
		CmdBuffer[0]=1;
		CmdBuffer[1]=x;
	}
	else{
		return;
	}
	WriteFile(hFile, CmdBuffer, sizeof(CmdBuffer), &Length, NULL);
}

int __cdecl main(int argc, char** argv) {
	hFile = CreateFile("\\\\.\\FDLVLCTL_D7E51260_2063_4761_9933_416742467721", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if(hFile == INVALID_HANDLE_VALUE) {
		return -1;
	}
	if(argc==1){
		read_control();
	}
	if(argc==2){
		write_control1(argv[1]);
	}
	if(argc==3){
		write_control2(argv[1], argv[2]);
	}
	return 0;
}
