/* retrofox: force-included (-FI) in every phone build before any Windows header.
   We compile against the desktop Windows 8.1 headers and let the link step against the
   phone's API set (WindowsPhoneCore.lib + phone-extra.def) decide what really exists. */
#ifndef RETROFOX_PHONE_H
#define RETROFOX_PHONE_H

/* MSVC's armintr.h names the user-mode TEB register (TPIDRURW) for _MoveFromCoprocessor;
   clang only has the 5-argument builtin, so spell out coproc, opc1, CRn, CRm, opc2. */
/* winnt.h picks the architecture from _ARM_, which only windows.h sets; some files include
   winnt.h-based headers (processthreadsapi.h, ...) directly. */
#if defined(_M_ARM) && !defined(_ARM_)
#define _ARM_ 1
#endif

#ifndef CP15_TPIDRURW
#define CP15_TPIDRURW 15, 0, 13, 0, 2
#endif

#endif
