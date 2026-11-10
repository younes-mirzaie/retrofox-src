/* retrofox shim: the phone kit has no winsdkver.h and the 8.1 one lacks WINVER_MAXVER,
   which UXP's configure uses to check the SDK targets at least Windows 8.1 (0x0603). */
#ifndef RETROFOX_WINSDKVER_H
#define RETROFOX_WINSDKVER_H

#define _WIN32_MAXVER        0x0603
#define _WIN32_WINDOWS_MAXVER 0x0603
#define NTDDI_MAXVER         0x06030000
#define _WIN32_IE_MAXVER     0x0A00
#define _WIN32_WINNT_MAXVER  0x0603
#define WINVER_MAXVER        0x0603

#endif
