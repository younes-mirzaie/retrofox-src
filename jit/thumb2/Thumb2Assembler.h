/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* retrofox: a Thumb-2 (T32) instruction encoder for the SpiderMonkey JIT port.
 *
 * Windows on ARM runs Thumb-2 only, but SpiderMonkey's ARM backend emits A32
 * ("ARM mode") instructions. This is the encoding layer the port needs, kept
 * free of SpiderMonkey types so it can be tested on its own (test/).
 *
 * Choices (see notes/jit-thumb2-plan.md):
 *  - 32-bit (".W") encodings everywhere except IT, BX/BLX reg and NOP16, so
 *    sizes are predictable and every branch/immediate is patchable in place.
 *  - 32-bit constants by MOVW/MOVT pairs (patchable), not constant pools.
 *  - Conditional non-branch instructions need an IT block: It() + the insn.
 *  - VFP encodings are the A32 ones with cond = 1110, as two halfwords.
 * Every encoding is checked against llvm-mc by test/check-encodings.sh. */

#ifndef rfx_Thumb2Assembler_h
#define rfx_Thumb2Assembler_h

#include <stdint.h>
#include <stddef.h>


namespace rfx {
namespace thumb2 {

// A minimal growable array, so this header needs no C++ library (the JIT
// port will swap in SpiderMonkey's AssemblerBuffer).
template <typename T>
class Vec {
public:
    Vec() {}
    Vec(const Vec&) = delete;
    Vec& operator=(const Vec&) = delete;
    ~Vec() { delete[] mData; }
    void push_back(T v) {
        if (mSize == mCap) {
            size_t cap = mCap ? mCap * 2 : 64;
            T* n = new T[cap];
            for (size_t i = 0; i < mSize; i++) n[i] = mData[i];
            delete[] mData;
            mData = n;
            mCap = cap;
        }
        mData[mSize++] = v;
    }
    size_t size() const { return mSize; }
    void clear() { mSize = 0; }
    T* data() { return mData; }
    const T* data() const { return mData; }
    T& operator[](size_t i) { return mData[i]; }
    const T& operator[](size_t i) const { return mData[i]; }
    T* begin() { return mData; }
    T* end() { return mData + mSize; }
private:
    T* mData = nullptr;
    size_t mSize = 0, mCap = 0;
};

enum Reg : uint8_t {
    r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, sp, lr, pc,
    ip = 12, fp = 11
};

enum Cond : uint8_t {
    EQ = 0, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL,
    HS = CS, LO = CC
};

static inline Cond Invert(Cond c) { return Cond(c ^ 1); }

enum ShiftType : uint8_t { LSL = 0, LSR = 1, ASR = 2, ROR = 3 };

// Data-processing opcodes shared by the immediate and shifted-register forms.
enum DPOp : uint8_t {
    OpAND = 0x0, OpBIC = 0x1, OpORR = 0x2, OpORN = 0x3, OpEOR = 0x4,
    OpADD = 0x8, OpADC = 0xA, OpSBC = 0xB, OpSUB = 0xD, OpRSB = 0xE
};

enum SetCC : bool { LeaveCC = false, SetFlags = true };

// VFP registers: d0-d31, s0-s31.
struct DReg { uint8_t code; };
struct SReg { uint8_t code; };
static inline DReg d(uint8_t n) { return DReg{ n }; }
static inline SReg s(uint8_t n) { return SReg{ n }; }

// Addressing for LDR/STR immediate forms.
enum IndexMode : uint8_t { Offset, PreIndex, PostIndex };

// "Modified immediate" constants of T32 data-processing instructions.
// Returns true and the 12-bit i:imm3:imm8 field if |v| can be encoded.
static inline bool EncodeModifiedImm(uint32_t v, uint32_t* imm12)
{
    if (v <= 0xFF) { *imm12 = v; return true; }
    uint32_t b0 = v & 0xFF;
    if (v == (b0 | (b0 << 16))) { *imm12 = 0x100 | b0; return true; }
    uint32_t b1 = (v >> 8) & 0xFF;
    if (v == ((b1 << 8) | (b1 << 24))) { *imm12 = 0x200 | b1; return true; }
    if (v == b0 * 0x01010101u) { *imm12 = 0x300 | b0; return true; }
    // 1bcdefgh rotated right by 8..31
    for (uint32_t rot = 8; rot < 32; rot++) {
        uint32_t unrot = (v << rot) | (v >> (32 - rot));   // rotate left = undo ROR
        if (unrot >= 0x80 && unrot <= 0xFF) {
            *imm12 = (rot << 7) | (unrot & 0x7F);
            return true;
        }
    }
    return false;
}

static inline uint32_t DecodeModifiedImm(uint32_t imm12)
{
    uint32_t b = imm12 & 0xFF;
    if ((imm12 >> 10) == 0) {
        switch ((imm12 >> 8) & 3) {
        case 0: return b;
        case 1: return b | (b << 16);
        case 2: return (b << 8) | (b << 24);
        default: return b * 0x01010101u;
        }
    }
    uint32_t unrot = 0x80 | (imm12 & 0x7F);
    uint32_t rot = imm12 >> 7;
    return (unrot >> rot) | (unrot << (32 - rot));
}

// A position in the code that branches can target before it is known.
struct Label {
    int32_t offset = -1;              // bound position, or -1
    Vec<int32_t> uses;        // branch instructions waiting for it
    bool bound() const { return offset >= 0; }
};

class Assembler
{
public:
    Vec<uint8_t> code;
    bool ok = true;                   // false once something couldn't be encoded

