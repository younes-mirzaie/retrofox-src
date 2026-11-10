/* Toolchain smoke test: proves clang-cl + the WP 8.1 libs produce a native ARM exe the Lumia can run. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

/* Exported by the phone's kernelbase but hidden from WINAPI_FAMILY_PHONE_APP headers */
WINBASEAPI LPVOID WINAPI VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
#ifndef PAGE_EXECUTE_READWRITE
#define PAGE_EXECUTE_READWRITE 0x40
#endif

int main(int argc, char **argv)
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);

    printf("hello from retrofox\n");
    printf("cpus: %lu, page size: %lu\n", si.dwNumberOfProcessors, si.dwPageSize);

    /* the JIT will need executable memory; see whether the unlock lets us have it */
    void *p = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    printf("VirtualAlloc(PAGE_EXECUTE_READWRITE): %s (error %lu)\n", p ? "ok" : "failed", p ? 0 : GetLastError());

    char *heap = malloc(64 * 1024 * 1024);
    printf("malloc 64 MB: %s\n", heap ? "ok" : "failed");
    free(heap);
    return 0;
}
