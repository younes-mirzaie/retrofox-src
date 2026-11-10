/* Test stand-in for UXP's xptcprivate.h (same class shapes). */
#ifndef xptcprivate_h___
#define xptcprivate_h___
#include "mock_xpcom.h"

#define STUB_ENTRY(n) NS_IMETHOD Stub##n() = 0;
#define SENTINEL_ENTRY(n) NS_IMETHOD Sentinel##n() = 0;
class nsIXPTCStubBase : public nsISupports {
public:
#include "xptcstubsdef.inc"
};
#undef STUB_ENTRY
#undef SENTINEL_ENTRY

#define STUB_ENTRY(n) NS_IMETHOD Stub##n() override;
#define SENTINEL_ENTRY(n) NS_IMETHOD Sentinel##n() override;
class nsXPTCStubBase final : public nsIXPTCStubBase {
public:
    nsresult QueryInterface(const void*, void**) override { return NS_ERROR_FAILURE; }
    uint32_t AddRef() override { return 1; }
    uint32_t Release() override { return 1; }
#include "xptcstubsdef.inc"
    nsXPTCStubBase(nsIXPTCProxy* aOuter, xptiInterfaceEntry* aEntry)
        : mOuter(aOuter), mEntry(aEntry) {}
    nsIXPTCProxy*       mOuter;
    xptiInterfaceEntry* mEntry;
};
#undef STUB_ENTRY
#undef SENTINEL_ENTRY
#endif
