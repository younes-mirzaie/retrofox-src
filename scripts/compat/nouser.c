/* Desktop windowing, DDE and shell UI, which the phone doesn't have: there are no Win32 windows,
   message queues or Explorer (verified with phone\probe\probe.exe). widget/retrofox draws
   offscreen, and the phone app shows the frames and sends input, so Gecko's window and shell code
   paths shouldn't run. If one does, each function fails the way Windows reports failure.

   As in nogdi.c, one function per failure value serves any signature on 32-bit ARM. */
#include <windows.h>
#include <ddeml.h>

#define RFX_FAIL(name, fn) void *__imp_##name = (void *)fn

/* 0, NULL or FALSE */
static UINT_PTR WINAPI rfx_user_fail(void)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

static HRESULT WINAPI rfx_shell_fail(void)
{
    return E_NOTIMPL;
}

/* Windows, window classes and properties */
RFX_FAIL(CreateWindowExW, rfx_user_fail);
RFX_FAIL(DestroyWindow, rfx_user_fail);
RFX_FAIL(RegisterClassW, rfx_user_fail);
RFX_FAIL(RegisterClassExW, rfx_user_fail);
RFX_FAIL(UnregisterClassW, rfx_user_fail);
RFX_FAIL(DefWindowProcW, rfx_user_fail);
RFX_FAIL(CallWindowProcW, rfx_user_fail);
RFX_FAIL(GetWindowLongW, rfx_user_fail);
RFX_FAIL(SetWindowLongW, rfx_user_fail);
RFX_FAIL(GetClassNameW, rfx_user_fail);
RFX_FAIL(GetPropW, rfx_user_fail);
RFX_FAIL(SetPropW, rfx_user_fail);
RFX_FAIL(RemovePropW, rfx_user_fail);
RFX_FAIL(IsWindow, rfx_user_fail);
RFX_FAIL(IsIconic, rfx_user_fail);
RFX_FAIL(ShowWindow, rfx_user_fail);
RFX_FAIL(UpdateWindow, rfx_user_fail);
RFX_FAIL(SetWindowPos, rfx_user_fail);
RFX_FAIL(SetForegroundWindow, rfx_user_fail);
RFX_FAIL(FindWindowW, rfx_user_fail);
RFX_FAIL(FindWindowExW, rfx_user_fail);
RFX_FAIL(EnumThreadWindows, rfx_user_fail);
RFX_FAIL(GetWindowThreadProcessId, rfx_user_fail);
RFX_FAIL(InvalidateRect, rfx_user_fail);
RFX_FAIL(ValidateRect, rfx_user_fail);
RFX_FAIL(RedrawWindow, rfx_user_fail);
RFX_FAIL(GetUpdateRect, rfx_user_fail);

/* Messages, timers and hooks: the queue is always empty */
RFX_FAIL(PeekMessageW, rfx_user_fail);
RFX_FAIL(TranslateMessage, rfx_user_fail);
RFX_FAIL(DispatchMessageW, rfx_user_fail);
RFX_FAIL(PostMessageW, rfx_user_fail);
RFX_FAIL(SendMessageW, rfx_user_fail);
RFX_FAIL(PostQuitMessage, rfx_user_fail);
RFX_FAIL(GetQueueStatus, rfx_user_fail);
RFX_FAIL(GetKeyState, rfx_user_fail);          /* no key down */
RFX_FAIL(SetTimer, rfx_user_fail);
RFX_FAIL(KillTimer, rfx_user_fail);
RFX_FAIL(SetWindowsHookExW, rfx_user_fail);
RFX_FAIL(UnhookWindowsHookEx, rfx_user_fail);
RFX_FAIL(CallNextHookEx, rfx_user_fail);
RFX_FAIL(SetWinEventHook, rfx_user_fail);

/* Shell UI */
RFX_FAIL(ShellExecuteExW, rfx_user_fail);
RFX_FAIL(SHGetPathFromIDListW, rfx_user_fail);
RFX_FAIL(ILCreateFromPathW, rfx_user_fail);
RFX_FAIL(SHAddToRecentDocs, rfx_user_fail);
RFX_FAIL(SHGetSpecialFolderLocation, rfx_shell_fail);
RFX_FAIL(SHParseDisplayName, rfx_shell_fail);
RFX_FAIL(SHOpenFolderAndSelectItems, rfx_shell_fail);
RFX_FAIL(SHGetStockIconInfo, rfx_shell_fail);
RFX_FAIL(SHQueryUserNotificationState, rfx_shell_fail);
RFX_FAIL(DwmFlush, rfx_shell_fail);

/* Returns 0 on success, so it needs a real error */
static int WINAPI rfx_SHFileOperationW(void)
{
    return ERROR_NOT_SUPPORTED;
}
RFX_FAIL(SHFileOperationW, rfx_SHFileOperationW);

/* Account and process odds and ends */
RFX_FAIL(GetUserNameW, rfx_user_fail);
RFX_FAIL(SetProcessWorkingSetSize, rfx_user_fail);

/* DDE (desktop single-instance handoff) is linked by name. DdeInitializeW returns 0 on success,
   so it reports DMLERR_DLL_NOT_INITIALIZED and nothing else gets called. */
UINT WINAPI DdeInitializeW(LPDWORD inst, PFNCALLBACK callback, DWORD cmd, DWORD reserved)
{
    return DMLERR_DLL_NOT_INITIALIZED;
}
BOOL WINAPI DdeUninitialize(DWORD inst) { return FALSE; }
HDDEDATA WINAPI DdeNameService(DWORD inst, HSZ hsz1, HSZ hsz2, UINT cmd) { return NULL; }
HSZ WINAPI DdeCreateStringHandleA(DWORD inst, LPCSTR str, int codePage) { return NULL; }
BOOL WINAPI DdeFreeStringHandle(DWORD inst, HSZ hsz) { return FALSE; }
DWORD WINAPI DdeQueryStringW(DWORD inst, HSZ hsz, LPWSTR out, DWORD max, int codePage) { return 0; }
int WINAPI DdeCmpStringHandles(HSZ hsz1, HSZ hsz2) { return hsz1 == hsz2 ? 0 : 1; }
HDDEDATA WINAPI DdeCreateDataHandle(DWORD inst, LPBYTE src, DWORD len, DWORD off, HSZ item,
                                    UINT fmt, UINT cmd)
{
    return NULL;
}
LPBYTE WINAPI DdeAccessData(HDDEDATA data, LPDWORD size) { return NULL; }
BOOL WINAPI DdeUnaccessData(HDDEDATA data) { return FALSE; }
