/* Tests xptcinvoke_arm_win.cpp and xptcstubs_arm_win.cpp against code the
 * compiler generates for the same signatures:
 *   A. NS_InvokeByIndex -> real C++ method   (invoke matches the callee ABI)
 *   B. typed C++ call    -> xptcall stub     (stubs match the caller ABI)
 *   C. NS_InvokeByIndex -> xptcall stub      (round trip)
 * Builds for Linux ARM hard-float (run with qemu-arm) or Windows ARM (phone). */
#include "mock_xpcom.h"
#include "xptcprivate.h"

void out_str(const char* s);
void out_hex(uint64_t v);
extern "C" void* rt_alloc(uint32_t n);

static uint64_t gTrace[64];
static uint32_t gTraceLen;
static int gFailures;

static void trace_begin() { gTraceLen = 0; }
static void rec(uint64_t v) { if (gTraceLen < 64) gTrace[gTraceLen++] = v; }
static void rec_b(int8_t v)   { rec(uint64_t(int64_t(v))); }
static void rec_h(int16_t v)  { rec(uint64_t(int64_t(v))); }
static void rec_i(int32_t v)  { rec(uint64_t(int64_t(v))); }
static void rec_q(int64_t v)  { rec(uint64_t(v)); }
static void rec_B(uint8_t v)  { rec(v); }
static void rec_H(uint16_t v) { rec(v); }
static void rec_I(uint32_t v) { rec(v); }
static void rec_Q(uint64_t v) { rec(v); }
static void rec_f(float v)    { uint32_t b; __builtin_memcpy(&b, &v, 4); rec(b); }
static void rec_d(double v)   { uint64_t b; __builtin_memcpy(&b, &v, 8); rec(b); }
static void rec_z(bool v)     { rec(v ? 1 : 0); }
static void rec_c(char v)     { rec(uint64_t(int64_t(v))); }
static void rec_w(char16_t v) { rec(v); }
static void rec_p(const void* v) { rec(uint32_t(uintptr_t(v))); }

void test_fail(const char* msg) { out_str("ASSERT: "); out_str(msg); out_str("\n"); gFailures++; }

#include "tests.inc"

class RecordingProxy final : public nsIXPTCProxy {
public:
    nsresult QueryInterface(const void*, void**) override { return NS_ERROR_FAILURE; }
    uint32_t AddRef() override { return 1; }
    uint32_t Release() override { return 1; }
    nsresult CallMethod(uint16_t idx, const nsXPTMethodInfo* info,
                        nsXPTCMiniVariant* p) override {
        trace_begin();
        for (uint8_t i = 0; i < info->GetParamCount(); i++) {
            const nsXPTParamInfo& pi = info->GetParam(i);
            if (pi.IsOut() || !pi.GetType().IsArithmetic()) { rec_p(p[i].val.p); continue; }
            switch (pi.GetType()) {
            case nsXPTType::T_I8: rec_b(p[i].val.i8); break;
            case nsXPTType::T_I16: rec_h(p[i].val.i16); break;
            case nsXPTType::T_I32: rec_i(p[i].val.i32); break;
            case nsXPTType::T_I64: rec_q(p[i].val.i64); break;
            case nsXPTType::T_U8: rec_B(p[i].val.u8); break;
            case nsXPTType::T_U16: rec_H(p[i].val.u16); break;
            case nsXPTType::T_U32: rec_I(p[i].val.u32); break;
            case nsXPTType::T_U64: rec_Q(p[i].val.u64); break;
            case nsXPTType::T_FLOAT: rec_f(p[i].val.f); break;
            case nsXPTType::T_DOUBLE: rec_d(p[i].val.d); break;
            case nsXPTType::T_BOOL: rec_z(p[i].val.b); break;
            case nsXPTType::T_CHAR: rec_c(p[i].val.c); break;
            case nsXPTType::T_WCHAR: rec_w(p[i].val.wc); break;
            }
        }
        return nsresult(0x1000 + (idx - 3));
    }
};

static void check(const char* what, uint32_t m, nsresult rv) {
    const uint32_t n = kMethods[m + 3].count;
    bool ok = gTraceLen == n && uint32_t(rv) == 0x1000 + m;
    for (uint32_t i = 0; ok && i < n; i++)
        ok = gTrace[i] == kExpect[m][i];
    if (ok) return;
    gFailures++;
    out_str("FAIL "); out_str(what); out_str(" M"); out_hex(m);
    out_str(" sig="); out_str(kSigs[m]); out_str(" rv="); out_hex(uint32_t(rv)); out_str("\n");
    for (uint32_t i = 0; i < n || i < gTraceLen; i++) {
        out_str("   #"); out_hex(i); out_str(" want "); out_hex(i < n ? kExpect[m][i] : 0);
        out_str(" got "); out_hex(i < gTraceLen ? gTrace[i] : 0); out_str("\n");
    }
}

int test_main() {
    TestImpl impl;
    RecordingProxy proxy;
    xptiInterfaceEntry entry = { kMethods, kMethodCount + 3 };
    nsXPTCStubBase stub(&proxy, &entry);
    ITest* stubAsTest = reinterpret_cast<ITest*>(static_cast<nsIXPTCStubBase*>(&stub));

    nsXPTCVariant* v = (nsXPTCVariant*)rt_alloc(sizeof(nsXPTCVariant) * 64);
    for (uint32_t m = 0; m < kMethodCount; m++) {
        uint32_t n = kMethods[m + 3].count;
        fill_variants(m, v);
        gTraceLen = 99;
        check("A invoke->impl", m, NS_InvokeByIndex(&impl, m + 3, n, v));
        gTraceLen = 99;
        check("B call->stub", m, direct_call(stubAsTest, m));
        gTraceLen = 99;
        check("C invoke->stub", m, NS_InvokeByIndex(stubAsTest, m + 3, n, v));
    }
    out_str(gFailures ? "xptcall test: FAILED, failures=" : "xptcall test: all passed, methods=");
    out_hex(gFailures ? gFailures : kMethodCount);
    out_str("\n");
    return gFailures ? 1 : 0;
}
