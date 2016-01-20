#include <windows.h>
#include <devioctl.h>
#include <ntdddisk.h>
#include <ntddscsi.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <strsafe.h>
#include <intsafe.h>
#include <commctrl.h>
#include <winioctl.h>
#include <wincrypt.h>
#include "dlgid.h"
#include "../lib/common.h"
#include "../lib/common.c"

HANDLE hFile;
CHAR DriveLetter;
CHAR StringBuffer[512];
CHAR Caption[512];

int max_size = 16000;
int mode;
int size;
int policy;
int timeout;
BOOL flush;

BOOL enable;
BOOL m_enable;
BOOL c_enable;
int c_size;
int c_policy;
int c_timeout;
BOOL c_flush;

BOOL open() {
	sprintf_s(StringBuffer, 16, "\\\\.\\%c:", DriveLetter);
	hFile = CreateFile(StringBuffer, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                   NULL, OPEN_EXISTING, 0, NULL);
	if(hFile == INVALID_HANDLE_VALUE) {
		return FALSE;
	}
	return TRUE;
}

#define SENSEINFOLENGTH (32)
#define DATABUFFEROFFSET (sizeof(SCSI_PASS_THROUGH) + SENSEINFOLENGTH)

BOOL read() {
	UCHAR SptBuffer[512];
	PSCSI_PASS_THROUGH Spt = (PSCSI_PASS_THROUGH)SptBuffer;
	PMODE_BUFFER ModeBuffer;
	DWORD Length = 0;
	DWORD BytesReturned = 0;
	ZeroMemory(Spt, sizeof(SptBuffer));
	Spt->Length = sizeof(SCSI_PASS_THROUGH);
	Spt->PathId = 0;
	Spt->TargetId = 0;
	Spt->Lun = 0;
	Spt->CdbLength = 6;
	Spt->SenseInfoLength = SENSEINFOLENGTH;
	Spt->DataIn = SCSI_IOCTL_DATA_IN;
	Spt->DataTransferLength = sizeof(MODE_BUFFER);
	Spt->TimeOutValue = 6;
	Spt->SenseInfoOffset = sizeof(SCSI_PASS_THROUGH);
	Spt->DataBufferOffset = DATABUFFEROFFSET;
	Spt->Cdb[0] = 0x1a;
	Spt->Cdb[1] = 8;
	Spt->Cdb[2] = 40;
	Spt->Cdb[3] = 41;
	Spt->Cdb[4] = (UCHAR)Spt->DataTransferLength;
	Length = DATABUFFEROFFSET + Spt->DataTransferLength;
	if(!DeviceIoControl(hFile, IOCTL_SCSI_PASS_THROUGH, Spt, sizeof(SCSI_PASS_THROUGH), Spt, Length,
	                    &BytesReturned, NULL)) {
		return FALSE;
	}
	if(BytesReturned < Length) {
		return FALSE;
	}
	ModeBuffer = (PMODE_BUFFER)(SptBuffer + Spt->DataBufferOffset);
	if(!VerifyChecksum128(ModeBuffer, sizeof(MODE_BUFFER))) {
		return FALSE;
	}
	if(ModeBuffer->Version != DRIVER_VERSION || ModeBuffer->Revision != DRIVER_REVISION) {
		return FALSE;
	}
	max_size = ModeBuffer->MAX_MAP / 256;
	mode = ModeBuffer->Mode;
	size = ModeBuffer->SIZE_OF_MAP / 256;
	timeout = ModeBuffer->CacheTimeout;
	flush = ModeBuffer->FlushFaithfully;
	if(ModeBuffer->WEIGHT_LIFE + ModeBuffer->WEIGHT_SPEED) {
		policy = 100 * ModeBuffer->WEIGHT_LIFE / (ModeBuffer->WEIGHT_LIFE + ModeBuffer->WEIGHT_SPEED);
	}
	if(policy < 0) {
		policy = 0;
	}
	if(policy > 100) {
		policy = 100;
	}
	return TRUE;
}

