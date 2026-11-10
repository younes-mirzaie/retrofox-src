/* rfxproxy: an HTTPS fixer for IE11 Mobile, running on the phone.

   IE  ->  rfxproxy (127.0.0.1:8080, set as the Wi-Fi proxy)  ->  internet with modern TLS (NSS)

   - CONNECT host:443 (https): IE's TLS is terminated here with a certificate for that host,
     signed by a local "retrofox" CA that the user installs once. The proxy then opens its own
     TLS 1.2/1.3 connection to the site, verified against Mozilla's roots (nssckbi.dll), and
     relays bytes. Certificate errors become an error page instead of a silent pass-through.
   - Plain http:// requests are forwarded as they are.
   - http://retrofox/ca.cer serves the CA certificate so it can be installed from IE.

   The CA key pair is generated on the phone at first start and kept next to the exe
   (rfx-ca.key, rfx-ca.cer). The key never leaves the phone.

   Usage: rfxproxy.exe [port] [bind-address] [--selftest]     (default 8080 127.0.0.1)
          --selftest connects to www.mozilla.org from the main and a worker thread, then exits. */
#include <signal.h>
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
#include "cert.h"
#include "certdb.h"
#include "cryptohi.h"
#include "keyhi.h"
#include "secder.h"
#include "secasn1.h"
#include "secerr.h"
#include "sslerr.h"

#define HEAD_MAX 16384
#define RELAY_BUF 32768
#define IO_TIMEOUT PR_SecondsToInterval(60)

static char gDir[MAX_PATH];             /* exe directory, with trailing backslash */
static CERTCertificate *gCaCert;
static SECKEYPrivateKey *gCaKey;
static SECKEYPrivateKey *gLeafKey;      /* one key for all generated host certs */
static SECKEYPublicKey *gLeafPub;
static PRLock *gCacheLock;
static PRUint16 gPort;                  /* our listening port */

/* Generated host certificates, cached for the life of the process */
typedef struct HostCert {
    char host[256];
    CERTCertificate *cert;
    struct HostCert *next;
} HostCert;
static HostCert *gHostCerts;

