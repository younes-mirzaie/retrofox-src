#!/usr/bin/env python3
"""Generates encoding test cases for Thumb2Assembler.h: each case is the
assembler text llvm-mc should produce the same bytes for, and the C++ call.
   gen_cases.py OUT.inc [seed]"""
import random, sys

rnd = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
cases = []
def case(text, call):
    cases.append((text, call))

def decode_mod_imm(imm12):
    b = imm12 & 0xFF
    if imm12 >> 10 == 0:
        k = (imm12 >> 8) & 3
        return [b, b | b << 16, b << 8 | b << 24, b * 0x01010101][k]
    unrot = 0x80 | (imm12 & 0x7F); rot = imm12 >> 7
    return ((unrot >> rot) | (unrot << (32 - rot))) & 0xFFFFFFFF

ENCODABLE = sorted({decode_mod_imm(i) for i in range(4096)})
LOW = [f"r{i}" for i in range(13)] + ["lr"]
def R(): return rnd.choice(LOW)
def Rcpp(r): return r
def imm(): return rnd.choice(ENCODABLE)

# data processing, modified immediate
for op, name in [("add", "add"), ("sub", "sub"), ("rsb", "rsb"), ("and_", "and"),
                 ("orr", "orr"), ("eor", "eor"), ("bic", "bic")]:
    for _ in range(12):
        rd, rn, v = R(), R(), imm()
        sc = rnd.random() < 0.3
        case(f"{name}{'s' if sc else ''}.w {rd}, {rn}, #{v}",
             f"A.{op}({rd}, {rn}, {v}u{', SetFlags' if sc else ''})")
for _ in range(10):
    rd, v = R(), imm()
    case(f"mov.w {rd}, #{v}", f"A.mov({rd}, {v}u)")
    case(f"mvn {rd}, #{v}", f"A.mvn({rd}, {v}u)")
    rn = R()
    case(f"cmp.w {rn}, #{v}", f"A.cmp({rn}, {v}u)")
    case(f"cmn.w {rn}, #{v}", f"A.cmn({rn}, {v}u)")
    case(f"tst.w {rn}, #{v}", f"A.tst({rn}, {v}u)")
    case(f"teq.w {rn}, #{v}", f"A.teq({rn}, {v}u)")

# data processing, register (+ shifts)
for op, name in [("OpADD", "add"), ("OpSUB", "sub"), ("OpAND", "and"), ("OpORR", "orr"),
                 ("OpEOR", "eor"), ("OpBIC", "bic"), ("OpADC", "adc"), ("OpSBC", "sbc"),
                 ("OpRSB", "rsb"), ("OpORN", "orn")]:
    for _ in range(8):
        rd, rn, rm = R(), R(), R()
        st = rnd.choice(["LSL", "LSR", "ASR", "ROR"])
        amt = rnd.randint(1, 31)
        sc = rnd.random() < 0.3
        case(f"{name}{'s' if sc else ''}.w {rd}, {rn}, {rm}, {st.lower()} #{amt}",
             f"A.DataReg({op}, {rd}, {rn}, {rm}, {st}, {amt}{', SetFlags' if sc else ', LeaveCC'})")
    rd, rn, rm = R(), R(), R()
    case(f"{name}.w {rd}, {rn}, {rm}", f"A.DataReg({op}, {rd}, {rn}, {rm})")
for _ in range(6):
    rd, rm, rn = R(), R(), R()
    case(f"mov.w {rd}, {rm}", f"A.mov({rd}, {rm})")
    case(f"mvn.w {rd}, {rm}", f"A.mvn({rd}, {rm})")
    case(f"cmp.w {rn}, {rm}", f"A.cmp({rn}, {rm})")
    case(f"tst.w {rn}, {rm}", f"A.tst({rn}, {rm})")
    case(f"cmn.w {rn}, {rm}", f"A.cmn({rn}, {rm})")
    for st in ["LSL", "LSR", "ASR", "ROR"]:
        amt = rnd.randint(1, 31)
        case(f"{st.lower()}.w {rd}, {rm}, #{amt}", f"A.ShiftImm({st}, {rd}, {rm}, {amt})")
        case(f"{st.lower()}.w {rd}, {rn}, {rm}", f"A.ShiftReg({st}, {rd}, {rn}, {rm})")
    case(f"lsr.w {rd}, {rm}, #32", f"A.ShiftImm(LSR, {rd}, {rm}, 32)")
    case(f"asr.w {rd}, {rm}, #32", f"A.ShiftImm(ASR, {rd}, {rm}, 32)")