BOOL write() {
	UCHAR SptBuffer[512];
	PSCSI_PASS_THROUGH Spt = (PSCSI_PASS_THROUGH)SptBuffer;
	PMODE_BUFFER ModeBuffer;
	DWORD Length = 0;
	DWORD BytesReturned = 0;
	ZeroMemory(Spt, sizeof(SptBuffer));
	Spt->Length = sizeof(SCSI_PASS_THROUGH);
	Spt->PathId = 0;
	Spt->TargetId = 0;
	Spt->Lun = 0;
	Spt->CdbLength = 6;
	Spt->SenseInfoLength = SENSEINFOLENGTH;
	Spt->DataIn = SCSI_IOCTL_DATA_OUT;
	Spt->DataTransferLength = sizeof(MODE_BUFFER);
	Spt->TimeOutValue = 6;
	Spt->SenseInfoOffset = sizeof(SCSI_PASS_THROUGH);
	Spt->DataBufferOffset = DATABUFFEROFFSET;
	Spt->Cdb[0] = 0x15;
	Spt->Cdb[1] = 0;
	Spt->Cdb[2] = 0;
	Spt->Cdb[3] = 0;
	Spt->Cdb[4] = (UCHAR)Spt->DataTransferLength;
	ModeBuffer = (PMODE_BUFFER)(SptBuffer + DATABUFFEROFFSET);
	ModeBuffer->ModeDataLength = sizeof(MODE_BUFFER) - 1;
	ModeBuffer->MediumType = 0;
	ModeBuffer->Reserved = 0;
	ModeBuffer->BlockDescriptorLength = 0;
	ModeBuffer->PageCode = 40;
	ModeBuffer->PageLength = sizeof(MODE_BUFFER) - 6;
	ModeBuffer->Version = DRIVER_VERSION;
	ModeBuffer->Revision = DRIVER_REVISION;
	ModeBuffer->Mode = VLTL_MODE_FORMAT;
	ModeBuffer->SIZE_OF_MAP = c_enable ? c_size * 256 : 0;
	ModeBuffer->WEIGHT_LIFE = c_policy;
	ModeBuffer->WEIGHT_SPEED = 100 - c_policy;
	ModeBuffer->CacheTimeout = (UCHAR)c_timeout;
	ModeBuffer->FlushFaithfully = (UCHAR)c_flush;
	HCRYPTPROV hProv;
	if(CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
		CryptGenRandom(hProv, sizeof(ULONG), (BYTE*)&ModeBuffer->Signature);
		CryptGenRandom(hProv, sizeof(ULONG), (BYTE*)&ModeBuffer->Magic);
	}
	ProduceChecksum128(ModeBuffer, sizeof(MODE_BUFFER));
	Length = DATABUFFEROFFSET + Spt->DataTransferLength;
	if(!DeviceIoControl(hFile, IOCTL_SCSI_PASS_THROUGH, Spt, Length, Spt, sizeof(SCSI_PASS_THROUGH),
	                    &BytesReturned, NULL)) {
		return FALSE;
	}
	return TRUE;
}

BOOL write_config() {
	UCHAR SptBuffer[512];
	PSCSI_PASS_THROUGH Spt = (PSCSI_PASS_THROUGH)SptBuffer;
	PMODE_BUFFER ModeBuffer;
	DWORD Length = 0;
	DWORD BytesReturned = 0;
	ZeroMemory(Spt, sizeof(SptBuffer));
	Spt->Length = sizeof(SCSI_PASS_THROUGH);
	Spt->PathId = 0;
	Spt->TargetId = 0;
	Spt->Lun = 0;
	Spt->CdbLength = 6;
	Spt->SenseInfoLength = SENSEINFOLENGTH;
	Spt->DataIn = SCSI_IOCTL_DATA_OUT;
	Spt->DataTransferLength = sizeof(MODE_BUFFER);
	Spt->TimeOutValue = 6;
	Spt->SenseInfoOffset = sizeof(SCSI_PASS_THROUGH);
	Spt->DataBufferOffset = DATABUFFEROFFSET;
	Spt->Cdb[0] = 0x15;
	Spt->Cdb[1] = 0;
	Spt->Cdb[2] = 0;
	Spt->Cdb[3] = 0;
	Spt->Cdb[4] = (UCHAR)Spt->DataTransferLength;
	ModeBuffer = (PMODE_BUFFER)(SptBuffer + DATABUFFEROFFSET);
	ModeBuffer->ModeDataLength = sizeof(MODE_BUFFER) - 1;
	ModeBuffer->MediumType = 0;
	ModeBuffer->Reserved = 0;
	ModeBuffer->BlockDescriptorLength = 0;
	ModeBuffer->PageCode = 40;
	ModeBuffer->PageLength = sizeof(MODE_BUFFER) - 6;
	ModeBuffer->Version = DRIVER_VERSION;
	ModeBuffer->Revision = DRIVER_REVISION;
	ModeBuffer->Mode = VLTL_MODE_CONFIG;
	ModeBuffer->SIZE_OF_MAP = c_size * 256;
	ModeBuffer->WEIGHT_LIFE = c_policy;
	ModeBuffer->WEIGHT_SPEED = 100 - c_policy;
	ModeBuffer->CacheTimeout = (UCHAR)c_timeout;
	ModeBuffer->FlushFaithfully = (UCHAR)c_flush;
	ProduceChecksum128(ModeBuffer, sizeof(MODE_BUFFER));
	Length = DATABUFFEROFFSET + Spt->DataTransferLength;
	if(!DeviceIoControl(hFile, IOCTL_SCSI_PASS_THROUGH, Spt, Length, Spt, sizeof(SCSI_PASS_THROUGH),
	                    &BytesReturned, NULL)) {
		return FALSE;
	}
	return TRUE;
}