static void log_line(const char *fmt, ...)
{
    va_list ap;
    PRExplodedTime t;
    PR_ExplodeTime(PR_Now(), PR_LocalTimeParameters, &t);
    printf("%02d:%02d:%02d ", t.tm_hour, t.tm_min, t.tm_sec);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static const char *err_name(void)
{
    const char *n = PORT_ErrorToName(PR_GetError());
    return n ? n : "unknown error";
}

/* ---------- files ---------- */

static SECStatus read_file(const char *name, SECItem *out)
{
    char path[MAX_PATH];
    FILE *f;
    long len;
    snprintf(path, sizeof(path), "%s%s", gDir, name);
    f = fopen(path, "rb");
    if (!f)
        return SECFailure;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (!SECITEM_AllocItem(NULL, out, (unsigned)len) || fread(out->data, 1, len, f) != (size_t)len) {
        fclose(f);
        return SECFailure;
    }
    fclose(f);
    return SECSuccess;
}

static SECStatus write_file(const char *name, const SECItem *data)
{
    char path[MAX_PATH];
    FILE *f;
    snprintf(path, sizeof(path), "%s%s", gDir, name);
    f = fopen(path, "wb");
    if (!f)
        return SECFailure;
    fwrite(data->data, 1, data->len, f);
    fclose(f);
    return SECSuccess;
}

/* ---------- certificates ---------- */

static SECStatus gen_rsa(SECKEYPrivateKey **priv, SECKEYPublicKey **pub, PRBool extractable)
{
    PK11RSAGenParams params = { 2048, 65537 };
    PK11SlotInfo *slot = PK11_GetInternalSlot();
    PK11AttrFlags attrs = PK11_ATTR_SESSION | PK11_ATTR_PUBLIC |
                          (extractable ? PK11_ATTR_EXTRACTABLE | PK11_ATTR_INSENSITIVE
                                       : PK11_ATTR_SENSITIVE);
    *priv = PK11_GenerateKeyPairWithFlags(slot, CKM_RSA_PKCS_KEY_PAIR_GEN, &params, pub, attrs, NULL);
    PK11_FreeSlot(slot);
    return *priv ? SECSuccess : SECFailure;
}

static SECStatus add_bitstring_ext(void *exts, SECOidTag tag, unsigned char bits, PRBool critical)
{
    PLArenaPool *arena = PORT_NewArena(256);
    SECItem bitstr = { siBuffer, &bits, 8 }, der = { 0 };
    SECStatus rv = SECFailure;
    if (SEC_ASN1EncodeItem(arena, &der, &bitstr, SEC_ASN1_GET(SEC_BitStringTemplate)))
        rv = CERT_AddExtension(exts, tag, &der, critical, PR_TRUE);
    PORT_FreeArena(arena, PR_FALSE);
    return rv;
}

/* Builds and signs a certificate. subject "CN=...", host (or NULL) goes into subjectAltName. */
static CERTCertificate *make_cert(const char *subject, const char *host, SECKEYPublicKey *pub,
                                  PRBool isCA, int days)
{
    CERTName *name = CERT_AsciiToName((char *)subject);
    CERTSubjectPublicKeyInfo *spki = SECKEY_CreateSubjectPublicKeyInfo(pub);
    CERTCertificateRequest *req = CERT_CreateCertificateRequest(name, spki, NULL);
    PRTime now = PR_Now();
    CERTValidity *validity = CERT_CreateValidity(now - (PRTime)86400 * PR_USEC_PER_SEC,
                                                 now + (PRTime)days * 86400 * PR_USEC_PER_SEC);
    CERTName *issuer = isCA ? name : &gCaCert->subject;
    unsigned long serial;
    CERTCertificate *cert = NULL, *tmp;
    void *exts;
    SECItem der = { 0 };
    PLArenaPool *arena;

    PK11_GenerateRandom((unsigned char *)&serial, sizeof(serial));
    serial = (serial & 0x7fffffff) | 1;
    tmp = CERT_CreateCertificate(serial, issuer, validity, req);
    CERT_DestroyValidity(validity);
    CERT_DestroyCertificateRequest(req);
    SECKEY_DestroySubjectPublicKeyInfo(spki);
    CERT_DestroyName(name);
    if (!tmp)
        return NULL;
    arena = tmp->arena;
    *(tmp->version.data) = SEC_CERTIFICATE_VERSION_3;
    tmp->version.len = 1;

    exts = CERT_StartCertExtensions(tmp);
    if (isCA) {
        CERTBasicConstraints bc = { PR_TRUE, CERT_UNLIMITED_PATH_CONSTRAINT };
        SECItem bcder = { 0 };
        CERT_EncodeBasicConstraintValue(arena, &bc, &bcder);
        CERT_AddExtension(exts, SEC_OID_X509_BASIC_CONSTRAINTS, &bcder, PR_TRUE, PR_TRUE);
        add_bitstring_ext(exts, SEC_OID_X509_KEY_USAGE, KU_KEY_CERT_SIGN | KU_CRL_SIGN, PR_TRUE);
    } else {
        CERTGeneralName gn;
        SECItem san = { 0 };
        PRNetAddr ip;
        memset(&gn, 0, sizeof(gn));
        PR_INIT_CLIST(&gn.l);
        if (PR_StringToNetAddr(host, &ip) == PR_SUCCESS && ip.raw.family == PR_AF_INET) {
            gn.type = certIPAddress;
            gn.name.other.data = (unsigned char *)&ip.inet.ip;
            gn.name.other.len = 4;
        } else {
            gn.type = certDNSName;
            gn.name.other.data = (unsigned char *)host;
            gn.name.other.len = (unsigned)strlen(host);
        }
        CERT_EncodeAltNameExtension(arena, &gn, &san);
        CERT_AddExtension(exts, SEC_OID_X509_SUBJECT_ALT_NAME, &san, PR_FALSE, PR_TRUE);
        add_bitstring_ext(exts, SEC_OID_X509_KEY_USAGE,
                          KU_DIGITAL_SIGNATURE | KU_KEY_ENCIPHERMENT, PR_TRUE);
    }
    CERT_FinishExtensions(exts);

    /* Everything is signed by the CA key (the CA certificate signs itself) */
    SECOID_SetAlgorithmID(arena, &tmp->signature, SEC_OID_PKCS1_SHA256_WITH_RSA_ENCRYPTION, NULL);
    if (!SEC_ASN1EncodeItem(arena, &der, tmp, SEC_ASN1_GET(CERT_CertificateTemplate)) ||
        SEC_DerSignData(arena, &tmp->derCert, der.data, der.len, gCaKey,
                        SEC_OID_PKCS1_SHA256_WITH_RSA_ENCRYPTION) != SECSuccess) {
        CERT_DestroyCertificate(tmp);
        return NULL;
    }
    cert = CERT_NewTempCertificate(CERT_GetDefaultCertDB(), &tmp->derCert, NULL, PR_FALSE, PR_TRUE);
    CERT_DestroyCertificate(tmp);
    return cert;
}

/* Loads the CA from rfx-ca.key/rfx-ca.cer, or creates it on first run */
static SECStatus load_or_create_ca(void)
{
    SECItem keyDer = { 0 }, certDer = { 0 };
    PK11SlotInfo *slot = PK11_GetInternalSlot();
    SECStatus rv = SECFailure;

    if (read_file("rfx-ca.key", &keyDer) == SECSuccess &&
        read_file("rfx-ca.cer", &certDer) == SECSuccess) {
        if (PK11_ImportDERPrivateKeyInfoAndReturnKey(slot, &keyDer, NULL, NULL, PR_FALSE, PR_FALSE,
                                                     KU_ALL, &gCaKey, NULL) == SECSuccess) {
            gCaCert = CERT_NewTempCertificate(CERT_GetDefaultCertDB(), &certDer, NULL, PR_FALSE, PR_TRUE);
            if (gCaCert) {
                log_line("CA loaded: %s", gCaCert->subjectName);
                rv = SECSuccess;
            }
        }
    } else {
        SECKEYPublicKey *pub;
        SECItem *pkcs8;
        log_line("creating the retrofox CA (first run, takes a few seconds)...");
        if (gen_rsa(&gCaKey, &pub, PR_TRUE) == SECSuccess) {
            gCaCert = make_cert("CN=retrofox local CA,O=retrofox (this phone only)", NULL, pub, PR_TRUE, 3650);
            pkcs8 = gCaCert ? PK11_ExportDERPrivateKeyInfo(gCaKey, NULL) : NULL;
            if (pkcs8 && write_file("rfx-ca.key", pkcs8) == SECSuccess &&
                write_file("rfx-ca.cer", &gCaCert->derCert) == SECSuccess) {
                log_line("CA created: %s", gCaCert->subjectName);
                rv = SECSuccess;
            }
            if (pkcs8)
                SECITEM_FreeItem(pkcs8, PR_TRUE);
            SECKEY_DestroyPublicKey(pub);
        }
    }
    if (rv != SECSuccess)
        log_line("CA setup failed: %s", err_name());
    SECITEM_FreeItem(&keyDer, PR_FALSE);
    SECITEM_FreeItem(&certDer, PR_FALSE);
    PK11_FreeSlot(slot);
    return rv;
}

static CERTCertificate *cert_for_host(const char *host)
{
    HostCert *h;
    CERTCertificate *cert = NULL;
    char subject[300];

    PR_Lock(gCacheLock);
    for (h = gHostCerts; h; h = h->next) {
        if (!strcmp(h->host, host)) {
            cert = CERT_DupCertificate(h->cert);
            break;
        }
    }
    if (!cert) {
        snprintf(subject, sizeof(subject), "CN=%s", host);
        cert = make_cert(subject, host, gLeafPub, PR_FALSE, 397);
        if (cert) {
            h = calloc(1, sizeof(*h));
            strncpy(h->host, host, sizeof(h->host) - 1);
            h->cert = CERT_DupCertificate(cert);
            h->next = gHostCerts;
            gHostCerts = h;
        }
    }
    PR_Unlock(gCacheLock);
    return cert;
}

/* ---------- I/O helpers ---------- */

static int send_all(PRFileDesc *fd, const char *buf, int len)
{
    while (len > 0) {
        int n = PR_Send(fd, buf, len, 0, IO_TIMEOUT);
        if (n <= 0)
            return -1;
        buf += n;
        len -= n;
    }
    return 0;
}

/* Reads up to the end of the HTTP head ("\r\n\r\n"). Returns head length, extra bytes after it in *extra. */
static int read_head(PRFileDesc *fd, char *buf, int cap, int *got)
{
    int total = 0;
    char *end;
    while (total < cap - 1) {
        int n = PR_Recv(fd, buf + total, cap - 1 - total, 0, IO_TIMEOUT);
        if (n <= 0)
            return -1;
        total += n;
        buf[total] = '\0';
        if ((end = strstr(buf, "\r\n\r\n")) != NULL) {
            *got = total;
            return (int)(end - buf) + 4;
        }
    }
    return -1;
}

/* Copies bytes both ways until either side closes */
static void relay(PRFileDesc *a, PRFileDesc *b)
{
    char *buf = malloc(RELAY_BUF);
    PRPollDesc pd[2];
    for (;;) {
        int i;
        pd[0].fd = a; pd[0].in_flags = PR_POLL_READ; pd[0].out_flags = 0;
        pd[1].fd = b; pd[1].in_flags = PR_POLL_READ; pd[1].out_flags = 0;
        if (PR_Poll(pd, 2, PR_SecondsToInterval(120)) <= 0)
            break;
        for (i = 0; i < 2; i++) {
            if (pd[i].out_flags & (PR_POLL_READ | PR_POLL_ERR | PR_POLL_HUP)) {
                int n = PR_Recv(pd[i].fd, buf, RELAY_BUF, 0, IO_TIMEOUT);
                if (n <= 0 || send_all(pd[1 - i].fd, buf, n) < 0)
                    goto done;
            }
        }
    }
done:
    free(buf);
}

static PRFileDesc *tcp_connect_family(const char *host, PRUint16 port, PRUint16 family)
{
    PRAddrInfo *ai = PR_GetAddrInfoByName(host, family, PR_AI_ADDRCONFIG);
    PRNetAddr addr;
    void *iter = NULL;
    PRFileDesc *fd = NULL;
    if (!ai)
        return NULL;
    while ((iter = PR_EnumerateAddrInfo(iter, ai, port, &addr)) != NULL) {
        fd = PR_OpenTCPSocket(addr.raw.family);
        if (fd && PR_Connect(fd, &addr, PR_SecondsToInterval(10)) == PR_SUCCESS)
            break;
        if (fd)
            PR_Close(fd);
        fd = NULL;
    }
    PR_FreeAddrInfo(ai);
    return fd;
}

/* IPv4 first: phones on Wi-Fi often get AAAA records without a working IPv6 route */
static PRFileDesc *tcp_connect(const char *host, PRUint16 port)
{
    PRFileDesc *fd = tcp_connect_family(host, port, PR_AF_INET);
    return fd ? fd : tcp_connect_family(host, port, PR_AF_INET6);
}

static void send_error_page(PRFileDesc *fd, int code, const char *title, const char *host, const char *detail)
{
    char body[2048], head[512];
    snprintf(body, sizeof(body),
             "<html><head><meta name='viewport' content='width=device-width'><title>%s</title></head>"
             "<body style='font-family:Segoe UI,sans-serif;padding:12px'><h2>%s</h2>"
             "<p><b>%s</b></p><p>%s</p><p style='color:#888'>retrofox proxy</p></body></html>",
             title, title, host, detail);
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\n"
             "Connection: close\r\n\r\n", code, title, (int)strlen(body));
    send_all(fd, head, (int)strlen(head));
    send_all(fd, body, (int)strlen(body));
}

