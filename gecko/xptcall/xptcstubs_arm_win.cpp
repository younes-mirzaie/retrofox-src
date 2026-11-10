/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* retrofox: shared vtbl stubs, Windows on ARM (Thumb-2, AAPCS-VFP, MSVC C++
 * ABI, built with clang-cl). See xptc_arm_win_abi.h. */

#include "xptcprivate.h"
#include "xptiprivate.h"
#include "xptc_arm_win_abi.h"

#if !defined(_M_ARM) && !defined(XPTC_ARM_WIN_TEST)
#error "This file is for 32-bit Windows on ARM"
#endif
#if !defined(__clang__)
#error "Needs clang (GNU-style inline assembly, naked functions)"
#endif

using namespace xptc_arm;

/*
 * gpr   = the caller's r0-r3 (gpr[0] is |this|)
 * vfp   = the caller's d0-d7 (as s0-s15)
 * stack = the caller's stack arguments
 */
extern "C" nsresult
xptc_arm_win_PrepareAndDispatch(nsXPTCStubBase* self, uint32_t methodIndex,
                                const uint32_t* gpr, const uint32_t* vfp,
                                const uint32_t* stack)
{
#define PARAM_BUFFER_COUNT 16

    nsXPTCMiniVariant paramBuffer[PARAM_BUFFER_COUNT];
    nsXPTCMiniVariant* dispatchParams = nullptr;
    const nsXPTMethodInfo* info = nullptr;
    uint8_t paramCount;
    nsresult result = NS_ERROR_FAILURE;

    NS_ASSERTION(self, "no self");

    self->mEntry->GetMethodInfo(uint16_t(methodIndex), &info);
    NS_ASSERTION(info, "no method info");
    if (!info)
        return NS_ERROR_UNEXPECTED;

    paramCount = info->GetParamCount();

    if (paramCount > PARAM_BUFFER_COUNT)
        dispatchParams = new nsXPTCMiniVariant[paramCount];
    else
        dispatchParams = paramBuffer;

    ArgAllocator alloc;
    for (uint8_t i = 0; i < paramCount; i++) {
        const nsXPTParamInfo& param = info->GetParam(i);
        const nsXPTType& type = param.GetType();
        nsXPTCMiniVariant* dp = &dispatchParams[i];

        ArgClass cls = kWord;
        bool isPtr = param.IsOut() || !type.IsArithmetic();
        if (!isPtr) {
            switch (type) {
            case nsXPTType::T_I64:
            case nsXPTType::T_U64:    cls = kDword;  break;
            case nsXPTType::T_FLOAT:  cls = kFloat;  break;
            case nsXPTType::T_DOUBLE: cls = kDouble; break;
            default:                  cls = kWord;   break;
            }
        }

        ArgSlot slot = alloc.Next(cls);
        const uint32_t* src;
        switch (slot.where) {
        case kInGpr:  src = &gpr[slot.index];   break;
        case kInVfp:  src = &vfp[slot.index];   break;
        default:      src = &stack[slot.index]; break;
        }
        uint32_t w = src[0];

        if (isPtr) {
            dp->val.p = reinterpret_cast<void*>(uintptr_t(w));
            continue;
        }

        switch (type) {
        case nsXPTType::T_I8:    dp->val.i8  = int8_t(w);   break;
        case nsXPTType::T_I16:   dp->val.i16 = int16_t(w);  break;
        case nsXPTType::T_I32:   dp->val.i32 = int32_t(w);  break;
        case nsXPTType::T_U8:    dp->val.u8  = uint8_t(w);  break;
        case nsXPTType::T_U16:   dp->val.u16 = uint16_t(w); break;
        case nsXPTType::T_U32:   dp->val.u32 = w;           break;
        case nsXPTType::T_BOOL:  dp->val.b   = uint8_t(w) != 0; break;
        case nsXPTType::T_CHAR:  dp->val.c   = char(w);     break;
        case nsXPTType::T_WCHAR: dp->val.wc  = char16_t(w); break;
        case nsXPTType::T_I64:
        case nsXPTType::T_U64:
            dp->val.u64 = uint64_t(src[0]) | (uint64_t(src[1]) << 32);
            break;
        case nsXPTType::T_FLOAT:
            __builtin_memcpy(&dp->val.f, src, 4);
            break;
        case nsXPTType::T_DOUBLE:
            __builtin_memcpy(&dp->val.d, src, 8);
            break;
        default:
            NS_ERROR("bad type");
            break;
        }
    }

    result = self->mOuter->CallMethod(uint16_t(methodIndex), info, dispatchParams);

    if (dispatchParams != paramBuffer)
        delete [] dispatchParams;

    return result;
}

/*
 * The shared stub. Entered from StubN with ip = N and every argument
 * register untouched.
 *
 *   push {r0-r3}       caller's core argument registers   (sp+80..95 later)
 *   vpush {d0-d7}      caller's VFP argument registers     (sp+16..79 later)
 *   push {r11, lr}                                         (sp+8..15 later)
 *   sub sp, #8         5th C argument of PrepareAndDispatch (sp+0)
 * The caller's stack arguments then start at sp+96. Every step is a multiple
 * of 8 bytes, so SP stays 8-byte aligned. The .seh_ lines give the Windows
 * unwinder the same layout.
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
    ".def xptc_arm_win_SharedStub\n"
    ".scl 2\n"
    ".type 32\n"
    ".endef\n"
#else
    ".type xptc_arm_win_SharedStub, %function\n"
#endif
    ".globl xptc_arm_win_SharedStub\n"
    ".p2align 2\n"
    ".thumb_func\n"
    "xptc_arm_win_SharedStub:\n"
    SEH(".seh_proc xptc_arm_win_SharedStub")
    "    push    {r0, r1, r2, r3}\n"
    SEH(".seh_save_regs {r0-r3}")
    "    vpush   {d0-d7}\n"
    SEH(".seh_save_fregs {d0-d7}")
    "    push    {r11, lr}\n"
    SEH(".seh_save_regs_w {r11, lr}")
    "    mov     r11, sp\n"
    SEH(".seh_nop")
    "    sub     sp, sp, #8\n"
    SEH(".seh_stackalloc 8")
    SEH(".seh_endprologue")
    "    mov     r1, ip\n"                 // methodIndex
    "    add     r2, sp, #80\n"            // gpr (r0-r3)
    "    add     r3, sp, #16\n"            // vfp (d0-d7)
    "    add     ip, sp, #96\n"            // caller's stack arguments
    "    str     ip, [sp]\n"
    "    bl      xptc_arm_win_PrepareAndDispatch\n"   // r0 is still |self|
    "    add     sp, sp, #8\n"
    "    pop     {r11, lr}\n"
    "    add     sp, sp, #80\n"
    "    bx      lr\n"
    SEH(".seh_endproc")
);

/*
 * Each stub only loads its method number into ip (r12, the intra-procedure
 * scratch register, free to clobber at a call) and jumps to the shared stub.
 * Naked member functions let the compiler produce the MSVC-mangled names.
 */
#define STUB_ENTRY(n)                                           \
__attribute__((naked)) nsresult nsXPTCStubBase::Stub##n()      \
{                                                               \
    __asm__ volatile("mov.w   ip, #" #n "\n"                    \
                     "b.w     xptc_arm_win_SharedStub\n");      \
}

#define SENTINEL_ENTRY(n)                                       \
nsresult nsXPTCStubBase::Sentinel##n()                          \
{                                                               \
    NS_ERROR("nsXPTCStubBase::Sentinel called");                \
    return NS_ERROR_NOT_IMPLEMENTED;                            \
}

#include "xptcstubsdef.inc"