VOID init(HWND hWnd) {
	SetWindowText(hWnd, Caption);
	USHORT state;
	switch(mode) {
	case VLTL_MODE_DIRECT:
		state = DLGID_STATE_INACTIVE;
		enable = FALSE;
		m_enable = TRUE;
		break;
	case VLTL_MODE_ACTIVE:
		state = DLGID_STATE_ACTIVE;
		enable = TRUE;
		m_enable = TRUE;
		break;
	case VLTL_MODE_INITIAL:
		state = DLGID_STATE_NOMEDIA;
		enable = FALSE;
		m_enable = FALSE;
		break;
	case VLTL_MODE_ERROR:
		state = DLGID_STATE_ERROR;
		enable = TRUE;
		m_enable = TRUE;
		break;
	default:
		state = DLGID_STATE_PREPARING;
		enable = TRUE;
		m_enable = FALSE;
		break;
	}
	c_enable = enable;
	c_size = size;
	c_policy = policy;
	c_timeout = timeout;
	c_flush = flush;
	LoadString(NULL, state, StringBuffer, 32);
	SetDlgItemText(hWnd, DLGID_STATE_TEXT, StringBuffer);
}

VOID update(HWND hWnd) {
	BOOL Translated;
	if(c_size != GetDlgItemInt(hWnd, DLGID_SIZE_TEXT, &Translated, FALSE) || !Translated) {
		SetDlgItemInt(hWnd, DLGID_SIZE_TEXT, c_size, FALSE);
	}
	if(c_policy != SendMessage(GetDlgItem(hWnd, DLGID_POLICY_SLIDER), TBM_GETPOS, 0, 0)) {
		SendMessage(GetDlgItem(hWnd, DLGID_POLICY_SLIDER), TBM_SETPOS, TRUE, c_policy);
	}
	if(c_enable != IsDlgButtonChecked(hWnd, DLGID_MASTER_CHECKBOX)) {
		CheckDlgButton(hWnd, DLGID_MASTER_CHECKBOX, c_enable ? BST_CHECKED : BST_UNCHECKED);
	}
	if(c_timeout == 0) {
		WCHAR Text[8];
		GetDlgItemTextW(hWnd, DLGID_CACHE_TEXT, Text, 8);
		if(wcscmp(Text, L"‡")) {
			SetDlgItemTextW(hWnd, DLGID_CACHE_TEXT, L"‡");
		}
	} else if(c_timeout != GetDlgItemInt(hWnd, DLGID_CACHE_TEXT, &Translated, FALSE) || !Translated) {
		SetDlgItemInt(hWnd, DLGID_CACHE_TEXT, c_timeout, FALSE);
	}
	if(c_flush != IsDlgButtonChecked(hWnd, DLGID_FLUSH_CHECKBOX)) {
		CheckDlgButton(hWnd, DLGID_FLUSH_CHECKBOX, c_flush ? BST_CHECKED : BST_UNCHECKED);
	}
	EnableWindow(GetDlgItem(hWnd, DLGID_MASTER_CHECKBOX), m_enable);
	EnableWindow(GetDlgItem(hWnd, DLGID_SIZE_LABEL), c_enable && m_enable);
	EnableWindow(GetDlgItem(hWnd, DLGID_SIZE_TEXT), c_enable && m_enable);
	EnableWindow(GetDlgItem(hWnd, DLGID_SIZE_TRAILER), c_enable && m_enable);
	EnableWindow(GetDlgItem(hWnd, DLGID_POLICY_LABEL), c_enable && m_enable);
	EnableWindow(GetDlgItem(hWnd, DLGID_POLICY_LOW), c_enable && m_enable);
	EnableWindow(GetDlgItem(hWnd, DLGID_POLICY_HIGH), c_enable && m_enable);
	EnableWindow(GetDlgItem(hWnd, DLGID_POLICY_SLIDER), c_enable && m_enable);
	EnableWindow(GetDlgItem(hWnd, DLGID_CACHE_LABEL), c_enable && m_enable);
	EnableWindow(GetDlgItem(hWnd, DLGID_CACHE_TEXT), c_enable && m_enable);
	EnableWindow(GetDlgItem(hWnd, DLGID_CACHE_TRAILER), c_enable && m_enable);
	EnableWindow(GetDlgItem(hWnd, DLGID_FLUSH_CHECKBOX), c_enable && m_enable);
	EnableWindow(GetDlgItem(hWnd, DLGID_APPLY), c_enable != enable || (enable && (c_size != size
	             || c_policy != policy || c_timeout != timeout || c_flush != flush)));
}