/* ---------- request handling ---------- */

static void serve_ca(PRFileDesc *fd)
{
    char head[256];
    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\nContent-Type: application/x-x509-ca-cert\r\n"
             "Content-Disposition: attachment; filename=retrofox-ca.cer\r\n"
             "Content-Length: %u\r\nConnection: close\r\n\r\n", gCaCert->derCert.len);
    send_all(fd, head, (int)strlen(head));
    send_all(fd, (const char *)gCaCert->derCert.data, (int)gCaCert->derCert.len);
    log_line("served the CA certificate");
}

/* CONNECT host:port -> TLS to IE with a local cert, TLS to the site with real checks */
static void handle_connect(PRFileDesc *client, const char *target)
{
    char host[256];
    const char *colon = strrchr(target, ':');
    PRUint16 port = 443;
    PRFileDesc *up = NULL, *upTls = NULL, *inTls = NULL;
    CERTCertificate *cert = NULL;
    SSLVersionRange upRange = { SSL_LIBRARY_VERSION_TLS_1_2, SSL_LIBRARY_VERSION_TLS_1_3 };
    SSLVersionRange inRange = { SSL_LIBRARY_VERSION_TLS_1_0, SSL_LIBRARY_VERSION_TLS_1_2 };
    PRErrorCode upErr = 0;
    PRIntervalTime start = PR_IntervalNow();

    snprintf(host, sizeof(host), "%.*s", colon ? (int)(colon - target) : (int)strlen(target), target);
    if (colon)
        port = (PRUint16)atoi(colon + 1);

    if (send_all(client, "HTTP/1.1 200 Connection established\r\n\r\n", 39) < 0)
        return;

    /* IE's side: present a certificate for this host signed by the retrofox CA */
    cert = cert_for_host(host);
    inTls = cert ? SSL_ImportFD(NULL, client) : NULL;
    if (!inTls || SSL_ConfigServerCert(inTls, cert, gLeafKey, NULL, 0) != SECSuccess) {
        log_line("CONNECT %s: local certificate failed: %s", target, err_name());
        goto out;
    }
    SSL_VersionRangeSet(inTls, &inRange);
    /* No server session cache: it's shared-memory based, and IE talks to us over loopback anyway */
    SSL_OptionSet(inTls, SSL_NO_CACHE, PR_TRUE);
    SSL_ResetHandshake(inTls, PR_TRUE);
    if (SSL_ForceHandshake(inTls) != SECSuccess) {
        /* Usually: the CA isn't installed in IE yet */
        log_line("CONNECT %s: IE refused the local certificate (%s). Is the retrofox CA installed?",
                 target, err_name());
        goto out;
    }

    /* The site's side: modern TLS, verified against Mozilla's roots */
    up = tcp_connect(host, port);
    if (up) {
        upTls = SSL_ImportFD(NULL, up);
        SSL_SetURL(upTls, host);
        SSL_OptionSet(upTls, SSL_HANDSHAKE_AS_CLIENT, PR_TRUE);
        SSL_VersionRangeSet(upTls, &upRange);
        SSL_ResetHandshake(upTls, PR_FALSE);
        if (SSL_ForceHandshake(upTls) != SECSuccess)
            upErr = PR_GetError();
    } else {
        upErr = PR_GetError();
    }
    if (!up || upErr) {
        const char *name = PORT_ErrorToName(upErr);
        log_line("CONNECT %s: upstream failed: %s", target, name ? name : "?");
        {
            /* Read IE's request inside the tunnel, then answer it with the error page */
            char *req = malloc(HEAD_MAX);
            int got = 0;
            read_head(inTls, req, HEAD_MAX, &got);
            free(req);
        }
        send_error_page(inTls, 502,
                        upErr == SEC_ERROR_EXPIRED_CERTIFICATE || upErr == SSL_ERROR_BAD_CERT_DOMAIN ||
                        upErr == SEC_ERROR_UNKNOWN_ISSUER || upErr == SEC_ERROR_CA_CERT_INVALID ||
                        upErr == SEC_ERROR_UNTRUSTED_ISSUER
                            ? "Insecure connection blocked" : "Can't reach this site",
                        host, name ? name : "connection failed");
        goto out;
    }
    {
        SSLChannelInfo ch;
        SSLCipherSuiteInfo cs;
        SSL_GetChannelInfo(upTls, &ch, sizeof(ch));
        SSL_GetCipherSuiteInfo(ch.cipherSuite, &cs, sizeof(cs));
        log_line("CONNECT %s: %s %s (%u ms)", target,
                 ch.protocolVersion == SSL_LIBRARY_VERSION_TLS_1_3 ? "TLS1.3" : "TLS1.2",
                 cs.cipherSuiteName, PR_IntervalToMilliseconds(PR_IntervalNow() - start));
    }
    relay(inTls, upTls);

out:
    if (cert)
        CERT_DestroyCertificate(cert);
    if (upTls)
        PR_Close(upTls);
    else if (up)
        PR_Close(up);
    if (inTls)
        PR_Close(inTls);    /* closes client too */
    else
        PR_Close(client);
}

