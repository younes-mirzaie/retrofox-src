/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "rfxview.h"

#include <string.h>
#include <chrono>
static double NowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define RFX_CLOSE closesocket
#define RFX_BAD ((intptr_t)INVALID_SOCKET)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define RFX_CLOSE close
#define RFX_BAD ((intptr_t)-1)
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace rfxview {

static const size_t kMaxPayload = 16 * 1024 * 1024;
static const uint32_t kMaxFramesInFlight = 2;

static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { wr16(p, uint16_t(v)); wr16(p + 2, uint16_t(v >> 16)); }

Server::Server()
  : mListen(RFX_BAD), mClient(RFX_BAD), mHelloDone(false), mWidth(0), mHeight(0),
    mFormat(kFormatBGRA), mSerial(0), mAcked(0), mDirty(false),
    mDirtyX0(0), mDirtyY0(0), mDirtyX1(0), mDirtyY1(0),
    mScrollSent(), mScrollSentSerial(0xFFFFFFFFu)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

Server::~Server()
{
    CloseClient();
    if (mListen != RFX_BAD)
        RFX_CLOSE((int)mListen);
}

bool Server::Listen(uint16_t port)
{
    intptr_t s = (intptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == RFX_BAD) { mError = "socket() failed"; return false; }
    int one = 1;
    setsockopt((int)s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind((int)s, (sockaddr*)&a, sizeof(a)) != 0 || listen((int)s, 4) != 0) {
        mError = "can't listen on port " + std::to_string(port);
        RFX_CLOSE((int)s);
        return false;
    }
    mListen = s;
    return true;
}

bool Server::Connected() const { return mClient != RFX_BAD && mHelloDone; }

void Server::CloseClient()
{
    if (mClient != RFX_BAD)
        RFX_CLOSE((int)mClient);
    mClient = RFX_BAD;
    mIn.clear();
    mHelloDone = false;
}

void Server::Poll(int timeoutMs, Listener* l)
{
    if (mListen == RFX_BAD)
        return;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET((int)mListen, &rd);
    int maxfd = (int)mListen;
    if (mClient != RFX_BAD) {
        FD_SET((int)mClient, &rd);
        if ((int)mClient > maxfd) maxfd = (int)mClient;
    }
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (select(maxfd + 1, &rd, nullptr, nullptr, &tv) <= 0)
        return;

    if (FD_ISSET((int)mListen, &rd)) {
        sockaddr_in peer, self;
        socklen_t pl = sizeof(peer), sl = sizeof(self);
        intptr_t c = (intptr_t)accept((int)mListen, (sockaddr*)&peer, &pl);
        if (c != RFX_BAD) {
            // Only this phone: loopback, or our own address (see PROTOCOL.md).
            bool local = getsockname((int)c, (sockaddr*)&self, &sl) == 0 &&
                         (peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) ||
                          peer.sin_addr.s_addr == self.sin_addr.s_addr);
            if (!local) {
                mError = "refused a connection from another device";
                RFX_CLOSE((int)c);
            } else {
                if (mClient != RFX_BAD && l)
                    l->OnDisconnect();
                CloseClient();
                mClient = c;
                int one = 1;
                setsockopt((int)c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
            }
        }
    }

    if (mClient != RFX_BAD && FD_ISSET((int)mClient, &rd)) {
        uint8_t buf[16384];
        int n = (int)recv((int)mClient, (char*)buf, sizeof(buf), 0);
        if (n <= 0) {
            CloseClient();
            if (l) l->OnDisconnect();
            return;
        }
        mIn.insert(mIn.end(), buf, buf + n);
        size_t off = 0;
        while (mIn.size() - off >= 8) {
            uint16_t type = rd16(&mIn[off]);
            uint32_t len = rd32(&mIn[off + 4]);
            if (len > kMaxPayload) {
                mError = "oversized message";
                CloseClient();
                if (l) l->OnDisconnect();
                return;
            }
            if (mIn.size() - off - 8 < len)
                break;
            Dispatch(type, mIn.data() + off + 8, len, l);
            if (mClient == RFX_BAD) {   // a send failed while answering
                if (l) l->OnDisconnect();
                return;
            }
            off += 8 + len;
        }
        mIn.erase(mIn.begin(), mIn.begin() + off);
    }
}

void Server::Dispatch(uint16_t type, const uint8_t* p, size_t n, Listener* l)
{
    if (!mHelloDone && type != kHello)
        return;
    switch (type) {
    case kHello: {
        if (n < 8) return;
        mWidth = rd16(p + 2);
        mHeight = rd16(p + 4);
        mFormat = n >= 9 && (p[8] == kFormatRGB565 || p[8] == kFormatShared) ? p[8] : kFormatBGRA;
        mHelloDone = true;
        mSerial = mAcked = 0;
        mScrollSentSerial = 0xFFFFFFFFu;   // the new app gets the scroll state again
        uint8_t w[4];
        wr16(w, kProtocolVersion);
        wr16(w + 2, 0);
        std::string name = "rfxengine";
        std::vector<uint8_t> payload(w, w + 4);
        payload.insert(payload.end(), name.begin(), name.end());
        SendMsg(kWelcome, payload.data(), payload.size());
        InvalidateAll();
        if (l) l->OnHello(mWidth, mHeight, rd16(p + 6));
        break;
    }
    case kNavigate: if (l) l->OnNavigate(std::string((const char*)p, n)); break;
    case kCommand:  if (n >= 1 && l) l->OnCommand(p[0]); break;
    case kPointer:  if (n >= 6 && l) l->OnPointer(p[0], int16_t(rd16(p + 2)), int16_t(rd16(p + 4))); break;
    case kScroll:
        stats.scrolls++;
        if (n >= 4 && l) l->OnScroll(int16_t(rd16(p)), int16_t(rd16(p + 2)));
        break;
    case kKey:      if (n >= 4 && l) l->OnKey(p[0], rd16(p + 2)); break;
    case kText:     if (l) l->OnText(std::string((const char*)p, n)); break;
    case kResize:
        if (n < 4) return;
        mWidth = rd16(p);
        mHeight = rd16(p + 2);
        InvalidateAll();
        if (l) l->OnResize(mWidth, mHeight);
        break;
    case kLog:      if (l) l->OnLog(std::string((const char*)p, n)); break;
    case kTab:
        if (n >= 8 && l)
            l->OnTab(p[0], rd32(p + 4), std::string((const char*)p + 8, n - 8));
        break;
    case kFrameAck:
        if (n >= 4) {
            uint32_t s = rd32(p);
            if (s <= mSerial && s > mAcked) {
                mAcked = s;
                stats.acks++;
                stats.ackMs += NowMs() - mSentAt[s % 8];
            }
        }
        break;
    default:
        break;   // unknown: skip
    }
}

void Server::Invalidate(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w <= 0 || h <= 0 || x >= x1 || y >= y1)
        return;
    if (!mDirty) {
        mDirtyX0 = x; mDirtyY0 = y; mDirtyX1 = x1; mDirtyY1 = y1;
        mDirty = true;
    } else {
        if (x < mDirtyX0) mDirtyX0 = x;
        if (y < mDirtyY0) mDirtyY0 = y;
        if (x1 > mDirtyX1) mDirtyX1 = x1;
        if (y1 > mDirtyY1) mDirtyY1 = y1;
    }
}

