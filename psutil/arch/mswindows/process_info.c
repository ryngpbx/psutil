/*
 * $Id$
 *
 * Helper functions related to fetching process information. Used by
 * _psutil_mswindows module methods.
 */

#include <Python.h>
#include <windows.h>
#include <Psapi.h>
#include <tlhelp32.h>

#include "security.h"
#include "process_info.h"
#include "ntextapi.h"

/*
 * NtQueryInformationProcess code taken from
 * http://wj32.wordpress.com/2009/01/24/howto-get-the-command-line-of-processes/
 * typedefs needed to compile against ntdll functions not exposted in the API
 */
typedef LONG NTSTATUS;

typedef NTSTATUS (NTAPI *_NtQueryInformationProcess)(
    HANDLE ProcessHandle,
    DWORD ProcessInformationClass,
    PVOID ProcessInformation,
    DWORD ProcessInformationLength,
    PDWORD ReturnLength
    );

typedef struct _PROCESS_BASIC_INFORMATION
{
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
} PROCESS_BASIC_INFORMATION, *PPROCESS_BASIC_INFORMATION;


/*
 * A wrapper around OpenProcess setting NSP exception if process
 * no longer exists.
 * "pid" is the process pid, "dwDesiredAccess" is the first argument
 * exptected by OpenProcess.
 * Return a process handle or NULL.
 */
HANDLE
handle_from_pid_waccess(DWORD pid, DWORD dwDesiredAccess)
{
    HANDLE hProcess;
    DWORD  processExitCode = 0;

    hProcess = OpenProcess(dwDesiredAccess, FALSE, pid);
    if (hProcess == NULL) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) {
            NoSuchProcess();
        }
        else {
            PyErr_SetFromWindowsErr(0);
        }
        return NULL;
    }

    /* make sure the process is running */
    GetExitCodeProcess(hProcess, &processExitCode);
    if (processExitCode == 0) {
        NoSuchProcess();
        CloseHandle(hProcess);
        return NULL;
    }
    return hProcess;
}


/*
 * Same as handle_from_pid_waccess but implicitly uses
 * PROCESS_QUERY_INFORMATION | PROCESS_VM_READ as dwDesiredAccess
 * parameter for OpenProcess.
 */
HANDLE
handle_from_pid(DWORD pid) {
    DWORD dwDesiredAccess = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
    return handle_from_pid_waccess(pid, dwDesiredAccess);
}


// fetch the PEB base address from NtQueryInformationProcess()
PVOID
GetPebAddress(HANDLE ProcessHandle)
{
    _NtQueryInformationProcess NtQueryInformationProcess =
        (_NtQueryInformationProcess)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    PROCESS_BASIC_INFORMATION pbi;

    NtQueryInformationProcess(ProcessHandle, 0, &pbi, sizeof(pbi), NULL);
    return pbi.PebBaseAddress;
}


DWORD*
get_pids(DWORD *numberOfReturnedPIDs) {
    int procArraySz = 1024;

    /* Win32 SDK says the only way to know if our process array
     * wasn't large enough is to check the returned size and make
     * sure that it doesn't match the size of the array.
     * If it does we allocate a larger array and try again*/

    /* Stores the actual array */
    DWORD *procArray = NULL;
    DWORD procArrayByteSz;

    /* Stores the byte size of the returned array from enumprocesses */
    DWORD enumReturnSz = 0;

    do {
        free(procArray);
        procArrayByteSz = procArraySz * sizeof(DWORD);
        procArray = malloc(procArrayByteSz);

        if (! EnumProcesses(procArray, procArrayByteSz, &enumReturnSz)) {
            free(procArray);
            PyErr_SetFromWindowsErr(0);
            return NULL;
        }
        else if (enumReturnSz == procArrayByteSz) {
            /* Process list was too large.  Allocate more space*/
            procArraySz += 1024;
        }

        /* else we have a good list */

    } while(enumReturnSz == procArraySz * sizeof(DWORD));

    /* The number of elements is the returned size / size of each element */
    *numberOfReturnedPIDs = enumReturnSz / sizeof(DWORD);

    return procArray;
}


int
pid_is_running(DWORD pid)
{
    HANDLE hProcess;
    DWORD exitCode;

    // Special case for PID 0 System Idle Process
    if (pid == 0) {
        return 1;
    }

    if (pid < 0) {
        return 0;
    }

    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                           FALSE, pid);
    if (NULL == hProcess) {
        // invalid parameter is no such process
        if (GetLastError() == ERROR_INVALID_PARAMETER) {
            CloseHandle(hProcess);
            return 0;
        }

        // access denied obviously means there's a process to deny access to...
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            CloseHandle(hProcess);
            return 1;
        }

        CloseHandle(hProcess);
        PyErr_SetFromWindowsErr(0);
        return -1;
    }

    if (GetExitCodeProcess(hProcess, &exitCode)) {
        CloseHandle(hProcess);
        return (exitCode == STILL_ACTIVE);
    }

    // access denied means there's a process there so we'll assume it's running
    if (GetLastError() == ERROR_ACCESS_DENIED) {
        CloseHandle(hProcess);
        return 1;
    }

    PyErr_SetFromWindowsErr(0);
    CloseHandle(hProcess);
    return -1;
}


