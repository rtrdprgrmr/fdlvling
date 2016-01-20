#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

HANDLE hFile;
UCHAR Buffer[4096];
ULONG MaxIndex;
ULONG Seq;

ULONG random1(ULONG limit){
	return (rand()&0x7fffffff)%limit;
}

ULONGLONG getfrequency(){
	LARGE_INTEGER t;
	QueryPerformanceFrequency(&t);
	return t.QuadPart;
}

ULONGLONG gettime(){
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

void prepare(){
	ULONG index;
	for(index=0;;index++){
		ULONG x1=Seq++;
		ULONG x2=index;
		PULONG b=(PULONG)Buffer;
		ULONG j;
		for(j=0;j<1024;j++){
			b[j]=x1;
			x1=x2+(x1+j)*(x1+j);
			x2=b[j];
		}
		ULONG Length;
		if(!WriteFile(hFile, Buffer, sizeof(Buffer), &Length, NULL)){
			break;
		}
		if(Length!=sizeof(Buffer)){
			break;
		}
	}
	MaxIndex=index;
}

void read1(){
	ULONG index=random1(MaxIndex);
	LARGE_INTEGER Dist;
	Dist.QuadPart=index;
	Dist.QuadPart*=sizeof(Buffer);
	if(!SetFilePointerEx(hFile,Dist, 0, FILE_BEGIN)){
		printf("error=%d\n",__LINE__);
		ExitProcess(2);
	}
	ULONG Length;
	if(!ReadFile(hFile, Buffer, sizeof(Buffer), &Length, NULL)) {
		printf("error=%d\n",__LINE__);
		ExitProcess(3);
	}
	if(Length!=sizeof(Buffer)){
		printf("error=%d\n",__LINE__);
		ExitProcess(4);
	}
	PULONG b=(PULONG)Buffer;
	ULONG x1=b[0];
	ULONG x2=index;
	ULONG j;
	for(j=0;j<1024;j++){
		if(b[j]!=x1){
			printf("error=%d\n",__LINE__);
			ExitProcess(5);
		}
		x1=x2+(x1+j)*(x1+j);
		x2=b[j];
	}
}

void write1(){
	ULONG index=random1(MaxIndex);
	PULONG b=(PULONG)Buffer;
	ULONG x1=Seq++;
	ULONG x2=index;
	ULONG j;
	for(j=0;j<1024;j++){
		b[j]=x1;
		x1=x2+(x1+j)*(x1+j);
		x2=b[j];
	}
	LARGE_INTEGER Dist;
	Dist.QuadPart=index;
	Dist.QuadPart*=sizeof(Buffer);
	if(!SetFilePointerEx(hFile,Dist, 0, FILE_BEGIN)){
		printf("error=%d\n",__LINE__);
		ExitProcess(2);
	}
	ULONG Length;
	if(!WriteFile(hFile, Buffer, sizeof(Buffer), &Length, NULL)) {
		printf("error=%d\n",__LINE__);
		ExitProcess(3);
	}
	if(Length!=sizeof(Buffer)){
		printf("error=%d\n",__LINE__);
		ExitProcess(4);
	}
}

void step(){
	ULONGLONG avg_r=0,max_r=0,min_r=-1;
	ULONGLONG avg_w=0,max_w=0,min_w=-1;
	ULONGLONG start,elapse;
	ULONG i;
	for(i=0;i<1024*64;i++){
		start= gettime();
		read1();
		elapse= gettime()-start;
		if(max_r<elapse)max_r=elapse;
		if(min_r>elapse)min_r=elapse;
		avg_r+=elapse;
		start= gettime();
		write1();
		elapse= gettime()-start;
		if(max_w<elapse)max_w=elapse;
		if(min_w>elapse)min_w=elapse;
		avg_w+=elapse;
	}
	ULONGLONG frequency= getfrequency();
	max_r=max_r*1000000/frequency;
	min_r=min_r*1000000/frequency;
	avg_r=avg_r*1000000/frequency/i;
	printf("R:min=%d avg=%d max=%d\n",min_r,avg_r,max_r);
	max_w=max_w*1000000/frequency;
	min_w=min_w*1000000/frequency;
	avg_w=avg_w*1000000/frequency/i;
	printf("W:min=%d avg=%d max=%d\n",min_w,avg_w,max_w);
}

int __cdecl main(int argc, char** argv) {
	hFile = CreateFile("test2.dat", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_FLAG_NO_BUFFERING, NULL);
	if(hFile == INVALID_HANDLE_VALUE) {
		ExitProcess(1);
	}
	srand(GetTickCount());
	//prepare();
	MaxIndex=2990000;
	printf("MaxIndex=%d\n",MaxIndex);
	ULONG i;
	for(i=0;i<500;i++){
		step();
	}
	printf("FINISHED\n");
	return 0;
}
