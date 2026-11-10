/* retrofox step 2: a TLS 1.2/1.3 client built on NSS, running natively on the phone.
   Connects, verifies the server against Mozilla's root store (nssckbi.dll next to the exe),
   sends an HTTP GET and prints the protocol, cipher and HTTP status line.
   Usage: tlstest.exe [host] [port]    (default www.mozilla.org 443) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "nspr.h"
#include "nss.h"
#include "pk11pub.h"
#include "secmod.h"
#include "ssl.h"
#include "sslproto.h"
#include "secerr.h"
#include "sslerr.h"
#include "cert.h"

static void fail(const char *what)
{
    PRErrorCode err = PR_GetError();
    const char *name = PORT_ErrorToName(err);
    printf("FAILED: %s: %s (%d, OS error %d)\n", what, name ? name : "?", (int)err,
           (int)PR_GetOSError());
}

static const char *version_name(PRUint16 v)
{
    switch (v) {
    case SSL_LIBRARY_VERSION_TLS_1_0: return "TLS 1.0";
    case SSL_LIBRARY_VERSION_TLS_1_1: return "TLS 1.1";
    case SSL_LIBRARY_VERSION_TLS_1_2: return "TLS 1.2";
    case SSL_LIBRARY_VERSION_TLS_1_3: return "TLS 1.3";
    default: return "?";
    }
}

/* Directory of this exe, so nssckbi.dll is found wherever the folder lives. Backslashes are
   doubled: NSS's module-spec parser treats '\' inside quotes as an escape, and NSS loads modules
   with LOAD_WITH_ALTERED_SEARCH_PATH, which needs real backslashes. */
static void exe_dir(char *buf, size_t len)
{
    char path[MAX_PATH], *p, *slash;
    size_t n = 0;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    slash = strrchr(path, '\\');
    if (slash)
        slash[1] = '\0';
    for (p = path; *p && n + 2 < len; p++) {
        if (*p == '\\')
            buf[n++] = '\\';
        buf[n++] = *p;
    }
    buf[n] = '\0';
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "www.mozilla.org";
    PRUint16 port = (PRUint16)(argc > 2 ? atoi(argv[2]) : 443);
    char dir[MAX_PATH], spec[MAX_PATH + 64], buf[4096];
    PRAddrInfo *ai;
    PRNetAddr addr;
    PRFileDesc *tcp, *ssl;
    SECMODModule *roots;
    SSLChannelInfo channel;
    SSLCipherSuiteInfo suite;
    SSLVersionRange range = { SSL_LIBRARY_VERSION_TLS_1_2, SSL_LIBRARY_VERSION_TLS_1_3 };
    CERTCertificate *cert;
    PRIntervalTime start;
    int n, total = 0;

    PR_Init(PR_USER_THREAD, PR_PRIORITY_NORMAL, 1);
    if (NSS_NoDB_Init(NULL) != SECSuccess) { fail("NSS_NoDB_Init"); return 1; }
    printf("NSS %s\n", NSS_GetVersion());

    exe_dir(dir, sizeof(dir));
    snprintf(spec, sizeof(spec), "name=\"Mozilla Root Certs\" library=\"%snssckbi.dll\"", dir);
    roots = SECMOD_LoadUserModule(spec, NULL, PR_FALSE);
    if (!roots || !roots->loaded) {
        WCHAR w[MAX_PATH];
        char plain[MAX_PATH];
        HMODULE h;
        fail("loading nssckbi.dll");
        printf("spec: %s\n", spec);
        GetModuleFileNameA(NULL, plain, MAX_PATH);
        strcpy(strrchr(plain, '\\') + 1, "nssckbi.dll");
        MultiByteToWideChar(CP_ACP, 0, plain, -1, w, MAX_PATH);
        h = LoadLibraryExW(w, NULL, 0);
        printf("LoadLibraryExW(%s, 0): %p err %lu\n", plain, (void *)h, h ? 0 : GetLastError());
        h = LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        printf("LoadLibraryExW(.., ALTERED): %p err %lu\n", (void *)h, h ? 0 : GetLastError());
        return 1;
    }
    printf("root store: %s\n", roots->commonName);

    if (NSS_SetDomesticPolicy() != SECSuccess) { fail("NSS_SetDomesticPolicy"); return 1; }
    SSL_VersionRangeSetDefault(ssl_variant_stream, &range);

    start = PR_IntervalNow();
    ai = PR_GetAddrInfoByName(host, PR_AF_UNSPEC, PR_AI_ADDRCONFIG);
    if (!ai || !PR_EnumerateAddrInfo(NULL, ai, port, &addr)) { fail("DNS lookup"); return 1; }
    PR_FreeAddrInfo(ai);
    PR_NetAddrToString(&addr, buf, sizeof(buf));
    printf("connecting to %s (%s) port %u\n", host, buf, port);

    tcp = PR_OpenTCPSocket(addr.raw.family);
    if (!tcp || PR_Connect(tcp, &addr, PR_SecondsToInterval(20)) != PR_SUCCESS) { fail("TCP connect"); return 1; }

    ssl = SSL_ImportFD(NULL, tcp);
    if (!ssl) { fail("SSL_ImportFD"); return 1; }
    SSL_SetURL(ssl, host);                      /* SNI + hostname check */
    SSL_OptionSet(ssl, SSL_HANDSHAKE_AS_CLIENT, PR_TRUE);
    SSL_ResetHandshake(ssl, PR_FALSE);
    if (SSL_ForceHandshake(ssl) != SECSuccess) { fail("TLS handshake"); return 1; }

    SSL_GetChannelInfo(ssl, &channel, sizeof(channel));
    SSL_GetCipherSuiteInfo(channel.cipherSuite, &suite, sizeof(suite));
    printf("protocol: %s\n", version_name(channel.protocolVersion));
    printf("cipher: %s\n", suite.cipherSuiteName);
    cert = SSL_PeerCertificate(ssl);
    if (cert) {
        printf("server cert: %s\n", cert->subjectName);
        printf("issued by: %s\n", cert->issuerName);
        CERT_DestroyCertificate(cert);
    }
    printf("handshake time: %u ms\n", PR_IntervalToMilliseconds(PR_IntervalNow() - start));

    snprintf(buf, sizeof(buf),
             "GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: retrofox-tlstest (Lumia)\r\n"
             "Connection: close\r\n\r\n", host);
    if (PR_Write(ssl, buf, (PRInt32)strlen(buf)) < 0) { fail("send"); return 1; }

    while ((n = PR_Read(ssl, buf + total, (PRInt32)(sizeof(buf) - 1 - total))) > 0) {
        total += n;
        if (strstr(buf, "\r\n") || total >= (int)sizeof(buf) - 1)
            break;
    }
    if (total <= 0) { fail("receive"); return 1; }
    buf[total] = '\0';
    *strstr(buf, "\r\n") = '\0';
    printf("HTTP: %s\n", buf);

    PR_Close(ssl);
    SECMOD_DestroyModule(roots);
    NSS_Shutdown();
    PR_Cleanup();
    return 0;
}