int
pid_in_proclist(DWORD pid)
{
    DWORD *proclist = NULL;
    DWORD numberOfReturnedPIDs;
    DWORD i;

    proclist = get_pids(&numberOfReturnedPIDs);
    if (NULL == proclist) {
        return -1;
    }

    for (i = 0; i < numberOfReturnedPIDs; i++) {
        if (pid == proclist[i]) {
            free(proclist);
            return 1;
        }
    }

    free(proclist);
    return 0;
}


// Check exit code from a process handle. Return FALSE on an error also
BOOL is_running(HANDLE hProcess)
{
    DWORD dwCode;

    if (NULL == hProcess) {
        return FALSE;
    }

    if (GetExitCodeProcess(hProcess, &dwCode)) {
        return (dwCode == STILL_ACTIVE);
    }
    return FALSE;
}


// Return None to represent NoSuchProcess, else return NULL for
// other exception or the name as a Python string
PyObject*
get_name(long pid)
{
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = { 0 };
    pe.dwSize = sizeof(PROCESSENTRY32);

    if( Process32First(h, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                CloseHandle(h);
                return Py_BuildValue("s", pe.szExeFile);
            }
        } while(Process32Next(h, &pe));

        // the process was never found, set NoSuchProcess exception
        NoSuchProcess();
        CloseHandle(h);
        return NULL;
    }

    CloseHandle(h);
    return PyErr_SetFromWindowsErr(0);
}


/* returns parent pid (as a Python int) for given pid or None on failure */
PyObject*
get_ppid(long pid)
{
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = { 0 };
    pe.dwSize = sizeof(PROCESSENTRY32);

    if( Process32First(h, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                ////printf("PID: %i; PPID: %i\n", pid, pe.th32ParentProcessID);
                CloseHandle(h);
                return Py_BuildValue("I", pe.th32ParentProcessID);
            }
        } while(Process32Next(h, &pe));

        // the process was never found, set NoSuchProcess exception
        NoSuchProcess();
        CloseHandle(h);
        return NULL;
    }

    CloseHandle(h);
    return PyErr_SetFromWindowsErr(0);
}



/*
 * returns a Python list representing the arguments for the process
 * with given pid or NULL on error.
 */