# plain immediates
for _ in range(10):
    rd, rn, v, w = R(), R(), rnd.randint(0, 4095), rnd.randint(0, 0xFFFF)
    case(f"addw {rd}, {rn}, #{v}", f"A.addw({rd}, {rn}, {v})")
    case(f"subw {rd}, {rn}, #{v}", f"A.subw({rd}, {rn}, {v})")
    case(f"movw {rd}, #{w}", f"A.movw({rd}, {w})")
    case(f"movt {rd}, #{w}", f"A.movt({rd}, {w})")
    x = rnd.randint(0, 0xFFFFFFFF)
    case(f"movw {rd}, #{x & 0xFFFF}; movt {rd}, #{x >> 16}", f"A.mov32({rd}, {x}u)")

# multiply / divide / misc
for _ in range(6):
    a, b_, c, e = R(), R(), R(), R()
    case(f"mul {a}, {b_}, {c}", f"A.mul({a}, {b_}, {c})")
    case(f"mla {a}, {b_}, {c}, {e}", f"A.mla({a}, {b_}, {c}, {e})")
    case(f"mls {a}, {b_}, {c}, {e}", f"A.mls({a}, {b_}, {c}, {e})")
    lo, hi = rnd.sample(LOW, 2)
    case(f"smull {lo}, {hi}, {c}, {e}", f"A.smull({lo}, {hi}, {c}, {e})")
    case(f"umull {lo}, {hi}, {c}, {e}", f"A.umull({lo}, {hi}, {c}, {e})")
    case(f"sdiv {a}, {b_}, {c}", f"A.sdiv({a}, {b_}, {c})")
    case(f"udiv {a}, {b_}, {c}", f"A.udiv({a}, {b_}, {c})")
    case(f"clz {a}, {b_}", f"A.clz({a}, {b_})")
    case(f"sxtb.w {a}, {b_}", f"A.sxtb({a}, {b_})")
    case(f"sxth.w {a}, {b_}", f"A.sxth({a}, {b_})")
    case(f"uxtb.w {a}, {b_}", f"A.uxtb({a}, {b_})")
    case(f"uxth.w {a}, {b_}", f"A.uxth({a}, {b_})")

# loads and stores
BASES = LOW + ["sp"]
for fn in ["ldr", "str", "ldrb", "strb", "ldrh", "strh", "ldrsb", "ldrsh"]:
    for _ in range(5):
        rt, rn = R(), rnd.choice(BASES)
        off = rnd.randint(0, 4095)
        case(f"{fn}.w {rt}, [{rn}, #{off}]", f"A.{fn}({rt}, {rn}, {off})")
        off = -rnd.randint(1, 255)
        case(f"{fn} {rt}, [{rn}, #{off}]", f"A.{fn}({rt}, {rn}, {off})")
        rn2 = rnd.choice([r for r in LOW if r != rt])
        off = rnd.randint(-255, 255)
        case(f"{fn} {rt}, [{rn2}, #{off}]!", f"A.{fn}({rt}, {rn2}, {off}, PreIndex)")
        case(f"{fn} {rt}, [{rn2}], #{off}", f"A.{fn}({rt}, {rn2}, {off}, PostIndex)")
        rm, sh = R(), rnd.randint(0, 3)
        op = {"ldr": "LDR_", "str": "STR_", "ldrb": "LDRB_", "strb": "STRB_", "ldrh": "LDRH_",
              "strh": "STRH_", "ldrsb": "LDRSB_", "ldrsh": "LDRSH_"}[fn]
        case(f"{fn}.w {rt}, [{rn}, {rm}, lsl #{sh}]", f"A.MemReg(Assembler::{op}, {rt}, {rn}, {rm}, {sh})")
for _ in range(6):
    rt, rt2 = rnd.sample([f"r{i}" for i in range(13)], 2)
    rn = rnd.choice([r for r in BASES if r not in (rt, rt2)])
    off = rnd.randint(-255, 255) * 4
    case(f"ldrd {rt}, {rt2}, [{rn}, #{off}]", f"A.ldrd({rt}, {rt2}, {rn}, {off})")
    case(f"strd {rt}, {rt2}, [{rn}, #{off}]", f"A.strd({rt}, {rt2}, {rn}, {off})")
    case(f"ldrd {rt}, {rt2}, [{rn}, #{off}]!", f"A.ldrd({rt}, {rt2}, {rn}, {off}, PreIndex)")
    case(f"strd {rt}, {rt2}, [{rn}], #{off}", f"A.strd({rt}, {rt2}, {rn}, {off}, PostIndex)")
    off = rnd.randint(-4095, 4095)
    case(f"ldr.w {rt}, [pc, #{off}]", f"A.ldr_literal({rt}, {off})")

