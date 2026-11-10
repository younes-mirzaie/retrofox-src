/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* retrofox: argument placement for Windows on ARM (32-bit, Thumb-2).
 *
 * Windows on ARM uses the AAPCS with the VFP ("hard float") variant:
 *   - core registers r0-r3 carry integer/pointer arguments; r0 is |this|,
 *   - 64-bit integers go in an even:odd register pair (r2:r3), or 8-byte
 *     aligned on the stack; once one has gone to the stack, no later
 *     argument may use a core register (no back-filling),
 *   - float/double go in s0-s15 / d0-d7, with back-filling of single
 *     precision holes; once a VFP argument has gone to the stack, no later
 *     VFP argument may use a VFP register,
 *   - stack arguments are 4-byte slots, 64-bit ones 8-byte aligned.
 *
 * Both NS_InvokeByIndex (writing arguments) and PrepareAndDispatch
 * (reading them) use this one allocator, so the two directions can't drift.
 */

#ifndef xptc_arm_win_abi_h___
#define xptc_arm_win_abi_h___

#include <stdint.h>

namespace xptc_arm {

enum ArgClass { kWord, kDword, kFloat, kDouble };

enum ArgWhere { kInGpr, kInVfp, kOnStack };

struct ArgSlot {
    ArgWhere where;
    uint32_t index;   // kInGpr: core register number (1..3, or 2 for a pair)
                      // kInVfp: single-precision register number (0..15)
                      // kOnStack: 32-bit word offset from the outgoing SP
};

class ArgAllocator {
public:
    ArgAllocator() : mNextGpr(1), mVfpUsed(0), mVfpDone(false), mStackWords(0) {}

    ArgSlot Next(ArgClass c) {
        ArgSlot s;
        switch (c) {
        case kWord:
            if (mNextGpr < 4) {
                s.where = kInGpr; s.index = mNextGpr++;
            } else {
                s.where = kOnStack; s.index = mStackWords++;
            }
            return s;
        case kDword:
            mNextGpr = (mNextGpr + 1) & ~1u;
            if (mNextGpr + 1 < 4) {
                s.where = kInGpr; s.index = mNextGpr; mNextGpr += 2;
            } else {
                mNextGpr = 4;
                mStackWords = (mStackWords + 1) & ~1u;
                s.where = kOnStack; s.index = mStackWords; mStackWords += 2;
            }
            return s;
        case kFloat:
            if (!mVfpDone) {
                for (uint32_t r = 0; r < 16; r++) {
                    if (!(mVfpUsed & (1u << r))) {
                        mVfpUsed |= 1u << r;
                        s.where = kInVfp; s.index = r;
                        return s;
                    }
                }
                mVfpDone = true;
            }
            s.where = kOnStack; s.index = mStackWords++;
            return s;
        case kDouble:
        default:
            if (!mVfpDone) {
                for (uint32_t r = 0; r < 16; r += 2) {
                    if (!(mVfpUsed & (3u << r))) {
                        mVfpUsed |= 3u << r;
                        s.where = kInVfp; s.index = r;
                        return s;
                    }
                }
                mVfpDone = true;
            }
            mStackWords = (mStackWords + 1) & ~1u;
            s.where = kOnStack; s.index = mStackWords; mStackWords += 2;
            return s;
        }
    }

    uint32_t StackWords() const { return mStackWords; }

private:
    uint32_t mNextGpr;
    uint32_t mVfpUsed;     // bit n = s<n> taken
    bool     mVfpDone;
    uint32_t mStackWords;
};

} // namespace xptc_arm

#endif /* xptc_arm_win_abi_h___ */
