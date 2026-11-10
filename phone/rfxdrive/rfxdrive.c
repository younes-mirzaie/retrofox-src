/* rfxdrive: drives rfxengine on the phone like the Retrofox app does (viewer/PROTOCOL.md), for
 * testing from the PC through rfxdev:  rfx run "rfxdrive.exe <steps>"
 *
 *   rfxdrive [--port N] [--size WxH] [--scale P] step...
 *     nav URL          open a page               wait MS        let it run
 *     load [MS]        wait until a load ends (default up to 30 s)
 *     scroll DX DY [N] [MS]   send N scroll steps (default 1), MS apart (default 16)
 *     tap X Y          touch down + up           text STR       type text
 *     at X Y           put the finger there for the scrolling that follows (as a pan starts)
 *     drag X1 Y1 X2 Y2 [N]   a finger drag in N steps (default 10): down, moves with scrolls, up
 *     key VK           press a key (Windows virtual-key code, e.g. 13 Enter)
 *     cmd N            0 reload, 1 back, 2 forward, 3 stop, 4 restart the engine (ends rfxdrive)
 *     shot FILE.bmp    save the picture the app would show
 *     tab new [URL] | tab select ID | tab close ID
 *
 * Prints what the engine sends (state, tabs, focus, scroll, frame counts) as it arrives. It takes
 * the engine's one app connection while it runs (close Retrofox first: the app takes it back). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SOCKET sSock = INVALID_SOCKET;
static int sW = 480, sH = 696, sScale = 120;
static uint8_t* sSurface;
static uint32_t sFrames, sLastSerial;
static uint64_t sFrameBytes;
static int sLoading = -1, sLoadsDone;
static DWORD sStart;

static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
static double Now() { return (GetTickCount() - sStart) / 1000.0; }

static int SendAll(const void* d, int n)
{
  const char* p = (const char*)d;
  while (n > 0) {
    int k = send(sSock, p, n, 0);
    if (k <= 0) return 0;
    p += k; n -= k;
  }
  return 1;
}

static int SendMsg(uint16_t type, const void* payload, uint32_t len)
{
  uint8_t h[8];
  wr16(h, type); wr16(h + 2, 0); wr32(h + 4, len);
  return SendAll(h, 8) && (len == 0 || SendAll(payload, (int)len));
}

static int RecvAll(void* d, int n)
{
  char* p = (char*)d;
  while (n > 0) {
    int k = recv(sSock, p, n, 0);
    if (k <= 0) return 0;
    p += k; n -= k;
  }
  return 1;
}

/* Reads and handles everything the engine sends for up to ms milliseconds */
static int Pump(int ms)
{
  DWORD until = GetTickCount() + ms;
  for (;;) {
    int left = (int)(until - GetTickCount());
    if (left < 0) left = 0;
    fd_set r; FD_ZERO(&r); FD_SET(sSock, &r);
    struct timeval tv = { left / 1000, (left % 1000) * 1000 };
    int n = select(0, &r, NULL, NULL, &tv);
    if (n < 0) return 0;
    if (n == 0) return 1;
    uint8_t h[8];
    if (!RecvAll(h, 8)) { printf("%7.2f engine closed the connection\n", Now()); return 0; }
    uint16_t type = rd16(h);
    uint32_t len = rd32(h + 4);
    if (len > 16 * 1024 * 1024) return 0;
    uint8_t* p = (uint8_t*)malloc(len + 1);
    if (len && !RecvAll(p, (int)len)) { free(p); return 0; }
    p[len] = 0;
    if (type == 2 && len >= 20) {               /* FRAME */
      uint32_t serial = rd32(p);
      int x = rd16(p + 8), y = rd16(p + 10), w = rd16(p + 12), hh = rd16(p + 14);
      int sw = rd16(p + 4), sh = rd16(p + 6);
      if (sw == sW && sh == sH && p[16] == 0 && x + w <= sW && y + hh <= sH) {
        for (int row = 0; row < hh; row++)
          memcpy(sSurface + ((size_t)(y + row) * sW + x) * 4, p + 20 + (size_t)row * w * 4, (size_t)w * 4);
      }
      sFrames++; sFrameBytes += len; sLastSerial = serial;
      uint8_t ack[4]; wr32(ack, serial);
      SendMsg(108, ack, 4);
    } else if (type == 3) {                     /* STATE */
      printf("%7.2f state %s\n", Now(), (char*)p);
      int loading = strstr((char*)p, "\"loading\":true") != NULL;
      if (sLoading == 1 && !loading) sLoadsDone++;
      sLoading = loading;
    } else if (type == 4 && len >= 1) {
      printf("%7.2f focus %s\n", Now(), p[0] ? "text field (keyboard on)" : "none");
    } else if (type == 5 && len >= 29) {
      printf("%7.2f scroll frame %u applied %d,%d at %d,%d of %d,%d%s\n", Now(), rd32(p),
             (int32_t)rd32(p + 4), (int32_t)rd32(p + 8), (int32_t)rd32(p + 12), (int32_t)rd32(p + 16),
             (int32_t)rd32(p + 20), (int32_t)rd32(p + 24), p[28] ? " (inner box)" : "");
    } else if (type == 6) {
      printf("%7.2f tabs %s\n", Now(), (char*)p);
    } else if (type == 7 && len >= 1) {
      static const char* names[] = { "none", "undecided", "browser scrolls", "page handles it" };
      printf("%7.2f gesture: %s\n", Now(), p[0] < 4 ? names[p[0]] : "?");
    }
    free(p);
  }
}