/* GET http://host[:port]/path ... -> forward to the site as origin-form */
static void handle_http(PRFileDesc *client, char *head, int headLen, int got)
{
    char method[16], url[2048], version[16], host[256], *path, *rest;
    PRUint16 port = 80;
    PRFileDesc *up;
    char line[2200];

    if (sscanf(head, "%15s %2047s %15s", method, url, version) != 3) {
        send_error_page(client, 400, "Bad request", "", "rfxproxy only proxies http:// and CONNECT.");
        PR_Close(client);
        return;
    }
    /* Opened directly (http://127.0.0.1:8080/...), not as a proxy: serve the CA certificate */
    if (url[0] == '/') {
        serve_ca(client);
        PR_Close(client);
        return;
    }
    if (strncmp(url, "http://", 7)) {
        send_error_page(client, 400, "Bad request", "", "rfxproxy only proxies http:// and CONNECT.");
        PR_Close(client);
        return;
    }
    path = strchr(url + 7, '/');
    snprintf(host, sizeof(host), "%.*s", path ? (int)(path - (url + 7)) : (int)strlen(url + 7), url + 7);
    if (!path)
        path = "/";
    if (strchr(host, ':')) {
        port = (PRUint16)atoi(strchr(host, ':') + 1);
        *strchr(host, ':') = '\0';
    }

    /* The CA download: http://retrofox/, http://retrofox.local/, or the proxy's own address */
    if (!_stricmp(host, "retrofox") || !_stricmp(host, "retrofox.local") ||
        ((!strcmp(host, "127.0.0.1") || !_stricmp(host, "localhost")) && port == gPort)) {
        serve_ca(client);
        PR_Close(client);
        return;
    }

    up = tcp_connect(host, port);
    if (!up) {
        send_error_page(client, 502, "Can't reach this site", host, err_name());
        PR_Close(client);
        return;
    }
    log_line("HTTP %s %s", method, url);
    rest = strstr(head, "\r\n");
    snprintf(line, sizeof(line), "%s %s %s", method, path, version);
    if (send_all(up, line, (int)strlen(line)) == 0 &&
        send_all(up, rest, got - (int)(rest - head)) == 0)
        relay(client, up);
    (void)headLen;
    PR_Close(up);
    PR_Close(client);
}

