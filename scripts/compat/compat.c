/* retrofox-compat.lib: desktop functions the phone lacks, built on ones it has.
   Each entry was confirmed MISSING on the Lumia by phone\probe\probe.exe.

   Code compiled against the desktop headers calls these through dllimport, i.e. via a
   pointer named __imp_<Function>. We define those pointers here (pointing at rfx_ versions),
   so callers link without knowing the function isn't really imported. */
#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>

#define RFX_IMPORT(name) void *__imp_##name = (void *)rfx_##name

/* ANSI name -> UTF-16 in a caller buffer; NULL stays NULL */
static LPCWSTR widen(LPCSTR s, WCHAR *buf, int len)
{
    if (!s)
        return NULL;
    if (!MultiByteToWideChar(CP_ACP, 0, s, -1, buf, len))
        return NULL;
    return buf;
}

static HANDLE WINAPI rfx_CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name)
{
    WCHAR buf[MAX_PATH];
    return CreateSemaphoreExW(sa, initial, maximum, widen(name, buf, MAX_PATH), 0, SEMAPHORE_ALL_ACCESS);
}
RFX_IMPORT(CreateSemaphoreA);

static HANDLE WINAPI rfx_OpenSemaphoreA(DWORD access, BOOL inherit, LPCSTR name)
{
    WCHAR buf[MAX_PATH];
    return OpenSemaphoreW(access, inherit, widen(name, buf, MAX_PATH));
}
RFX_IMPORT(OpenSemaphoreA);

static HANDLE WINAPI rfx_OpenFileMappingA(DWORD access, BOOL inherit, LPCSTR name)
{
    WCHAR buf[MAX_PATH];
    return OpenFileMappingW(access, inherit, widen(name, buf, MAX_PATH));
}
RFX_IMPORT(OpenFileMappingA);

static BOOL WINAPI rfx_GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD nameLen, LPDWORD serial,
                                             LPDWORD maxComponent, LPDWORD flags, LPSTR fsName,
                                             DWORD fsNameLen)
{
    WCHAR rootW[MAX_PATH], nameW[MAX_PATH + 1], fsW[MAX_PATH + 1];
    BOOL ok = GetVolumeInformationW(widen(root, rootW, MAX_PATH), name ? nameW : NULL,
                                    name ? MAX_PATH + 1 : 0, serial, maxComponent, flags,
                                    fsName ? fsW : NULL, fsName ? MAX_PATH + 1 : 0);
    if (ok && name)
        WideCharToMultiByte(CP_ACP, 0, nameW, -1, name, (int)nameLen, NULL, NULL);
    if (ok && fsName)
        WideCharToMultiByte(CP_ACP, 0, fsW, -1, fsName, (int)fsNameLen, NULL, NULL);
    return ok;
}
RFX_IMPORT(GetVolumeInformationA);

/* MSVC's compiler-only barrier: clang declares it in intrin0.h but has no ARM implementation.
   An opaque call already keeps the compiler from moving memory accesses across it. */
__declspec(noinline) void _ReadWriteBarrier(void)
{
    __asm__ __volatile__("" ::: "memory");
}

/* Console input and colors: the phone's console host has none of these (they are absent from its
   export lists). The CRT's console-input setup and NSS's command-line tools (password prompts,
   colored output) reference them; on the phone they fail as they would without a console. */