static void Shot(const char* path)
{
  FILE* f = fopen(path, "wb");
  if (!f) { printf("can't write %s\n", path); return; }
  uint32_t img = (uint32_t)sW * sH * 4;
  uint8_t h[54] = { 'B', 'M' };
  wr32(h + 2, 54 + img); wr32(h + 10, 54); wr32(h + 14, 40);
  wr32(h + 18, (uint32_t)sW); wr32(h + 22, (uint32_t)-sH);   /* top-down */
  wr16(h + 26, 1); wr16(h + 28, 32); wr32(h + 34, img);
  fwrite(h, 1, 54, f);
  fwrite(sSurface, 1, img, f);
  fclose(f);
  printf("%7.2f saved %s (%dx%d, frame %u)\n", Now(), path, sW, sH, sLastSerial);
}

int main(int argc, char** argv)
{
  int port = 8081, i = 1;
  sStart = GetTickCount();
  setvbuf(stdout, NULL, _IONBF, 0);
  for (; i < argc; i++) {
    if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &sW, &sH);
    else if (!strcmp(argv[i], "--scale") && i + 1 < argc) sScale = atoi(argv[++i]);
    else break;
  }
  sSurface = (uint8_t*)calloc((size_t)sW * sH, 4);
  WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
  sSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  struct sockaddr_in a = { 0 };
  a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(sSock, (struct sockaddr*)&a, sizeof(a)) != 0) {
    printf("can't connect to the engine on port %d (%d)\n", port, WSAGetLastError());
    return 1;
  }
  int one = 1; setsockopt(sSock, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
  uint8_t hello[9];
  wr16(hello, 1); wr16(hello + 2, (uint16_t)sW); wr16(hello + 4, (uint16_t)sH);
  wr16(hello + 6, (uint16_t)sScale); hello[8] = 0;
  SendMsg(100, hello, 9);
  printf("%7.2f connected, %dx%d at %d%%\n", Now(), sW, sH, sScale);
  Pump(500);

  for (; i < argc; i++) {
    const char* s = argv[i];
    if (!strcmp(s, "nav") && i + 1 < argc) {
      const char* u = argv[++i];
      printf("%7.2f nav %s\n", Now(), u);
      SendMsg(101, u, (uint32_t)strlen(u));
      Pump(200);
    } else if (!strcmp(s, "wait") && i + 1 < argc) {
      if (!Pump(atoi(argv[++i]))) return 1;
    } else if (!strcmp(s, "load")) {
      int ms = (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') ? atoi(argv[++i]) : 30000;
      int before = sLoadsDone;
      DWORD until = GetTickCount() + ms;
      while (sLoadsDone == before && (int)(until - GetTickCount()) > 0)
        if (!Pump(100)) return 1;
      printf("%7.2f %s\n", Now(), sLoadsDone > before ? "loaded" : "load wait timed out");
      Pump(500);
    } else if (!strcmp(s, "scroll") && i + 2 < argc) {
      int dx = atoi(argv[++i]), dy = atoi(argv[++i]), n = 1, gap = 16;
      if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') n = atoi(argv[++i]);
      if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') gap = atoi(argv[++i]);
      uint32_t f0 = sFrames; DWORD t0 = GetTickCount();
      for (int k = 0; k < n; k++) {
        uint8_t b[4]; wr16(b, (uint16_t)dx); wr16(b + 2, (uint16_t)dy);
        SendMsg(104, b, 4);
        if (!Pump(gap)) return 1;
      }
      Pump(300);
      DWORD t = GetTickCount() - t0;
      printf("%7.2f scrolled %d x (%d,%d): %u frames in %lu ms (%.1f fps)\n", Now(), n, dx, dy,
             sFrames - f0, t, (sFrames - f0) * 1000.0 / (t ? t : 1));
    } else if (!strcmp(s, "drag") && i + 4 < argc) {
      /* A finger drag as the app sends it: down, then each step a move plus the matching scroll
       * (finger up = page scrolls down), then up */
      int x1 = atoi(argv[++i]), y1 = atoi(argv[++i]), x2 = atoi(argv[++i]), y2 = atoi(argv[++i]);
      int n = 10;
      if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') n = atoi(argv[++i]);
      uint8_t b[6] = { 0 };
      wr16(b + 2, (uint16_t)x1); wr16(b + 4, (uint16_t)y1);
      b[0] = 0; SendMsg(103, b, 6);
      Pump(30);
      int px = x1, py = y1;
      for (int k = 1; k <= n; k++) {
        int x = x1 + (x2 - x1) * k / n, y = y1 + (y2 - y1) * k / n;
        wr16(b + 2, (uint16_t)x); wr16(b + 4, (uint16_t)y);
        b[0] = 1; SendMsg(103, b, 6);
        uint8_t sc[4]; wr16(sc, (uint16_t)(px - x)); wr16(sc + 2, (uint16_t)(py - y));
        SendMsg(104, sc, 4);
        px = x; py = y;
        if (!Pump(16)) return 1;
      }
      b[0] = 2; SendMsg(103, b, 6);
      printf("%7.2f drag %d,%d -> %d,%d in %d steps\n", Now(), x1, y1, x2, y2, n);
      Pump(300);
    } else if (!strcmp(s, "tap") && i + 2 < argc) {
      int x = atoi(argv[++i]), y = atoi(argv[++i]);
      uint8_t b[6] = { 0 };
      wr16(b + 2, (uint16_t)x); wr16(b + 4, (uint16_t)y);
      b[0] = 0; SendMsg(103, b, 6);
      b[0] = 2; SendMsg(103, b, 6);
      printf("%7.2f tap %d,%d\n", Now(), x, y);
      Pump(200);
    } else if (!strcmp(s, "text") && i + 1 < argc) {
      const char* t = argv[++i];
      SendMsg(106, t, (uint32_t)strlen(t));
      Pump(100);
    } else if (!strcmp(s, "key") && i + 1 < argc) {
      uint8_t b[4] = { 0 };
      wr16(b + 2, (uint16_t)atoi(argv[++i]));
      SendMsg(105, b, 4);
      b[0] = 1; SendMsg(105, b, 4);
      Pump(100);
    } else if (!strcmp(s, "at") && i + 2 < argc) {
      /* Where the finger is for the scrolling that follows (what the app sends as a pan starts:
       * POINTER cancel), so it scrolls the box under it */
      int x = atoi(argv[++i]), y = atoi(argv[++i]);
      uint8_t b[6] = { 3, 0 };
      wr16(b + 2, (uint16_t)x); wr16(b + 4, (uint16_t)y);
      SendMsg(103, b, 6);
      Pump(50);
    } else if (!strcmp(s, "cmd") && i + 1 < argc) {
      /* COMMAND: 0 reload, 1 back, 2 forward, 3 stop, 4 restart the engine */
      uint8_t c = (uint8_t)atoi(argv[++i]);
      SendMsg(102, &c, 1);
      printf("%7.2f command %u\n", Now(), c);
      if (c == 4) {
        Pump(1000);
        printf("%7.2f done (engine restarting)\n", Now());
        return 0;
      }
      Pump(200);
    } else if (!strcmp(s, "shot") && i + 1 < argc) {
      Shot(argv[++i]);
    } else if (!strcmp(s, "tab") && i + 1 < argc) {
      const char* what = argv[++i];
      uint8_t b[8 + 512] = { 0 };
      uint32_t len = 8;
      if (!strcmp(what, "new")) {
        b[0] = 0;
        if (i + 1 < argc && strncmp(argv[i + 1], "tab", 3) && strlen(argv[i + 1]) < 512 &&
            strchr(argv[i + 1], ':')) {
          const char* u = argv[++i];
          memcpy(b + 8, u, strlen(u)); len += (uint32_t)strlen(u);
        }
      } else if (i + 1 < argc) {
        b[0] = !strcmp(what, "select") ? 1 : 2;
        wr32(b + 4, (uint32_t)atoi(argv[++i]));
      }
      SendMsg(110, b, len);
      Pump(300);
    } else {
      printf("unknown step: %s\n", s);
      return 2;
    }
  }
  printf("%7.2f done: %u frames, %.1f MB\n", Now(), sFrames, sFrameBytes / 1048576.0);
  closesocket(sSock);
  return 0;
}