    size_t size() const { return code.size(); }

    // ---- raw emission ----
    void Emit16(uint16_t h) { code.push_back(uint8_t(h)); code.push_back(uint8_t(h >> 8)); }
    void Emit32(uint16_t hw1, uint16_t hw2) { Emit16(hw1); Emit16(hw2); }
    void EmitA32VFP(uint32_t a32) {   // VFP: A32 encoding with cond 1110
        uint32_t w = (a32 & 0x0FFFFFFF) | 0xE0000000;
        Emit32(uint16_t(w >> 16), uint16_t(w));
    }
    static uint32_t Read32(const uint8_t* p) {
        uint16_t hw1 = uint16_t(p[0] | (p[1] << 8)), hw2 = uint16_t(p[2] | (p[3] << 8));
        return (uint32_t(hw1) << 16) | hw2;
    }
    static void Write32(uint8_t* p, uint32_t insn) {
        p[0] = uint8_t(insn >> 16); p[1] = uint8_t(insn >> 24);
        p[2] = uint8_t(insn); p[3] = uint8_t(insn >> 8);
    }

    // ---- data processing ----

    // op rd, rn, #imm (modified immediate). Returns false if |imm| doesn't fit.
    bool DataImm(DPOp op, Reg rd, Reg rn, uint32_t imm, SetCC sc = LeaveCC) {
        uint32_t imm12;
        if (!EncodeModifiedImm(imm, &imm12)) return false;
        Emit32(uint16_t(0xF000 | ((imm12 >> 11) & 1) << 10 | op << 5 | sc << 4 | rn),
               uint16_t(((imm12 >> 8) & 7) << 12 | rd << 8 | (imm12 & 0xFF)));
        return true;
    }
    // op rd, rn, rm {, shift #amount}
    void DataReg(DPOp op, Reg rd, Reg rn, Reg rm, ShiftType st = LSL, uint32_t amount = 0,
                 SetCC sc = LeaveCC) {
        uint32_t imm5 = amount & 31;   // LSR/ASR #32 are encoded as 0
        Emit32(uint16_t(0xEA00 | op << 5 | sc << 4 | rn),
               uint16_t((imm5 >> 2) << 12 | rd << 8 | (imm5 & 3) << 6 | st << 4 | rm));
    }