bool Server::SendFrameIfReady(const Surface& s)
{
    // Shared frames: the app copies each one out of the shared buffer before acknowledging it, so
    // the next one waits for that (it would overwrite the buffer)
    bool shared = mFormat == kFormatShared && mSink;
    uint32_t inFlight = shared ? 1 : kMaxFramesInFlight;
    if (!Connected() || !mDirty)
        return false;
    if (mSerial - mAcked >= inFlight) {
        stats.ackWaits++;
        return false;
    }
    mSentAt[(mSerial + 1) % 8] = NowMs();
    int sw = s.width < mWidth ? s.width : mWidth;
    int sh = s.height < mHeight ? s.height : mHeight;
    int x0 = mDirtyX0, y0 = mDirtyY0;
    int x1 = mDirtyX1 < sw ? mDirtyX1 : sw;
    int y1 = mDirtyY1 < sh ? mDirtyY1 : sh;
    mDirty = false;
    if (x0 >= x1 || y0 >= y1)
        return false;
    int w = x1 - x0, h = y1 - y0;
    if (shared && mSink->Write(s.data, s.stride, sw, sh, x0, y0, w, h)) {
        // Only the rectangle's place and the serial go over the connection
        uint8_t m[8 + 20];
        wr16(m, kFrame);
        wr16(m + 2, 0);
        wr32(m + 4, 20);
        uint8_t* f = m + 8;
        wr32(f, ++mSerial);
        wr16(f + 4, uint16_t(mWidth));
        wr16(f + 6, uint16_t(mHeight));
        wr16(f + 8, uint16_t(x0));
        wr16(f + 10, uint16_t(y0));
        wr16(f + 12, uint16_t(w));
        wr16(f + 14, uint16_t(h));
        f[16] = kFormatShared;
        f[17] = 0;
        wr16(f + 18, 0);
        return SendAll(m, sizeof(m));
    }
    int bpp = mFormat == kFormatRGB565 ? 2 : 4;

    mOut.resize(8 + 20 + size_t(w) * h * bpp);
    uint8_t* m = mOut.data();
    wr16(m, kFrame);
    wr16(m + 2, 0);
    wr32(m + 4, uint32_t(mOut.size() - 8));
    uint8_t* f = m + 8;
    wr32(f, ++mSerial);
    wr16(f + 4, uint16_t(mWidth));
    wr16(f + 6, uint16_t(mHeight));
    wr16(f + 8, uint16_t(x0));
    wr16(f + 10, uint16_t(y0));
    wr16(f + 12, uint16_t(w));
    wr16(f + 14, uint16_t(h));
    f[16] = bpp == 2 ? kFormatRGB565 : kFormatBGRA;
    f[17] = 0;
    wr16(f + 18, 0);
    uint8_t* px = f + 20;
    for (int y = y0; y < y1; y++) {
        const uint8_t* row = s.data + size_t(y) * s.stride + size_t(x0) * 4;
        if (bpp == 4) {
            memcpy(px, row, size_t(w) * 4);
            px += size_t(w) * 4;
        } else {
            for (int x = 0; x < w; x++, row += 4, px += 2) {
                uint16_t v = uint16_t(((row[2] >> 3) << 11) | ((row[1] >> 2) << 5) | (row[0] >> 3));
                wr16(px, v);
            }
        }
    }
    return SendAll(mOut.data(), mOut.size());
}

