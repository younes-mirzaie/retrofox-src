/* GDI text/bitmap drawing and Uniscribe, which the phone doesn't have at all (its GDI32.DLL only
   has regions, DCs and stock objects; there is no USP10.DLL). Verified with phone\probe\probe.exe.

   Gecko still compiles its Win32 cairo surfaces, GDI fonts and Uniscribe shaper. On the phone it
   draws with Skia and gets fonts from DirectWrite instead, so these paths should never run. If one
   does, each function fails the way GDI reports failure, so callers take their error paths.

   On 32-bit ARM every Windows API uses the same calling convention (arguments in r0-r3 and on the
   stack, caller cleans up), so one function per failure value serves any signature. */
#include <windows.h>

#define RFX_FAIL(name, fn) void *__imp_##name = (void *)fn

/* Most GDI calls: 0, NULL or FALSE */
static UINT_PTR WINAPI rfx_gdi_fail(void)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

/* GDI_ERROR / CLR_INVALID */
static DWORD WINAPI rfx_gdi_error(void)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return GDI_ERROR;
}

RFX_FAIL(AddFontMemResourceEx, rfx_gdi_fail);
RFX_FAIL(BitBlt, rfx_gdi_fail);
RFX_FAIL(CreateCompatibleBitmap, rfx_gdi_fail);
RFX_FAIL(CreateCompatibleDC, rfx_gdi_fail);
RFX_FAIL(CreateDIBSection, rfx_gdi_fail);
RFX_FAIL(CreateFontIndirectW, rfx_gdi_fail);
RFX_FAIL(CreatePatternBrush, rfx_gdi_fail);
RFX_FAIL(CreateSolidBrush, rfx_gdi_fail);
RFX_FAIL(EnumFontFamiliesExW, rfx_gdi_fail);
RFX_FAIL(ExtSelectClipRgn, rfx_gdi_fail);       /* ERROR (0) */
RFX_FAIL(ExtTextOutW, rfx_gdi_fail);
RFX_FAIL(FillRect, rfx_gdi_fail);
RFX_FAIL(GdiFlush, rfx_gdi_fail);
RFX_FAIL(GetCharABCWidthsW, rfx_gdi_fail);
RFX_FAIL(GetCharWidth32A, rfx_gdi_fail);
RFX_FAIL(GetClientRect, rfx_gdi_fail);
RFX_FAIL(GetClipBox, rfx_gdi_fail);
RFX_FAIL(GetClipRgn, rfx_gdi_fail);             /* 0 = no clip region */
RFX_FAIL(GetFontData, rfx_gdi_error);
RFX_FAIL(GetFontUnicodeRanges, rfx_gdi_fail);
RFX_FAIL(GetGlyphIndicesW, rfx_gdi_error);
RFX_FAIL(GetGlyphOutlineW, rfx_gdi_error);
RFX_FAIL(GetGraphicsMode, rfx_gdi_fail);
RFX_FAIL(GetObjectW, rfx_gdi_fail);
RFX_FAIL(GetOutlineTextMetricsA, rfx_gdi_fail);
RFX_FAIL(GetOutlineTextMetricsW, rfx_gdi_fail);
RFX_FAIL(GetTextExtentPointI, rfx_gdi_fail);
RFX_FAIL(GetTextFaceW, rfx_gdi_fail);
RFX_FAIL(GetTextMetricsA, rfx_gdi_fail);
RFX_FAIL(GetTextMetricsW, rfx_gdi_fail);
RFX_FAIL(GetWorldTransform, rfx_gdi_fail);
RFX_FAIL(ModifyWorldTransform, rfx_gdi_fail);
RFX_FAIL(PatBlt, rfx_gdi_fail);
RFX_FAIL(RemoveFontMemResourceEx, rfx_gdi_fail);
RFX_FAIL(RestoreDC, rfx_gdi_fail);
RFX_FAIL(SaveDC, rfx_gdi_fail);
RFX_FAIL(SetBkMode, rfx_gdi_fail);
RFX_FAIL(SetBrushOrgEx, rfx_gdi_fail);
RFX_FAIL(SetGraphicsMode, rfx_gdi_fail);
RFX_FAIL(SetMapMode, rfx_gdi_fail);
RFX_FAIL(SetStretchBltMode, rfx_gdi_fail);
RFX_FAIL(SetTextAlign, rfx_gdi_error);
RFX_FAIL(SetTextColor, rfx_gdi_error);          /* CLR_INVALID */
RFX_FAIL(SetWorldTransform, rfx_gdi_fail);
RFX_FAIL(StretchBlt, rfx_gdi_fail);
RFX_FAIL(StretchDIBits, rfx_gdi_fail);
RFX_FAIL(GetCharWidthI, rfx_gdi_fail);
RFX_FAIL(GetDIBits, rfx_gdi_fail);
RFX_FAIL(GetTextExtentPoint32W, rfx_gdi_fail);
RFX_FAIL(GetViewportOrgEx, rfx_gdi_fail);
RFX_FAIL(SetViewportOrgEx, rfx_gdi_fail);
RFX_FAIL(GetICMProfileW, rfx_gdi_fail);
RFX_FAIL(GetIconInfo, rfx_gdi_fail);
RFX_FAIL(DestroyIcon, rfx_gdi_fail);

/* Printing: a job that can't start (StartDoc returns <= 0) */
RFX_FAIL(StartDocW, rfx_gdi_fail);
RFX_FAIL(StartPage, rfx_gdi_fail);
RFX_FAIL(EndPage, rfx_gdi_fail);
RFX_FAIL(EndDoc, rfx_gdi_fail);
RFX_FAIL(AbortDoc, rfx_gdi_fail);

/* Uniscribe is linked by name (usp10.h doesn't declare it dllimport) and returns an HRESULT */
HRESULT WINAPI ScriptFreeCache(void) { return E_NOTIMPL; }
HRESULT WINAPI ScriptItemize(void) { return E_NOTIMPL; }
HRESULT WINAPI ScriptShape(void) { return E_NOTIMPL; }
HRESULT WINAPI ScriptBreak(void) { return E_NOTIMPL; }
HRESULT WINAPI ScriptGetCMap(void) { return E_NOTIMPL; }