    bool add(Reg rd, Reg rn, uint32_t imm, SetCC sc = LeaveCC) { return DataImm(OpADD, rd, rn, imm, sc); }
    bool sub(Reg rd, Reg rn, uint32_t imm, SetCC sc = LeaveCC) { return DataImm(OpSUB, rd, rn, imm, sc); }
    bool rsb(Reg rd, Reg rn, uint32_t imm, SetCC sc = LeaveCC) { return DataImm(OpRSB, rd, rn, imm, sc); }
    bool and_(Reg rd, Reg rn, uint32_t imm, SetCC sc = LeaveCC) { return DataImm(OpAND, rd, rn, imm, sc); }
    bool orr(Reg rd, Reg rn, uint32_t imm, SetCC sc = LeaveCC) { return DataImm(OpORR, rd, rn, imm, sc); }
    bool eor(Reg rd, Reg rn, uint32_t imm, SetCC sc = LeaveCC) { return DataImm(OpEOR, rd, rn, imm, sc); }
    bool bic(Reg rd, Reg rn, uint32_t imm, SetCC sc = LeaveCC) { return DataImm(OpBIC, rd, rn, imm, sc); }
    bool mov(Reg rd, uint32_t imm, SetCC sc = LeaveCC) { return DataImm(OpORR, rd, pc, imm, sc); }
    bool mvn(Reg rd, uint32_t imm, SetCC sc = LeaveCC) { return DataImm(OpORN, rd, pc, imm, sc); }
    bool cmp(Reg rn, uint32_t imm) { return DataImm(OpSUB, pc, rn, imm, SetFlags); }
    bool cmn(Reg rn, uint32_t imm) { return DataImm(OpADD, pc, rn, imm, SetFlags); }
    bool tst(Reg rn, uint32_t imm) { return DataImm(OpAND, pc, rn, imm, SetFlags); }
    bool teq(Reg rn, uint32_t imm) { return DataImm(OpEOR, pc, rn, imm, SetFlags); }

    void add(Reg rd, Reg rn, Reg rm, SetCC sc = LeaveCC) { DataReg(OpADD, rd, rn, rm, LSL, 0, sc); }
    void sub(Reg rd, Reg rn, Reg rm, SetCC sc = LeaveCC) { DataReg(OpSUB, rd, rn, rm, LSL, 0, sc); }
    void and_(Reg rd, Reg rn, Reg rm, SetCC sc = LeaveCC) { DataReg(OpAND, rd, rn, rm, LSL, 0, sc); }
    void orr(Reg rd, Reg rn, Reg rm, SetCC sc = LeaveCC) { DataReg(OpORR, rd, rn, rm, LSL, 0, sc); }
    void eor(Reg rd, Reg rn, Reg rm, SetCC sc = LeaveCC) { DataReg(OpEOR, rd, rn, rm, LSL, 0, sc); }
    void mov(Reg rd, Reg rm, SetCC sc = LeaveCC) { DataReg(OpORR, rd, pc, rm, LSL, 0, sc); }
    void mvn(Reg rd, Reg rm, SetCC sc = LeaveCC) { DataReg(OpORN, rd, pc, rm, LSL, 0, sc); }
    void cmp(Reg rn, Reg rm) { DataReg(OpSUB, pc, rn, rm, LSL, 0, SetFlags); }
    void cmn(Reg rn, Reg rm) { DataReg(OpADD, pc, rn, rm, LSL, 0, SetFlags); }
    void tst(Reg rn, Reg rm) { DataReg(OpAND, pc, rn, rm, LSL, 0, SetFlags); }
    // Shift by immediate (amount 1..31; LSR/ASR also 32)
    void ShiftImm(ShiftType st, Reg rd, Reg rm, uint32_t amount, SetCC sc = LeaveCC) {
        DataReg(OpORR, rd, pc, rm, st, amount, sc);
    }
    // Shift by register: rd = rn <st> rm
    void ShiftReg(ShiftType st, Reg rd, Reg rn, Reg rm, SetCC sc = LeaveCC) {
        Emit32(uint16_t(0xFA00 | st << 5 | sc << 4 | rn), uint16_t(0xF000 | rd << 8 | rm));
    }