void Server::SendState(const std::string& json)
{
    if (Connected())
        SendMsg(kState, json.data(), json.size());
}

void Server::SendFocus(bool focused)
{
    uint8_t b = focused ? 1 : 0;
    if (Connected())
        SendMsg(kFocus, &b, 1);
}

void Server::SendGesture(uint8_t gesture)
{
    if (Connected())
        SendMsg(kGesture, &gesture, 1);
}

void Server::SendTabs(const std::string& json)
{
    if (Connected())
        SendMsg(kTabs, json.data(), json.size());
}

void Server::SendScrollInfo(const ScrollInfo& info)
{
    if (!Connected())
        return;
    uint32_t serial = mDirty ? mSerial + 1 : mSerial;
    const ScrollInfo& o = mScrollSent;
    if (serial == mScrollSentSerial && info.appliedX == o.appliedX && info.appliedY == o.appliedY &&
        info.x == o.x && info.y == o.y && info.maxX == o.maxX && info.maxY == o.maxY &&
        info.inner == o.inner)
        return;
    uint8_t b[32];
    wr32(b, serial);
    wr32(b + 4, uint32_t(info.appliedX));
    wr32(b + 8, uint32_t(info.appliedY));
    wr32(b + 12, uint32_t(info.x));
    wr32(b + 16, uint32_t(info.y));
    wr32(b + 20, uint32_t(info.maxX));
    wr32(b + 24, uint32_t(info.maxY));
    b[28] = info.inner ? 1 : 0;
    b[29] = b[30] = b[31] = 0;
    if (SendMsg(kScrollInfo, b, sizeof(b))) {
        mScrollSent = info;
        mScrollSentSerial = serial;
    }
}

bool Server::SendMsg(uint16_t type, const void* payload, size_t length)
{
    uint8_t h[8];
    wr16(h, type);
    wr16(h + 2, 0);
    wr32(h + 4, uint32_t(length));
    return SendAll(h, 8) && (length == 0 || SendAll(payload, length));
}

bool Server::SendAll(const void* data, size_t length)
{
    const char* p = (const char*)data;
    while (length > 0) {
        int n = (int)send((int)mClient, p, (int)(length > 65536 ? 65536 : length), MSG_NOSIGNAL);
        if (n <= 0) {
            mError = "send failed";
            CloseClient();
            return false;
        }
        p += n;
        length -= size_t(n);
    }
    return true;
}

} // namespace rfxview