VOID apply(HWND hWnd) {
	if(c_enable == enable && enable && c_size == size) {
		if(!write_config()) {
			LoadString(NULL, DLGID_MSG_CANNOT_PROCESS, StringBuffer, 32);
			MessageBox(NULL, StringBuffer, Caption, MB_OK | MB_ICONERROR | MB_APPLMODAL);
			ExitProcess(1);
		}
		goto finish;
	}
	DWORD BytesReturned = 0;
	if(!DeviceIoControl(hFile, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &BytesReturned, NULL)) {
		LoadString(NULL, DLGID_MSG_WARN_USING, StringBuffer, 128);
		if(IDOK != MessageBox(hWnd, StringBuffer, Caption,
		                      MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_APPLMODAL)) {
			DeviceIoControl(hFile, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &BytesReturned, NULL);
			return;
		}
	}
	LoadString(NULL, DLGID_MSG_WARN_ERASE, StringBuffer, 128);
	if(IDOK != MessageBox(hWnd, StringBuffer, Caption,
	                      MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_APPLMODAL)) {
		DeviceIoControl(hFile, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &BytesReturned, NULL);
		return;
	}
	DeviceIoControl(hFile, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &BytesReturned, NULL);
	if(!write()) {
		LoadString(NULL, DLGID_MSG_CANNOT_PROCESS, StringBuffer, 32);
		MessageBox(NULL, StringBuffer, Caption, MB_OK | MB_ICONERROR | MB_APPLMODAL);
		ExitProcess(1);
	}
	DeviceIoControl(hFile, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &BytesReturned, NULL);
finish:
	/*
	LoadString(NULL, DLGID_MSG_SUCCESS, StringBuffer, 32);
	MessageBox(NULL, StringBuffer, Caption, MB_OK | MB_APPLMODAL);
	*/
	ExitProcess(0);
}

LRESULT CALLBACK DialogProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	BOOL Translated;
	switch(uMsg) {
	case WM_CLOSE:
	case WM_DESTROY:
		EndDialog(hWnd, 0);
		return TRUE;
	case WM_INITDIALOG:
		init(hWnd);
		update(hWnd);
		SetFocus(GetDlgItem(hWnd, DLGID_CLOSE));
		return FALSE;
	case WM_COMMAND:
		switch(LOWORD(wParam)) {
		case DLGID_CLOSE:
			if(HIWORD(wParam) == BN_CLICKED) {
				EndDialog(hWnd, 0);
				return TRUE;
			}
			break;
		case DLGID_APPLY:
			if(HIWORD(wParam) == BN_CLICKED) {
				apply(hWnd);
				return TRUE;
			}
			break;
		case DLGID_MASTER_CHECKBOX:
			c_enable = IsDlgButtonChecked(hWnd, DLGID_MASTER_CHECKBOX);
			update(hWnd);
			return TRUE;
		case DLGID_SIZE_TEXT:
			c_size = GetDlgItemInt(hWnd, DLGID_SIZE_TEXT, &Translated, FALSE);
			if(c_size > max_size) {
				c_size = max_size;
			}
			if(Translated && c_size == 0) {
				c_size = 1;
			}
			update(hWnd);
			return TRUE;
		case DLGID_CACHE_TEXT:
			c_timeout = GetDlgItemInt(hWnd, DLGID_CACHE_TEXT, &Translated, FALSE);
			if(c_timeout > 120) {
				c_timeout = 120;
			}
			update(hWnd);
			return TRUE;
		case DLGID_FLUSH_CHECKBOX:
			c_flush = IsDlgButtonChecked(hWnd, DLGID_FLUSH_CHECKBOX);
			update(hWnd);
			return TRUE;
		}
		break;
	case WM_HSCROLL:
		if((HWND)lParam == GetDlgItem(hWnd, DLGID_POLICY_SLIDER)) {
			c_policy = (int)SendMessage((HWND)lParam, TBM_GETPOS, 0, 0);
			update(hWnd);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	/*
	AllocConsole();
	freopen("CONOUT$", "w", stdout);
	freopen("CONIN$", "r", stdin);
	*/
	DriveLetter = lpCmdLine[0];
	LoadString(NULL, DLGID_MSG_CAPTION, StringBuffer, 32);
	sprintf_s(Caption, 32, StringBuffer, DriveLetter);
	if(!open() || !read()) {
		LoadString(NULL, DLGID_MSG_CANNOT_PROCESS, StringBuffer, 32);
		MessageBox(NULL, StringBuffer, Caption, MB_OK | MB_ICONERROR | MB_APPLMODAL);
		ExitProcess(1);
	}
	InitCommonControls();
	DialogBox(hInstance, MAKEINTRESOURCE(DLGID_PARENT), NULL, (DLGPROC)DialogProc);
	return 0;
}