    // Plain 12-bit immediates (ADDW/SUBW), 16-bit moves, and 32-bit constants
    void addw(Reg rd, Reg rn, uint32_t imm12) {
        Emit32(uint16_t(0xF200 | ((imm12 >> 11) & 1) << 10 | rn),
               uint16_t(((imm12 >> 8) & 7) << 12 | rd << 8 | (imm12 & 0xFF)));
    }
    void subw(Reg rd, Reg rn, uint32_t imm12) {
        Emit32(uint16_t(0xF2A0 | ((imm12 >> 11) & 1) << 10 | rn),
               uint16_t(((imm12 >> 8) & 7) << 12 | rd << 8 | (imm12 & 0xFF)));
    }
    static uint32_t EncodeMovWT(bool top, Reg rd, uint32_t imm16) {
        uint16_t hw1 = uint16_t((top ? 0xF2C0 : 0xF240) | ((imm16 >> 11) & 1) << 10 | (imm16 >> 12));
        uint16_t hw2 = uint16_t(((imm16 >> 8) & 7) << 12 | rd << 8 | (imm16 & 0xFF));
        return (uint32_t(hw1) << 16) | hw2;
    }
    static uint32_t DecodeMovWTImm(uint32_t insn) {
        uint32_t hw1 = insn >> 16, hw2 = insn & 0xFFFF;
        return (hw1 & 0xF) << 12 | ((hw1 >> 10) & 1) << 11 | ((hw2 >> 12) & 7) << 8 | (hw2 & 0xFF);
    }
    void movw(Reg rd, uint32_t imm16) { uint32_t i = EncodeMovWT(false, rd, imm16); Emit32(uint16_t(i >> 16), uint16_t(i)); }
    void movt(Reg rd, uint32_t imm16) { uint32_t i = EncodeMovWT(true, rd, imm16); Emit32(uint16_t(i >> 16), uint16_t(i)); }
    // Always 8 bytes, so it can be patched later (PatchMov32).
    void mov32(Reg rd, uint32_t v) { movw(rd, v & 0xFFFF); movt(rd, v >> 16); }
    static void PatchMov32(uint8_t* at, uint32_t v) {
        uint32_t lo = Read32(at), hi = Read32(at + 4);
        Reg rd = Reg((lo >> 8) & 0xF);
        (void)hi;
        Write32(at, EncodeMovWT(false, rd, v & 0xFFFF));
        Write32(at + 4, EncodeMovWT(true, rd, v >> 16));
    }
    static uint32_t ReadMov32(const uint8_t* at) {
        return DecodeMovWTImm(Read32(at)) | (DecodeMovWTImm(Read32(at + 4)) << 16);
    }

    // ---- multiply / divide / misc ----
    void mul(Reg rd, Reg rn, Reg rm) { Emit32(uint16_t(0xFB00 | rn), uint16_t(0xF000 | rd << 8 | rm)); }
    void mla(Reg rd, Reg rn, Reg rm, Reg ra) { Emit32(uint16_t(0xFB00 | rn), uint16_t(ra << 12 | rd << 8 | rm)); }
    void mls(Reg rd, Reg rn, Reg rm, Reg ra) { Emit32(uint16_t(0xFB00 | rn), uint16_t(ra << 12 | rd << 8 | 0x10 | rm)); }
    void smull(Reg lo, Reg hi, Reg rn, Reg rm) { Emit32(uint16_t(0xFB80 | rn), uint16_t(lo << 12 | hi << 8 | rm)); }
    void umull(Reg lo, Reg hi, Reg rn, Reg rm) { Emit32(uint16_t(0xFBA0 | rn), uint16_t(lo << 12 | hi << 8 | rm)); }
    void sdiv(Reg rd, Reg rn, Reg rm) { Emit32(uint16_t(0xFB90 | rn), uint16_t(0xF0F0 | rd << 8 | rm)); }
    void udiv(Reg rd, Reg rn, Reg rm) { Emit32(uint16_t(0xFBB0 | rn), uint16_t(0xF0F0 | rd << 8 | rm)); }
    void clz(Reg rd, Reg rm) { Emit32(uint16_t(0xFAB0 | rm), uint16_t(0xF080 | rd << 8 | rm)); }
    void sxtb(Reg rd, Reg rm) { Emit32(0xFA4F, uint16_t(0xF080 | rd << 8 | rm)); }
    void sxth(Reg rd, Reg rm) { Emit32(0xFA0F, uint16_t(0xF080 | rd << 8 | rm)); }
    void uxtb(Reg rd, Reg rm) { Emit32(0xFA5F, uint16_t(0xF080 | rd << 8 | rm)); }
    void uxth(Reg rd, Reg rm) { Emit32(0xFA1F, uint16_t(0xF080 | rd << 8 | rm)); }