PyObject*
get_arg_list(long pid)
{
    int nArgs, i;
    LPWSTR *szArglist;
    HANDLE hProcess;
    PVOID pebAddress;
    PVOID rtlUserProcParamsAddress;
    UNICODE_STRING commandLine;
    WCHAR *commandLineContents;
    PyObject *arg = NULL;
    PyObject *arg_from_wchar = NULL;
    PyObject *argList = NULL;

    hProcess = handle_from_pid(pid);
    if(hProcess == NULL) {
        return NULL;
    }

    pebAddress = GetPebAddress(hProcess);

    /* get the address of ProcessParameters */
#ifdef _WIN64
    if (!ReadProcessMemory(hProcess, (PCHAR)pebAddress + 32,
        &rtlUserProcParamsAddress, sizeof(PVOID), NULL))
#else
    if (!ReadProcessMemory(hProcess, (PCHAR)pebAddress + 0x10,
        &rtlUserProcParamsAddress, sizeof(PVOID), NULL))
#endif
    {
        ////printf("Could not read the address of ProcessParameters!\n");
        PyErr_SetFromWindowsErr(0);
        CloseHandle(hProcess);
        return NULL;
    }

    /* read the CommandLine UNICODE_STRING structure */
#ifdef _WIN64
    if (!ReadProcessMemory(hProcess, (PCHAR)rtlUserProcParamsAddress + 112,
        &commandLine, sizeof(commandLine), NULL))
#else
    if (!ReadProcessMemory(hProcess, (PCHAR)rtlUserProcParamsAddress + 0x40,
        &commandLine, sizeof(commandLine), NULL))
#endif
    {
        ////printf("Could not read CommandLine!\n");
        CloseHandle(hProcess);
        PyErr_SetFromWindowsErr(0);
        return NULL;
    }


    /* allocate memory to hold the command line */
    commandLineContents = (WCHAR *)malloc(commandLine.Length+1);

    /* read the command line */
    if (!ReadProcessMemory(hProcess, commandLine.Buffer,
        commandLineContents, commandLine.Length, NULL))
    {
        ////printf("Could not read the command line string!\n");
        CloseHandle(hProcess);
        PyErr_SetFromWindowsErr(0);
        free(commandLineContents);
        return NULL;
    }

    /* print the commandline */
    ////printf("%.*S\n", commandLine.Length / 2, commandLineContents);

    // null-terminate the string to prevent wcslen from returning incorrect length
    // the length specifier is in characters, but commandLine.Length is in bytes
    commandLineContents[(commandLine.Length/sizeof(WCHAR))] = '\0';

    // attemempt tp parse the command line using Win32 API, fall back on string
    // cmdline version otherwise
    szArglist = CommandLineToArgvW(commandLineContents, &nArgs);
    if (NULL == szArglist) {
        // failed to parse arglist
        // encode as a UTF8 Python string object from WCHAR string
        arg_from_wchar = PyUnicode_FromWideChar(commandLineContents,
                                                commandLine.Length / 2);
        #if PY_MAJOR_VERSION >= 3
            argList = Py_BuildValue("N", PyUnicode_AsUTF8String(arg_from_wchar));
        #else
            argList = Py_BuildValue("N", PyUnicode_FromObject(arg_from_wchar));
        #endif
    }
    else {
        // arglist parsed as array of UNICODE_STRING, so convert each to Python
        // string object and add to arg list
        argList = Py_BuildValue("[]");
        for(i=0; i<nArgs; i++) {
            ////printf("%d: %.*S (%d characters)\n", i, wcslen(szArglist[i]),
            //                  szArglist[i], wcslen(szArglist[i]));
            arg_from_wchar = PyUnicode_FromWideChar(szArglist[i],
                                                    wcslen(szArglist[i])
                                                    );
            #if PY_MAJOR_VERSION >= 3
                arg = PyUnicode_FromObject(arg_from_wchar);
            #else
                arg = PyUnicode_AsUTF8String(arg_from_wchar);
            #endif
            Py_XDECREF(arg_from_wchar);
            PyList_Append(argList, arg);
            Py_XDECREF(arg);
        }
    }

    LocalFree(szArglist);
    free(commandLineContents);
    CloseHandle(hProcess);
    return argList;
}


#define PH_FIRST_PROCESS(Processes) ((PSYSTEM_PROCESS_INFORMATION)(Processes))

#define PH_NEXT_PROCESS(Process) ( \
    ((PSYSTEM_PROCESS_INFORMATION)(Process))->NextEntryOffset ? \
    (PSYSTEM_PROCESS_INFORMATION)((PCHAR)(Process) + \
    ((PSYSTEM_PROCESS_INFORMATION)(Process))->NextEntryOffset) : \
    NULL \
    )

const STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;
const STATUS_BUFFER_TOO_SMALL = 0xC0000023L;

/*
 * Given a process PID and a PSYSTEM_PROCESS_INFORMATION structure
 * fills the structure with process information.
 * On success return 1, else 0 with Python exception already set.
 */
int
get_process_info(DWORD pid, PSYSTEM_PROCESS_INFORMATION *retProcess, PVOID *retBuffer)
{
    static ULONG initialBufferSize = 0x4000;
    NTSTATUS status;
    PVOID buffer;
    ULONG bufferSize;
    PSYSTEM_PROCESS_INFORMATION process;

    // get NtQuerySystemInformation
    typedef DWORD (_stdcall *NTQSI_PROC) (int, PVOID, ULONG, PULONG);
    NTQSI_PROC NtQuerySystemInformation;
    HINSTANCE hNtDll;
    hNtDll = LoadLibrary(TEXT("ntdll.dll"));
    NtQuerySystemInformation = (NTQSI_PROC)GetProcAddress(
                                hNtDll, "NtQuerySystemInformation");

    bufferSize = initialBufferSize;
    buffer = malloc(bufferSize);

    while (TRUE) {
        status = NtQuerySystemInformation(SystemProcessInformation, buffer,
                                          bufferSize, &bufferSize);

        if (status == STATUS_BUFFER_TOO_SMALL || status == STATUS_INFO_LENGTH_MISMATCH)
        {
            free(buffer);
            buffer = malloc(bufferSize);
        }
        else {
            break;
        }
    }

    if (status != 0) {
        PyErr_Format(PyExc_RuntimeError, "NtQuerySystemInformation() failed");
        return 0;
    }

    if (bufferSize <= 0x20000) {
        initialBufferSize = bufferSize;
    }

    process = PH_FIRST_PROCESS(buffer);
    do {
        if (process->UniqueProcessId == (HANDLE)pid) {
            *retProcess = process;
            *retBuffer = buffer;
            return 1;
        }
    } while ( (process = PH_NEXT_PROCESS(process)) );

    NoSuchProcess();
    return 0;
}


