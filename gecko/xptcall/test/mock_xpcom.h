/* Minimal stand-ins for the XPCOM types xptcall uses, so the real
 * xptcinvoke_arm_win.cpp / xptcstubs_arm_win.cpp can be tested outside Gecko.
 * Layouts and enum values follow UXP's xptcall.h / xptinfo.h. */
#ifndef mock_xpcom_h
#define mock_xpcom_h
#include <stdint.h>

enum class nsresult : uint32_t {};
static const nsresult NS_OK                    = nsresult(0);
static const nsresult NS_ERROR_FAILURE         = nsresult(0x80004005);
static const nsresult NS_ERROR_NOT_IMPLEMENTED = nsresult(0x80004001);
static const nsresult NS_ERROR_UNEXPECTED      = nsresult(0x8000ffff);

#define NS_IMETHOD virtual nsresult
#define EXPORT_XPCOM_API(t) extern "C" t
void test_fail(const char* msg);
#define NS_ASSERTION(c, m) do { if (!(c)) test_fail(m); } while (0)
#define NS_ERROR(m) test_fail(m)

class nsISupports {
public:
    virtual nsresult QueryInterface(const void*, void**) = 0;
    virtual uint32_t AddRef() = 0;
    virtual uint32_t Release() = 0;
};

struct nsXPTType {
    uint8_t flags;
    enum {
        T_I8 = 0, T_I16 = 1, T_I32 = 2, T_I64 = 3, T_U8 = 4, T_U16 = 5,
        T_U32 = 6, T_U64 = 7, T_FLOAT = 8, T_DOUBLE = 9, T_BOOL = 10,
        T_CHAR = 11, T_WCHAR = 12, T_VOID = 13, T_IID = 14, T_INTERFACE = 18
    };
    constexpr nsXPTType() : flags(0) {}
    constexpr nsXPTType(uint8_t t) : flags(t) {}  // constexpr: no static ctors in the freestanding test
    uint8_t TagPart() const { return flags & 0x1f; }
    bool IsArithmetic() const { return TagPart() <= T_WCHAR; }
    operator uint8_t() const { return TagPart(); }
};

struct nsXPTCMiniVariant {
    union {
        int8_t i8; int16_t i16; int32_t i32; int64_t i64;
        uint8_t u8; uint16_t u16; uint32_t u32; uint64_t u64;
        float f; double d; bool b; char c; char16_t wc; void* p;
    } val;
};

struct nsXPTCVariant : public nsXPTCMiniVariant {
    void* ptr;
    nsXPTType type;
    uint8_t flags;
    enum { PTR_IS_DATA = 0x1 };
    bool IsPtrData() const { return 0 != (flags & PTR_IS_DATA); }
};

struct nsXPTParamInfo {
    nsXPTType type;
    bool out;
    bool IsOut() const { return out; }
    const nsXPTType& GetType() const { return type; }
};

struct nsXPTMethodInfo {
    uint8_t count;
    const nsXPTParamInfo* params;
    uint8_t GetParamCount() const { return count; }
    const nsXPTParamInfo& GetParam(uint8_t i) const { return params[i]; }
};

class xptiInterfaceEntry {
public:
    const nsXPTMethodInfo* methods;   // indexed by vtable slot
    uint32_t count;
    nsresult GetMethodInfo(uint16_t i, const nsXPTMethodInfo** out) {
        *out = i < count ? &methods[i] : nullptr;
        return NS_OK;
    }
};

class nsIXPTCProxy : public nsISupports {
public:
    NS_IMETHOD CallMethod(uint16_t aMethodIndex, const nsXPTMethodInfo* aInfo,
                          nsXPTCMiniVariant* aParams) = 0;
};

extern "C" nsresult
NS_InvokeByIndex(nsISupports* that, uint32_t methodIndex,
                 uint32_t paramCount, nsXPTCVariant* params);
#endif