    // ---- loads and stores ----
    enum MemOp : uint16_t {   // hw1 base for the T4 (imm8) / register forms
        STRB_ = 0xF800, STRH_ = 0xF820, STR_ = 0xF840,
        LDRB_ = 0xF810, LDRH_ = 0xF830, LDR_ = 0xF850,
        LDRSB_ = 0xF910, LDRSH_ = 0xF930
    };
    // [rn, #offset] / [rn, #offset]! / [rn], #offset. Offset -255..4095 (Offset mode),
    // -255..255 with writeback. Returns false if it doesn't fit.
    bool Mem(MemOp op, Reg rt, Reg rn, int32_t offset, IndexMode mode = Offset) {
        if (mode == Offset && offset >= 0 && offset <= 4095) {
            // T3/T2: 12-bit positive offset (hw1 bit 7 set)
            Emit32(uint16_t(op | 0x80 | rn), uint16_t(rt << 12 | offset));
            return true;
        }
        if (offset < -255 || offset > 255) return false;
        uint32_t u = offset >= 0, imm8 = uint32_t(offset >= 0 ? offset : -offset);
        uint32_t p = mode != PostIndex, w = mode != Offset;
        Emit32(uint16_t(op | rn), uint16_t(rt << 12 | 0x800 | p << 10 | u << 9 | w << 8 | imm8));
        return true;
    }
    // [rn, rm, lsl #shift], shift 0..3
    void MemReg(MemOp op, Reg rt, Reg rn, Reg rm, uint32_t shift = 0) {
        Emit32(uint16_t(op | rn), uint16_t(rt << 12 | (shift & 3) << 4 | rm));
    }
    bool ldr(Reg rt, Reg rn, int32_t off, IndexMode m = Offset) { return Mem(LDR_, rt, rn, off, m); }
    bool str(Reg rt, Reg rn, int32_t off, IndexMode m = Offset) { return Mem(STR_, rt, rn, off, m); }
    bool ldrb(Reg rt, Reg rn, int32_t off, IndexMode m = Offset) { return Mem(LDRB_, rt, rn, off, m); }
    bool strb(Reg rt, Reg rn, int32_t off, IndexMode m = Offset) { return Mem(STRB_, rt, rn, off, m); }
    bool ldrh(Reg rt, Reg rn, int32_t off, IndexMode m = Offset) { return Mem(LDRH_, rt, rn, off, m); }
    bool strh(Reg rt, Reg rn, int32_t off, IndexMode m = Offset) { return Mem(STRH_, rt, rn, off, m); }
    bool ldrsb(Reg rt, Reg rn, int32_t off, IndexMode m = Offset) { return Mem(LDRSB_, rt, rn, off, m); }
    bool ldrsh(Reg rt, Reg rn, int32_t off, IndexMode m = Offset) { return Mem(LDRSH_, rt, rn, off, m); }

    // LDR rt, [pc, #+/-offset] (literal; offset from Align(this insn + 4, 4))
    void ldr_literal(Reg rt, int32_t offset) {
        uint32_t u = offset >= 0, imm12 = uint32_t(offset >= 0 ? offset : -offset) & 0xFFF;
        Emit32(uint16_t(0xF85F | u << 7), uint16_t(rt << 12 | imm12));
    }

    // LDRD/STRD rt, rt2, [rn, #+/-offset] (multiple of 4, up to 1020)
    bool MemDual(bool load, Reg rt, Reg rt2, Reg rn, int32_t offset, IndexMode mode = Offset) {
        uint32_t mag = uint32_t(offset >= 0 ? offset : -offset);
        if ((mag & 3) || mag > 1020) return false;
        uint32_t u = offset >= 0, p = mode != PostIndex, w = mode != Offset;
        Emit32(uint16_t(0xE840 | p << 8 | u << 7 | w << 5 | load << 4 | rn),
               uint16_t(rt << 12 | rt2 << 8 | (mag >> 2)));
        return true;
    }
    bool ldrd(Reg rt, Reg rt2, Reg rn, int32_t off, IndexMode m = Offset) { return MemDual(true, rt, rt2, rn, off, m); }
    bool strd(Reg rt, Reg rt2, Reg rn, int32_t off, IndexMode m = Offset) { return MemDual(false, rt, rt2, rn, off, m); }

    // Register lists (bit n = rn). SP can't be in a list; PC only in loads.
    void ldmia(Reg rn, uint16_t list, bool writeback) { Emit32(uint16_t(0xE890 | writeback << 5 | rn), list); }
    void stmia(Reg rn, uint16_t list, bool writeback) { Emit32(uint16_t(0xE880 | writeback << 5 | rn), list); }
    void ldmdb(Reg rn, uint16_t list, bool writeback) { Emit32(uint16_t(0xE910 | writeback << 5 | rn), list); }
    void stmdb(Reg rn, uint16_t list, bool writeback) { Emit32(uint16_t(0xE900 | writeback << 5 | rn), list); }
    static bool SingleReg(uint16_t list, Reg* r) {
        if (!list || (list & (list - 1))) return false;
        uint8_t n = 0;
        while (!(list & (1u << n))) n++;
        *r = Reg(n);
        return true;
    }
    // A one-register list is a STR/LDR with writeback (as assemblers do).
    void push(uint16_t list) { Reg r; if (SingleReg(list, &r)) str(r, sp, -4, PreIndex); else stmdb(sp, list, true); }
    void pop(uint16_t list) { Reg r; if (SingleReg(list, &r)) ldr(r, sp, 4, PostIndex); else ldmia(sp, list, true); }

