/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* retrofox: invoke XPCOM methods on native objects, Windows on ARM (Thumb-2,
 * AAPCS-VFP, MSVC C++ ABI, built with clang-cl). See xptc_arm_win_abi.h. */

#include "xptcprivate.h"
#include "xptc_arm_win_abi.h"

#if !defined(_M_ARM) && !defined(XPTC_ARM_WIN_TEST)
#error "This file is for 32-bit Windows on ARM"
#endif
#if !defined(__clang__)
#error "Needs clang (GNU-style inline assembly)"
#endif

using namespace xptc_arm;

// Calls |func(that, ...)| with r1-r3 = gpr[1..3], d0-d7 = vfp[0..7] and
// |stackWords| words from |stack| at the outgoing SP. Defined below.
extern "C" uint32_t
xptc_arm_win_invoke(void* func, nsISupports* that, const uint32_t* gpr,
                    const uint64_t* vfp, const uint32_t* stack,
                    uint32_t stackWords);

static void
PutWord(const ArgSlot& s, uint32_t* gpr, float* vfp, uint32_t* stack, uint32_t v)
{
    switch (s.where) {
    case kInGpr:  gpr[s.index] = v; break;
    case kInVfp:  reinterpret_cast<uint32_t*>(vfp)[s.index] = v; break;
    case kOnStack: stack[s.index] = v; break;
    }
}

static void
PutDword(const ArgSlot& s, uint32_t* gpr, float* vfp, uint32_t* stack, uint64_t v)
{
    uint32_t lo = uint32_t(v), hi = uint32_t(v >> 32);
    switch (s.where) {
    case kInGpr:   gpr[s.index] = lo; gpr[s.index + 1] = hi; break;
    case kInVfp: {
        uint32_t* w = reinterpret_cast<uint32_t*>(vfp);
        w[s.index] = lo; w[s.index + 1] = hi;
        break;
    }
    case kOnStack: stack[s.index] = lo; stack[s.index + 1] = hi; break;
    }
}

EXPORT_XPCOM_API(nsresult)
NS_InvokeByIndex(nsISupports* that, uint32_t methodIndex,
                 uint32_t paramCount, nsXPTCVariant* params)
{
    // Worst case each parameter takes two stack words plus one alignment word.
    const uint32_t kInline = 64;
    uint32_t stackInline[kInline];
    uint32_t needed = paramCount * 3 + 1;
    uint32_t* stack = needed <= kInline ? stackInline : new uint32_t[needed];

    uint32_t gpr[4] = { 0, 0, 0, 0 };
    uint64_t vfpRegs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    float* vfp = reinterpret_cast<float*>(vfpRegs);

    ArgAllocator alloc;
    nsXPTCVariant* s = params;
    for (uint32_t i = 0; i < paramCount; i++, s++) {
        if (s->IsPtrData()) {
            PutWord(alloc.Next(kWord), gpr, vfp, stack, uint32_t(uintptr_t(s->ptr)));
            continue;
        }
        // AAPCS: integral types narrower than a word are sign/zero-extended.
        switch (s->type) {
        case nsXPTType::T_I8:   PutWord(alloc.Next(kWord), gpr, vfp, stack, uint32_t(int32_t(s->val.i8)));  break;
        case nsXPTType::T_I16:  PutWord(alloc.Next(kWord), gpr, vfp, stack, uint32_t(int32_t(s->val.i16))); break;
        case nsXPTType::T_I32:  PutWord(alloc.Next(kWord), gpr, vfp, stack, uint32_t(s->val.i32));          break;
        case nsXPTType::T_U8:   PutWord(alloc.Next(kWord), gpr, vfp, stack, s->val.u8);                     break;
        case nsXPTType::T_U16:  PutWord(alloc.Next(kWord), gpr, vfp, stack, s->val.u16);                    break;
        case nsXPTType::T_U32:  PutWord(alloc.Next(kWord), gpr, vfp, stack, s->val.u32);                    break;
        case nsXPTType::T_BOOL: PutWord(alloc.Next(kWord), gpr, vfp, stack, s->val.b ? 1 : 0);              break;
        case nsXPTType::T_CHAR: PutWord(alloc.Next(kWord), gpr, vfp, stack, uint32_t(int32_t(s->val.c)));   break;
        case nsXPTType::T_WCHAR: PutWord(alloc.Next(kWord), gpr, vfp, stack, uint32_t(s->val.wc));          break;
        case nsXPTType::T_I64:  PutDword(alloc.Next(kDword), gpr, vfp, stack, uint64_t(s->val.i64));        break;
        case nsXPTType::T_U64:  PutDword(alloc.Next(kDword), gpr, vfp, stack, s->val.u64);                  break;
        case nsXPTType::T_FLOAT: {
            uint32_t bits;
            __builtin_memcpy(&bits, &s->val.f, 4);
            PutWord(alloc.Next(kFloat), gpr, vfp, stack, bits);
            break;
        }
        case nsXPTType::T_DOUBLE: {
            uint64_t bits;
            __builtin_memcpy(&bits, &s->val.d, 8);
            PutDword(alloc.Next(kDouble), gpr, vfp, stack, bits);
            break;
        }
        default:
            // all the others are plain pointer types
            PutWord(alloc.Next(kWord), gpr, vfp, stack, uint32_t(uintptr_t(s->val.p)));
            break;
        }
    }

    void** vtable = *reinterpret_cast<void***>(that);
    uint32_t result = xptc_arm_win_invoke(vtable[methodIndex], that, gpr,
                                          vfpRegs, stack, alloc.StackWords());
    if (stack != stackInline)
        delete [] stack;
    return nsresult(result);
}