def reglist():
    regs = sorted(rnd.sample(range(13), rnd.randint(2, 6)))
    return regs
for _ in range(6):
    regs = reglist()
    txt = ", ".join(f"r{i}" for i in regs); mask = sum(1 << i for i in regs)
    rn = rnd.choice([f"r{i}" for i in range(13) if i not in regs])
    case(f"ldm.w {rn}!, {{{txt}}}", f"A.ldmia({rn}, {mask}, true)")
    case(f"stm.w {rn}, {{{txt}}}", f"A.stmia({rn}, {mask}, false)")
    case(f"ldmdb {rn}, {{{txt}}}", f"A.ldmdb({rn}, {mask}, false)")
    case(f"stmdb {rn}!, {{{txt}}}", f"A.stmdb({rn}, {mask}, true)")
    case(f"push.w {{{txt}, lr}}", f"A.push({mask | 1 << 14})")
    case(f"pop.w {{{txt}, pc}}", f"A.pop({mask | 1 << 15})")
case("push.w {r4}", "A.push(1 << 4)")
case("pop.w {r5}", "A.pop(1 << 5)")
case("ldrex r1, [r2]", "A.ldrex(r1, r2)")
case("strex r0, r1, [r2]", "A.strex(r0, r1, r2)")
case("dmb sy", "A.dmb()")

# branches by offset (llvm resolves "." relative labels)
for _ in range(20):
    off = rnd.randint(-(1 << 23), (1 << 23) - 1) * 2
    case(f"b.w #{off}", f"A.Emit32(uint16_t(Assembler::EncodeB({off}) >> 16), uint16_t(Assembler::EncodeB({off})))")
    case(f"bl #{off}", f"A.Emit32(uint16_t(Assembler::EncodeBL({off}) >> 16), uint16_t(Assembler::EncodeBL({off})))")
    off = rnd.randint(-(1 << 19), (1 << 19) - 1) * 2
    c = rnd.choice(["EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE"])
    case(f"b{c.lower()}.w #{off}", f"A.Emit32(uint16_t(Assembler::EncodeBCond({c}, {off}) >> 16), uint16_t(Assembler::EncodeBCond({c}, {off})))")
for r in ["r0", "r3", "r12", "lr"]:
    case(f"bx {r}", f"A.bx({r})")
    case(f"blx {r}", f"A.blx({r})")

# IT blocks (with the instructions they cover)
for c in ["EQ", "NE", "GE", "LT", "HI", "LS"]:
    inv = {"EQ": "ne", "NE": "eq", "GE": "lt", "LT": "ge", "HI": "ls", "LS": "hi"}[c]
    case(f"it {c.lower()}; mov{c.lower()}.w r0, #1", f"A.it({c}); A.mov(r0, 1u)")
    case(f"ite {c.lower()}; mov{c.lower()}.w r0, #1; mov{inv}.w r0, #0",
         f"A.it({c}, \"E\"); A.mov(r0, 1u); A.mov(r0, 0u)")
    case(f"itte {c.lower()}; add{c.lower()}.w r1, r1, #1; add{c.lower()}.w r2, r2, #1; sub{inv}.w r3, r3, #1",
         f"A.it({c}, \"TE\"); A.add(r1, r1, 1u); A.add(r2, r2, 1u); A.sub(r3, r3, 1u)")
    case(f"itet {c.lower()}; mov{c.lower()}.w r0, #1; mov{inv}.w r0, #2; mov{c.lower()}.w r1, #3",
         f"A.it({c}, \"ET\"); A.mov(r0, 1u); A.mov(r0, 2u); A.mov(r1, 3u)")
case("nop.w", "A.nop()")
case("nop", "A.nop16()")
case("bkpt #3", "A.bkpt(3)")
case("udf.w #1234", "A.udf(1234)")