    // ---- atomics ----
    void ldrex(Reg rt, Reg rn) { Emit32(uint16_t(0xE850 | rn), uint16_t(rt << 12 | 0xF00)); }
    void strex(Reg rd, Reg rt, Reg rn) { Emit32(uint16_t(0xE840 | rn), uint16_t(rt << 12 | rd << 8)); }
    void dmb() { Emit32(0xF3BF, 0x8F5F); }   // dmb sy

    // ---- branches ----
    // Offsets are relative to the branch address + 4.
    static uint32_t EncodeB(int32_t off) {           // B.W, +/-16 MB
        uint32_t s = off < 0, imm = uint32_t(off) >> 1;
        uint32_t i1 = (imm >> 22) & 1, i2 = (imm >> 21) & 1;
        uint32_t j1 = (~(i1 ^ s)) & 1, j2 = (~(i2 ^ s)) & 1;
        return (0xF000u | s << 10 | ((imm >> 11) & 0x3FF)) << 16 |
               (0x9000u | j1 << 13 | j2 << 11 | (imm & 0x7FF));
    }
    static uint32_t EncodeBL(int32_t off) { return EncodeB(off) | 0x4000; }   // BL
    static uint32_t EncodeBCond(Cond c, int32_t off) {   // B<c>.W, +/-1 MB
        uint32_t s = off < 0, imm = uint32_t(off) >> 1;
        uint32_t j2 = (imm >> 18) & 1, j1 = (imm >> 17) & 1;
        return (0xF000u | s << 10 | c << 6 | ((imm >> 11) & 0x3F)) << 16 |
               (0x8000u | j1 << 13 | j2 << 11 | (imm & 0x7FF));
    }
    static bool IsBranch(uint32_t insn) { return (insn & 0xF8008000u) == 0xF0008000u; }
    static bool IsCondBranch(uint32_t insn) { return IsBranch(insn) && !(insn & 0x1000); }
    static int32_t DecodeBranchOffset(uint32_t insn) {
        uint32_t hw1 = insn >> 16, hw2 = insn & 0xFFFF;
        uint32_t s = (hw1 >> 10) & 1, j1 = (hw2 >> 13) & 1, j2 = (hw2 >> 11) & 1;
        if (IsCondBranch(insn)) {
            uint32_t v = s << 20 | j2 << 19 | j1 << 18 | (hw1 & 0x3F) << 12 | (hw2 & 0x7FF) << 1;
            return int32_t(v << 11) >> 11;
        }
        uint32_t i1 = (~(j1 ^ s)) & 1, i2 = (~(j2 ^ s)) & 1;
        uint32_t v = s << 24 | i1 << 23 | i2 << 22 | (hw1 & 0x3FF) << 12 | (hw2 & 0x7FF) << 1;
        return int32_t(v << 7) >> 7;
    }
    // Retargets the branch at |at| (B.W, B<c>.W or BL) to |target| (both code offsets).
    static bool PatchBranch(uint8_t* base, int32_t at, int32_t target) {
        uint32_t insn = Read32(base + at);
        int32_t off = target - (at + 4);
        if (IsCondBranch(insn)) {
            if (off < -(1 << 20) || off >= (1 << 20)) return false;
            Write32(base + at, EncodeBCond(Cond((insn >> 22) & 0xF), off));
        } else {
            if (off < -(1 << 24) || off >= (1 << 24)) return false;
            uint32_t enc = (insn & 0x4000) ? EncodeBL(off) : EncodeB(off);
            Write32(base + at, enc);
        }
        return true;
    }

    void b(Label& l) { BranchTo(l, AL, false); }
    void b(Cond c, Label& l) { BranchTo(l, c, false); }
    void bl(Label& l) { BranchTo(l, AL, true); }
    void bx(Reg rm) { Emit16(uint16_t(0x4700 | rm << 3)); }
    void blx(Reg rm) { Emit16(uint16_t(0x4780 | rm << 3)); }
    void bind(Label& l) {
        l.offset = int32_t(size());
        for (int32_t at : l.uses)
            if (!PatchBranch(code.data(), at, l.offset)) ok = false;
        l.uses.clear();
    }