static void handle_client(void *arg)
{
    PRFileDesc *client = arg;
    char *head = malloc(HEAD_MAX);
    int got = 0, len = read_head(client, head, HEAD_MAX, &got);
    if (len < 0) {
        PR_Close(client);
    } else if (!strncmp(head, "CONNECT ", 8)) {
        char target[300];
        sscanf(head + 8, "%299s", target);
        handle_connect(client, target);
    } else {
        handle_http(client, head, len, got);
    }
    free(head);
}

/* ---------- main ---------- */

static void selftest_thread(void *arg)
{
    PRFileDesc *t;
    (void)arg;
    log_line("selftest: worker thread DNS...");
    {
        PRAddrInfo *ai = PR_GetAddrInfoByName("www.mozilla.org", PR_AF_UNSPEC, PR_AI_ADDRCONFIG);
        log_line("selftest: worker thread DNS %s", ai ? "ok" : err_name());
        if (ai)
            PR_FreeAddrInfo(ai);
    }
    t = tcp_connect("www.mozilla.org", 443);
    log_line("selftest: worker thread connect %s", t ? "ok" : err_name());
    if (t)
        PR_Close(t);
}

/* Report CRT aborts and invalid-parameter traps instead of dying silently (0xC0000409) */
static void on_abort(int sig)
{
    log_line("FATAL: abort() (signal %d)", sig);
}

