/* Runs on the phone: for each function name given, prints which phone DLL exports it.
   Usage: probe.exe Func1 Func2 ...          (searches the DLL list below)
          probe.exe -d other.dll Func1 ...   (adds a DLL to the front of the search)
   Output lines: "Func1 KERNELBASE.DLL" or "Func1 MISSING". */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static const wchar_t *kDlls[] = {
    L"KERNELBASE.DLL", L"KERNEL32LEGACY.DLL", L"ADVAPI32LEGACY.DLL", L"SECHOST.DLL",
    L"WS2_32.DLL", L"MSWSOCK.DLL", L"NTDLL.DLL", L"WINMM.DLL", L"WINMMBASE.DLL",
    L"USER32LEGACY.DLL", L"GDI32LEGACY.DLL", L"OLE32.DLL", L"COMBASE.DLL",
    L"SHLWAPI.DLL", L"SHELL32.DLL", L"PSAPI.DLL", L"VERSION.DLL", L"CRYPT32.DLL",
    L"IPHLPAPI.DLL", L"USERENV.DLL", L"DBGHELP.DLL", L"SSPICLI.DLL", L"BCRYPT.DLL",
};

int main(int argc, char **argv)
{
    HMODULE mods[64];
    const wchar_t *names[64];
    int n = 0, first = 1;

    if (argc > 2 && strcmp(argv[1], "-d") == 0) {
        static wchar_t extra[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, extra, MAX_PATH);
        names[n] = extra;
        mods[n++] = LoadLibraryExW(extra, NULL, 0);
        first = 3;
    }
    for (int i = 0; i < (int)(sizeof(kDlls) / sizeof(kDlls[0])); i++) {
        names[n] = kDlls[i];
        mods[n++] = LoadLibraryExW(kDlls[i], NULL, 0);
    }

    for (int a = first; a < argc; a++) {
        const wchar_t *found = NULL;
        for (int i = 0; i < n && !found; i++) {
            if (mods[i] && GetProcAddress(mods[i], argv[a]))
                found = names[i];
        }
        if (found)
            printf("%s %ls\n", argv[a], found);
        else
            printf("%s MISSING\n", argv[a]);
    }
    return 0;
}