    // IT block for up to 4 following instructions. |pattern| describes
    // instructions 2..4: 'T' (same condition) or 'E' (inverse), e.g. "TE".
    void it(Cond firstcond, const char* pattern = "") {
        uint32_t n = 0, mask = 0, c0 = firstcond & 1;
        while (pattern[n]) n++;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t bit = pattern[i] == 'T' ? c0 : (c0 ^ 1);
            mask |= bit << (3 - i);
        }
        mask |= 1u << (3 - n);
        Emit16(uint16_t(0xBF00 | firstcond << 4 | mask));
    }

    void nop16() { Emit16(0xBF00); }
    void nop() { Emit32(0xF3AF, 0x8000); }
    void bkpt(uint8_t imm = 0) { Emit16(uint16_t(0xBE00 | imm)); }
    void udf(uint16_t imm16) { Emit32(uint16_t(0xF7F0 | (imm16 >> 12)), uint16_t(0xA000 | (imm16 & 0xFFF))); }

    // ---- VFP (double and single precision) ----
    static uint32_t VD(DReg r) { return (uint32_t(r.code >> 4) << 22) | (uint32_t(r.code & 15) << 12); }
    static uint32_t VN(DReg r) { return (uint32_t(r.code >> 4) << 7) | (uint32_t(r.code & 15) << 16); }
    static uint32_t VM(DReg r) { return (uint32_t(r.code >> 4) << 5) | (r.code & 15); }
    static uint32_t VD(SReg r) { return (uint32_t(r.code & 1) << 22) | (uint32_t(r.code >> 1) << 12); }
    static uint32_t VN(SReg r) { return (uint32_t(r.code & 1) << 7) | (uint32_t(r.code >> 1) << 16); }
    static uint32_t VM(SReg r) { return (uint32_t(r.code & 1) << 5) | (r.code >> 1); }