static BOOL WINAPI rfx_SetConsoleMode(HANDLE console, DWORD mode)
{
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
RFX_IMPORT(SetConsoleMode);

static BOOL WINAPI rfx_GetConsoleScreenBufferInfo(HANDLE console, PCONSOLE_SCREEN_BUFFER_INFO info)
{
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
RFX_IMPORT(GetConsoleScreenBufferInfo);

static BOOL WINAPI rfx_SetConsoleTextAttribute(HANDLE console, WORD attributes)
{
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
RFX_IMPORT(SetConsoleTextAttribute);

static BOOL WINAPI rfx_GetNumberOfConsoleInputEvents(HANDLE console, LPDWORD count)
{
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
RFX_IMPORT(GetNumberOfConsoleInputEvents);

static BOOL WINAPI rfx_PeekConsoleInputA(HANDLE console, PINPUT_RECORD records, DWORD length, LPDWORD read)
{
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
RFX_IMPORT(PeekConsoleInputA);

static BOOL WINAPI rfx_ReadConsoleInputW(HANDLE console, PINPUT_RECORD records, DWORD length, LPDWORD read)
{
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
RFX_IMPORT(ReadConsoleInputW);

/* No message boxes on the phone: send the text to the debugger and answer as if OK was pressed */
static int WINAPI rfx_MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type)
{
    if (caption) {
        OutputDebugStringW(caption);
        OutputDebugStringW(L": ");
    }
    if (text)
        OutputDebugStringW(text);
    OutputDebugStringW(L"\n");
    return IDOK;
}
RFX_IMPORT(MessageBoxW);

/* No window manager metrics on the phone. Answer like a single 480x800 screen with no scroll
   bars or window frames (widget/retrofox draws everything itself); anything else is 0. */
static int WINAPI rfx_GetSystemMetrics(int index)
{
    switch (index) {
    case SM_CXSCREEN:
    case SM_CXVIRTUALSCREEN:
    case SM_CXFULLSCREEN:
    case SM_CXMAXIMIZED:
        return 480;
    case SM_CYSCREEN:
    case SM_CYVIRTUALSCREEN:
    case SM_CYFULLSCREEN:
    case SM_CYMAXIMIZED:
        return 800;
    case SM_CMONITORS:
    case SM_SAMEDISPLAYFORMAT:
        return 1;
    case SM_CXDOUBLECLK:
    case SM_CYDOUBLECLK:
    case SM_CXDRAG:
    case SM_CYDRAG:
        return 4;
    default:
        return 0;
    }
}
RFX_IMPORT(GetSystemMetrics);

/* The phone only has the UTF-16 version; the CSD (service pack) text is the only string */
static BOOL WINAPI rfx_VerifyVersionInfoA(LPOSVERSIONINFOEXA info, DWORD typeMask,
                                          DWORDLONG conditionMask)
{
    OSVERSIONINFOEXW w;
    ZeroMemory(&w, sizeof(w));
    w.dwOSVersionInfoSize = sizeof(w);
    w.dwMajorVersion = info->dwMajorVersion;
    w.dwMinorVersion = info->dwMinorVersion;
    w.dwBuildNumber = info->dwBuildNumber;
    w.dwPlatformId = info->dwPlatformId;
    MultiByteToWideChar(CP_ACP, 0, info->szCSDVersion, -1, w.szCSDVersion,
                        sizeof(w.szCSDVersion) / sizeof(w.szCSDVersion[0]));
    w.wServicePackMajor = info->wServicePackMajor;
    w.wServicePackMinor = info->wServicePackMinor;
    w.wSuiteMask = info->wSuiteMask;
    w.wProductType = info->wProductType;
    return VerifyVersionInfoW(&w, typeMask, conditionMask);
}
RFX_IMPORT(VerifyVersionInfoA);

/* Device names are the only strings in these structures */
static MMRESULT WINAPI rfx_waveOutGetDevCapsA(UINT_PTR id, LPWAVEOUTCAPSA caps, UINT size)
{
    WAVEOUTCAPSW w;
    MMRESULT r;
    if (size < sizeof(*caps))
        return MMSYSERR_INVALPARAM;
    r = waveOutGetDevCapsW(id, &w, sizeof(w));
    if (r != MMSYSERR_NOERROR)
        return r;
    caps->wMid = w.wMid;
    caps->wPid = w.wPid;
    caps->vDriverVersion = w.vDriverVersion;
    WideCharToMultiByte(CP_ACP, 0, w.szPname, -1, caps->szPname, MAXPNAMELEN, NULL, NULL);
    caps->dwFormats = w.dwFormats;
    caps->wChannels = w.wChannels;
    caps->wReserved1 = w.wReserved1;
    caps->dwSupport = w.dwSupport;
    return MMSYSERR_NOERROR;
}
RFX_IMPORT(waveOutGetDevCapsA);

static MMRESULT WINAPI rfx_waveInGetDevCapsA(UINT_PTR id, LPWAVEINCAPSA caps, UINT size)
{
    WAVEINCAPSW w;
    MMRESULT r;
    if (size < sizeof(*caps))
        return MMSYSERR_INVALPARAM;
    r = waveInGetDevCapsW(id, &w, sizeof(w));
    if (r != MMSYSERR_NOERROR)
        return r;
    caps->wMid = w.wMid;
    caps->wPid = w.wPid;
    caps->vDriverVersion = w.vDriverVersion;
    WideCharToMultiByte(CP_ACP, 0, w.szPname, -1, caps->szPname, MAXPNAMELEN, NULL, NULL);
    caps->dwFormats = w.dwFormats;
    caps->wChannels = w.wChannels;
    caps->wReserved1 = w.wReserved1;
    return MMSYSERR_NOERROR;
}
RFX_IMPORT(waveInGetDevCapsA);

static UINT WINAPI rfx_GetTempFileNameA(LPCSTR path, LPCSTR prefix, UINT unique, LPSTR out)
{
    WCHAR pathW[MAX_PATH], prefixW[MAX_PATH], outW[MAX_PATH];
    UINT r = GetTempFileNameW(widen(path, pathW, MAX_PATH), widen(prefix, prefixW, MAX_PATH),
                              unique, outW);
    if (r && !WideCharToMultiByte(CP_ACP, 0, outW, -1, out, MAX_PATH, NULL, NULL))
        return 0;
    return r;
}
RFX_IMPORT(GetTempFileNameA);

/* No window messages on the phone, so waiting for "objects or input" is waiting for objects */
static DWORD WINAPI rfx_MsgWaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, DWORD ms,
                                                    DWORD wakeMask, DWORD flags)
{
    if (count == 0)
        return SleepEx(ms, (flags & MWMO_ALERTABLE) != 0) == WAIT_IO_COMPLETION
               ? WAIT_IO_COMPLETION : WAIT_TIMEOUT;
    return WaitForMultipleObjectsEx(count, handles, (flags & MWMO_WAITALL) != 0, ms,
                                    (flags & MWMO_ALERTABLE) != 0);
}
RFX_IMPORT(MsgWaitForMultipleObjectsEx);

static DWORD WINAPI rfx_MsgWaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll,
                                                  DWORD ms, DWORD wakeMask)
{
    return rfx_MsgWaitForMultipleObjectsEx(count, handles, ms, wakeMask,
                                           waitAll ? MWMO_WAITALL : 0);
}
RFX_IMPORT(MsgWaitForMultipleObjects);

/* The phone's COM only has the Ex form */
static HRESULT WINAPI rfx_CoInitialize(LPVOID reserved)
{
    return CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
}
RFX_IMPORT(CoInitialize);

/* The phone has only the Ex forms, in the API set. version.h doesn't declare these dllimport, so
   they are real functions here. */
DWORD WINAPI GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR file, LPDWORD handle);
BOOL WINAPI GetFileVersionInfoExW(DWORD flags, LPCWSTR file, DWORD handle, DWORD len, LPVOID data);

DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR file, LPDWORD handle)
{
    return GetFileVersionInfoSizeExW(0, file, handle);
}

BOOL WINAPI GetFileVersionInfoW(LPCWSTR file, DWORD handle, DWORD len, LPVOID data)
{
    return GetFileVersionInfoExW(0, file, handle, len, data);
}