/*
 * r0 = func, r1 = that, r2 = gpr, r3 = vfp, [sp] = stack, [sp+4] = stackWords
 *
 * Keeps SP 8-byte aligned at the call (AAPCS). The outgoing area is at most
 * 255 * 8 + 4 bytes, under one 4 KB page, so no __chkstk probe is needed.
 * r11 points at the saved {r11, lr} pair, as Windows on ARM expects, and r4
 * keeps the SP from the end of the prologue (the .seh_save_sp unwind code),
 * so the Windows unwinder can walk through this frame.
 */
#ifdef _WIN32
#define SEH(x) x "\n"
#else
#define SEH(x)
#endif
__asm__(
    ".text\n"
    ".syntax unified\n"
    ".thumb\n"
#ifdef _WIN32
    ".def xptc_arm_win_invoke\n"
    ".scl 2\n"
    ".type 32\n"
    ".endef\n"
#else
    ".type xptc_arm_win_invoke, %function\n"
#endif
    ".globl xptc_arm_win_invoke\n"
    ".p2align 2\n"
    ".thumb_func\n"
    "xptc_arm_win_invoke:\n"
    SEH(".seh_proc xptc_arm_win_invoke")
    "    push    {r4, r5, r6, r7, r11, lr}\n"
    SEH(".seh_save_regs_w {r4-r7, r11, lr}")
    "    add     r11, sp, #16\n"
    SEH(".seh_nop_w")
    "    mov     r4, sp\n"
    SEH(".seh_save_sp r4")
    SEH(".seh_endprologue")
    "    ldr     r5, [r4, #24]\n"         // stack
    "    ldr     r12, [r4, #28]\n"        // stackWords
    "    mov     r6, r0\n"                // func
    "    mov     r7, r1\n"                // that
    "    vldmia  r3, {d0-d7}\n"
    "    lsl     r0, r12, #2\n"
    "    adds    r0, r0, #7\n"
    "    bic     r0, r0, #7\n"
    "    sub     sp, sp, r0\n"
    "    mov     r0, sp\n"
    "    cmp     r12, #0\n"
    "    beq     2f\n"
    "1:  ldr     r1, [r5], #4\n"
    "    str     r1, [r0], #4\n"
    "    subs    r12, r12, #1\n"
    "    bne     1b\n"
    "2:  ldr     r1, [r2, #4]\n"
    "    ldr     r3, [r2, #12]\n"
    "    ldr     r2, [r2, #8]\n"
    "    mov     r0, r7\n"
    "    blx     r6\n"
    "    mov     sp, r4\n"                // r0 holds the result
    "    pop     {r4, r5, r6, r7, r11, pc}\n"
    SEH(".seh_endproc")
);