    void vadd(DReg dd, DReg dn, DReg dm) { EmitA32VFP(0x0E300B00 | VD(dd) | VN(dn) | VM(dm)); }
    void vsub(DReg dd, DReg dn, DReg dm) { EmitA32VFP(0x0E300B40 | VD(dd) | VN(dn) | VM(dm)); }
    void vmul(DReg dd, DReg dn, DReg dm) { EmitA32VFP(0x0E200B00 | VD(dd) | VN(dn) | VM(dm)); }
    void vdiv(DReg dd, DReg dn, DReg dm) { EmitA32VFP(0x0E800B00 | VD(dd) | VN(dn) | VM(dm)); }
    void vadd(SReg sd, SReg sn, SReg sm) { EmitA32VFP(0x0E300A00 | VD(sd) | VN(sn) | VM(sm)); }
    void vsub(SReg sd, SReg sn, SReg sm) { EmitA32VFP(0x0E300A40 | VD(sd) | VN(sn) | VM(sm)); }
    void vmul(SReg sd, SReg sn, SReg sm) { EmitA32VFP(0x0E200A00 | VD(sd) | VN(sn) | VM(sm)); }
    void vdiv(SReg sd, SReg sn, SReg sm) { EmitA32VFP(0x0E800A00 | VD(sd) | VN(sn) | VM(sm)); }
    void vneg(DReg dd, DReg dm) { EmitA32VFP(0x0EB10B40 | VD(dd) | VM(dm)); }
    void vabs(DReg dd, DReg dm) { EmitA32VFP(0x0EB00BC0 | VD(dd) | VM(dm)); }
    void vsqrt(DReg dd, DReg dm) { EmitA32VFP(0x0EB10BC0 | VD(dd) | VM(dm)); }
    void vmov(DReg dd, DReg dm) { EmitA32VFP(0x0EB00B40 | VD(dd) | VM(dm)); }
    void vneg(SReg sd, SReg sm) { EmitA32VFP(0x0EB10A40 | VD(sd) | VM(sm)); }
    void vabs(SReg sd, SReg sm) { EmitA32VFP(0x0EB00AC0 | VD(sd) | VM(sm)); }
    void vsqrt(SReg sd, SReg sm) { EmitA32VFP(0x0EB10AC0 | VD(sd) | VM(sm)); }
    void vmov(SReg sd, SReg sm) { EmitA32VFP(0x0EB00A40 | VD(sd) | VM(sm)); }
    void vcmp(DReg dd, DReg dm) { EmitA32VFP(0x0EB40B40 | VD(dd) | VM(dm)); }
    void vcmpe(DReg dd, DReg dm) { EmitA32VFP(0x0EB40BC0 | VD(dd) | VM(dm)); }
    void vcmpz(DReg dd) { EmitA32VFP(0x0EB50B40 | VD(dd)); }        // vcmp dd, #0
    void vcmp(SReg sd, SReg sm) { EmitA32VFP(0x0EB40A40 | VD(sd) | VM(sm)); }
    void vmrs_apsr() { EmitA32VFP(0x0EF1FA10); }                      // vmrs APSR_nzcv, fpscr
    // Conversions
    void vcvt_f64_f32(DReg dd, SReg sm) { EmitA32VFP(0x0EB70AC0 | VD(dd) | VM(sm)); }
    void vcvt_f32_f64(SReg sd, DReg dm) { EmitA32VFP(0x0EB70BC0 | VD(sd) | VM(dm)); }
    void vcvt_f64_s32(DReg dd, SReg sm) { EmitA32VFP(0x0EB80BC0 | VD(dd) | VM(sm)); }
    void vcvt_f64_u32(DReg dd, SReg sm) { EmitA32VFP(0x0EB80B40 | VD(dd) | VM(sm)); }
    void vcvt_s32_f64(SReg sd, DReg dm) { EmitA32VFP(0x0EBD0BC0 | VD(sd) | VM(dm)); }  // round to zero
    void vcvt_u32_f64(SReg sd, DReg dm) { EmitA32VFP(0x0EBC0BC0 | VD(sd) | VM(dm)); }
    void vcvt_f32_s32(SReg sd, SReg sm) { EmitA32VFP(0x0EB80AC0 | VD(sd) | VM(sm)); }
    void vcvt_s32_f32(SReg sd, SReg sm) { EmitA32VFP(0x0EBD0AC0 | VD(sd) | VM(sm)); }
    // Core <-> VFP
    void vmov(SReg sn, Reg rt) { EmitA32VFP(0x0E000A10 | VN(sn) | rt << 12); }
    void vmov(Reg rt, SReg sn) { EmitA32VFP(0x0E100A10 | VN(sn) | rt << 12); }
    void vmov(DReg dm, Reg rt, Reg rt2) { EmitA32VFP(0x0C400B10 | rt2 << 16 | rt << 12 | VM(dm)); }
    void vmov(Reg rt, Reg rt2, DReg dm) { EmitA32VFP(0x0C500B10 | rt2 << 16 | rt << 12 | VM(dm)); }
    // VLDR/VSTR [rn, #+/-offset], offset a multiple of 4 up to 1020
    bool VMem(bool load, uint32_t vd, bool dbl, Reg rn, int32_t offset) {
        uint32_t mag = uint32_t(offset >= 0 ? offset : -offset);
        if ((mag & 3) || mag > 1020) return false;
        EmitA32VFP((load ? 0x0D100A00 : 0x0D000A00) | (offset >= 0) << 23 | vd |
                   dbl << 8 | rn << 16 | (mag >> 2));
        return true;
    }
    bool vldr(DReg dd, Reg rn, int32_t off) { return VMem(true, VD(dd), true, rn, off); }
    bool vstr(DReg dd, Reg rn, int32_t off) { return VMem(false, VD(dd), true, rn, off); }
    bool vldr(SReg sd, Reg rn, int32_t off) { return VMem(true, VD(sd), false, rn, off); }
    bool vstr(SReg sd, Reg rn, int32_t off) { return VMem(false, VD(sd), false, rn, off); }
    // vpush/vpop {d<first>..d<first+count-1>}
    void vpush(DReg first, uint32_t count) { EmitA32VFP(0x0D2D0B00 | VD(first) | count * 2); }
    void vpop(DReg first, uint32_t count) { EmitA32VFP(0x0CBD0B00 | VD(first) | count * 2); }

private:
    void BranchTo(Label& l, Cond c, bool link) {
        int32_t at = int32_t(size());
        int32_t off = l.bound() ? l.offset - (at + 4) : 0;
        uint32_t insn = link ? EncodeBL(off) : c == AL ? EncodeB(off) : EncodeBCond(c, off);
        Emit32(uint16_t(insn >> 16), uint16_t(insn));
        if (!l.bound())
            l.uses.push_back(at);
    }
};

} // namespace thumb2
} // namespace rfx

#endif