static void on_invalid_parameter(const wchar_t *expr, const wchar_t *func, const wchar_t *file,
                                 unsigned int line, uintptr_t reserved)
{
    log_line("FATAL: CRT invalid parameter (%ls in %ls, %ls:%u)", expr ? expr : L"?",
             func ? func : L"?", file ? file : L"?", line);
}

int main(int argc, char **argv)
{
    PRUint16 port = gPort = (PRUint16)(argc > 1 ? atoi(argv[1]) : 8080);
    const char *bindAddr = argc > 2 ? argv[2] : "127.0.0.1";
    char spec[MAX_PATH * 2 + 64], *p, *q;
    PRFileDesc *listener;
    PRNetAddr addr;
    PRSocketOptionData opt;
    SECMODModule *roots;

    signal(SIGABRT, on_abort);
    _set_invalid_parameter_handler(on_invalid_parameter);
    PR_Init(PR_USER_THREAD, PR_PRIORITY_NORMAL, 1);
    GetModuleFileNameA(NULL, gDir, MAX_PATH);
    strrchr(gDir, '\\')[1] = '\0';

    if (NSS_NoDB_Init(NULL) != SECSuccess) { log_line("NSS init failed: %s", err_name()); return 1; }
    /* NSS's module spec parser treats '\' as an escape: double them */
    for (p = gDir, q = spec + sprintf(spec, "name=\"Mozilla Root Certs\" library=\""); *p; p++) {
        if (*p == '\\')
            *q++ = '\\';
        *q++ = *p;
    }
    strcpy(q, "nssckbi.dll\"");
    roots = SECMOD_LoadUserModule(spec, NULL, PR_FALSE);
    if (!roots || !roots->loaded) { log_line("can't load nssckbi.dll: %s", err_name()); return 1; }
    NSS_SetDomesticPolicy();
    gCacheLock = PR_NewLock();

    if (load_or_create_ca() != SECSuccess)
        return 1;
    if (gen_rsa(&gLeafKey, &gLeafPub, PR_FALSE) != SECSuccess) {
        log_line("host key generation failed: %s", err_name());
        return 1;
    }

    if (argc > 3 && !strcmp(argv[3], "--selftest")) {
        PRFileDesc *t;
        PRThread *th;
        log_line("selftest: main thread connect...");
        t = tcp_connect("www.mozilla.org", 443);
        log_line("selftest: main thread connect %s", t ? "ok" : err_name());
        if (t)
            PR_Close(t);
        th = PR_CreateThread(PR_USER_THREAD, selftest_thread, NULL, PR_PRIORITY_NORMAL,
                             PR_GLOBAL_THREAD, PR_JOINABLE_THREAD, 1024 * 1024);
        PR_JoinThread(th);
        log_line("selftest: done");
        return 0;
    }

    listener = PR_OpenTCPSocket(PR_AF_INET);
    opt.option = PR_SockOpt_Reuseaddr;
    opt.value.reuse_addr = PR_TRUE;
    PR_SetSocketOption(listener, &opt);
    PR_StringToNetAddr(bindAddr, &addr);
    addr.inet.port = PR_htons(port);
    if (PR_Bind(listener, &addr) != PR_SUCCESS || PR_Listen(listener, 32) != PR_SUCCESS) {
        log_line("can't listen on %s:%u: %s", bindAddr, port, err_name());
        return 1;
    }
    log_line("rfxproxy listening on %s:%u (NSS %s)", bindAddr, port, NSS_GetVersion());

    for (;;) {
        PRNetAddr peer, self;
        PRFileDesc *client = PR_Accept(listener, &peer, PR_INTERVAL_NO_TIMEOUT);
        if (!client)
            continue;
        /* Only this phone may use the proxy: the peer must be loopback or one of our own
           addresses (IE can't reach 127.0.0.1 from its AppContainer, so it may come in on the
           Wi-Fi address). Anyone else on the network is refused. */
        if (PR_GetSockName(client, &self) == PR_SUCCESS && peer.raw.family == PR_AF_INET &&
            peer.inet.ip != PR_htonl(PR_INADDR_LOOPBACK) && peer.inet.ip != self.inet.ip) {
            char who[64];
            PR_NetAddrToString(&peer, who, sizeof(who));
            log_line("refused connection from %s (not this phone)", who);
            PR_Close(client);
            continue;
        }
        if (!PR_CreateThread(PR_USER_THREAD, handle_client, client, PR_PRIORITY_NORMAL,
                             PR_GLOBAL_THREAD, PR_UNJOINABLE_THREAD, 1024 * 1024))
            PR_Close(client);
    }
}
