#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

HANDLE hFile;
UCHAR Buffer[512*400];
ULONG Written[100000];
ULONG Checked;

ULONG random1(ULONG limit){
	return (rand()&0x7fffffff)%limit;
}

void read1(ULONG limit, ULONG scale){
	ULONG lba,len;
	LARGE_INTEGER Dist;
	ULONG i,j,Length;
	lba=(rand()&0x7fffffff)%limit;
	len=(rand()&0x7fffffff)%scale;
	len*=(rand()&0x7fffffff)%scale;
	len+=(rand()&0x7fffffff)%scale;
	Dist.QuadPart=lba;
	Dist.QuadPart*=512;
	Length=len*512;
	if(Length>sizeof(Buffer)){
		Length=sizeof(Buffer);
	}
	if(!SetFilePointerEx(hFile,Dist, 0, FILE_BEGIN)){
		ExitProcess(5);
	}
	if(!ReadFile(hFile, Buffer, Length, &Length, NULL)) {
		ExitProcess(6);
	}
	for(i=0;i<Length&&lba<100000;i+=512){
		PULONG b=(PVOID)(Buffer+i);
		ULONG x1=b[0];
		ULONG x2=0;
		if(Written[lba]==0){
			continue;
		}
		if(Written[lba]!=b[0]){
			ExitProcess(8);
		}
		lba++;
		for(j=0;j<128;j++){
			if(b[j]!=x1){
				ExitProcess(7);
			}
			x1=x2+(x1+j)*(x1+j);
			x2=b[j];
		}
		Checked++;
	}
}

void write1(ULONG limit, ULONG scale){
	ULONG lba,len;
	LARGE_INTEGER Dist;
	ULONG i,j,Length;
	lba=(rand()&0x7fffffff)%limit;
	len=(rand()&0x7fffffff)%scale;
	len*=(rand()&0x7fffffff)%scale;
	len+=(rand()&0x7fffffff)%scale;
	Dist.QuadPart=lba;
	Dist.QuadPart*=512;
	Length=len*512;
	if(Length>sizeof(Buffer)){
		Length=sizeof(Buffer);
	}
	for(i=0;i<Length;i+=512){
		ULONG x1=rand();
		ULONG x2=0;
		PULONG b=(PVOID)(Buffer+i);
		for(j=0;j<128;j++){
			b[j]=x1;
			x1=x2+(x1+j)*(x1+j);
			x2=b[j];
		}
	}
	if(!SetFilePointerEx(hFile,Dist, 0, FILE_BEGIN)){
		ExitProcess(2);
	}
	if(!WriteFile(hFile, Buffer, Length, &Length, NULL)){
		ExitProcess(3);
	}
	if(Length&511){
		ExitProcess(4);
	}
	for(i=0;i<Length;i+=512){
		PULONG b=(PVOID)(Buffer+i);
		if(lba<100000){
			Written[lba++]=b[0];
		}
	}
}

int __cdecl main(int argc, char** argv) {
	ULONG lba;
	ULONG i,len;
	hFile = CreateFile("test1.dat", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_FLAG_NO_BUFFERING, NULL);
	if(hFile == INVALID_HANDLE_VALUE) {
		ExitProcess(1);
	}
	srand(GetTickCount());
	for(i=0;;i++){
		switch(random1(10)){
		case 0: write1(10,3);break;
		case 1: write1(100,6);break;
		case 2: write1(1000,10);break;
		case 3: write1(10000,20);break;
		case 4: write1(80000,20);break;
		case 5: read1(100,5);break;
		default: read1(10000,20);break;
		}
		if(0==(i&0xff)){
			printf("Checked=%d\n",Checked);
		}
	}
}