# VFP
def D(): return rnd.randint(0, 31)
def S(): return rnd.randint(0, 31)
for _ in range(6):
    a, b_, c = D(), D(), D()
    for fn in ["vadd", "vsub", "vmul", "vdiv"]:
        case(f"{fn}.f64 d{a}, d{b_}, d{c}", f"A.{fn}(d({a}), d({b_}), d({c}))")
    sa, sb, sc_ = S(), S(), S()
    for fn in ["vadd", "vsub", "vmul", "vdiv"]:
        case(f"{fn}.f32 s{sa}, s{sb}, s{sc_}", f"A.{fn}(s({sa}), s({sb}), s({sc_}))")
    for fn in ["vneg", "vabs", "vsqrt"]:
        case(f"{fn}.f64 d{a}, d{b_}", f"A.{fn}(d({a}), d({b_}))")
        case(f"{fn}.f32 s{sa}, s{sb}", f"A.{fn}(s({sa}), s({sb}))")
    case(f"vmov.f64 d{a}, d{b_}", f"A.vmov(d({a}), d({b_}))")
    case(f"vmov.f32 s{sa}, s{sb}", f"A.vmov(s({sa}), s({sb}))")
    case(f"vcmp.f64 d{a}, d{b_}", f"A.vcmp(d({a}), d({b_}))")
    case(f"vcmpe.f64 d{a}, d{b_}", f"A.vcmpe(d({a}), d({b_}))")
    case(f"vcmp.f64 d{a}, #0", f"A.vcmpz(d({a}))")
    case(f"vcmp.f32 s{sa}, s{sb}", f"A.vcmp(s({sa}), s({sb}))")
    case(f"vcvt.f64.f32 d{a}, s{sa}", f"A.vcvt_f64_f32(d({a}), s({sa}))")
    case(f"vcvt.f32.f64 s{sa}, d{a}", f"A.vcvt_f32_f64(s({sa}), d({a}))")
    case(f"vcvt.f64.s32 d{a}, s{sa}", f"A.vcvt_f64_s32(d({a}), s({sa}))")
    case(f"vcvt.f64.u32 d{a}, s{sa}", f"A.vcvt_f64_u32(d({a}), s({sa}))")
    case(f"vcvt.s32.f64 s{sa}, d{a}", f"A.vcvt_s32_f64(s({sa}), d({a}))")
    case(f"vcvt.u32.f64 s{sa}, d{a}", f"A.vcvt_u32_f64(s({sa}), d({a}))")
    case(f"vcvt.f32.s32 s{sa}, s{sb}", f"A.vcvt_f32_s32(s({sa}), s({sb}))")
    case(f"vcvt.s32.f32 s{sa}, s{sb}", f"A.vcvt_s32_f32(s({sa}), s({sb}))")
    rt, rt2 = rnd.sample([f"r{i}" for i in range(13)], 2)
    case(f"vmov s{sa}, {rt}", f"A.vmov(s({sa}), {rt})")
    case(f"vmov {rt}, s{sa}", f"A.vmov({rt}, s({sa}))")
    case(f"vmov d{a}, {rt}, {rt2}", f"A.vmov(d({a}), {rt}, {rt2})")
    case(f"vmov {rt}, {rt2}, d{a}", f"A.vmov({rt}, {rt2}, d({a}))")
    rn = rnd.choice(BASES)
    off = rnd.randint(-255, 255) * 4
    case(f"vldr d{a}, [{rn}, #{off}]", f"A.vldr(d({a}), {rn}, {off})")
    case(f"vstr d{a}, [{rn}, #{off}]", f"A.vstr(d({a}), {rn}, {off})")
    case(f"vldr s{sa}, [{rn}, #{off}]", f"A.vldr(s({sa}), {rn}, {off})")
    case(f"vstr s{sa}, [{rn}, #{off}]", f"A.vstr(s({sa}), {rn}, {off})")
    first, n = rnd.randint(0, 24), rnd.randint(1, 8)
    lst = ", ".join(f"d{i}" for i in range(first, first + n))
    case(f"vpush {{{lst}}}", f"A.vpush(d({first}), {n})")
    case(f"vpop {{{lst}}}", f"A.vpop(d({first}), {n})")
case("vmrs APSR_nzcv, fpscr", "A.vmrs_apsr()")

with open(sys.argv[1], "w") as f:
    for text, call in cases:
        esc = text.replace('"', '\\"')
        f.write(f'CASE("{esc}", {call});\n')
print(len(cases), "cases")
