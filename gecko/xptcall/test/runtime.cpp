/* Tiny runtime for the xptcall test. Linux: freestanding (no libc), for
 * qemu-arm. Windows: on top of the CRT. */
#include <stdint.h>
#include <stddef.h>

int test_main();

#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
void out_str(const char* s) { fputs(s, stdout); }
extern "C" void* rt_alloc(uint32_t n) { return calloc(1, n); }
int main() { return test_main(); }
#else
static long sys3(long n, long a, long b, long c) {
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
    return r0;
}
void out_str(const char* s) { size_t n = 0; while (s[n]) n++; sys3(4, 1, (long)s, n); }
static char gHeap[1 << 20] __attribute__((aligned(16)));
static uint32_t gHeapUsed;
extern "C" void* rt_alloc(uint32_t n) {
    void* p = gHeap + gHeapUsed;
    gHeapUsed += (n + 15) & ~15u;
    for (uint32_t i = 0; i < n; i++) ((char*)p)[i] = 0;
    return p;
}
void* operator new[](size_t n) { return rt_alloc(n); }
void* operator new(size_t n) { return rt_alloc(n); }
void operator delete[](void*) noexcept {}
void operator delete(void*) noexcept {}
void operator delete(void*, size_t) noexcept {}
void operator delete[](void*, size_t) noexcept {}
extern "C" {
void __cxa_pure_virtual() { out_str("pure virtual\n"); sys3(1, 3, 0, 0); }
void* memcpy(void* d, const void* s, size_t n) { for (size_t i = 0; i < n; i++) ((char*)d)[i] = ((const char*)s)[i]; return d; }
void* memset(void* d, int c, size_t n) { for (size_t i = 0; i < n; i++) ((char*)d)[i] = (char)c; return d; }
void __aeabi_memcpy(void* d, const void* s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void* d, const void* s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void* d, const void* s, size_t n) { memcpy(d, s, n); }
void __aeabi_memclr(void* d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void* d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void* d, size_t n) { memset(d, 0, n); }
void __aeabi_memset(void* d, size_t n, int c) { memset(d, c, n); }
void rt_start_c() { sys3(1, test_main(), 0, 0); }
}
__asm__(".text\n.globl _start\n.type _start,%function\n.thumb_func\n_start:\n"
        "  mov r0, sp\n  bic r0, r0, #7\n  mov sp, r0\n  bl rt_start_c\n");
#endif

void out_hex(uint64_t v) {
    char b[19]; int i = 18; b[i] = 0;
    do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    b[--i] = 'x'; b[--i] = '0';
    out_str(b + i);
}
