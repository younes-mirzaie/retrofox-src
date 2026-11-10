/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* rfxview: the engine side of the phone app <-> rfxengine connection
 * (viewer/PROTOCOL.md). No Gecko dependencies: rfxengine feeds it the frames
 * from widget/retrofox (RetrofoxFrame) and gets input back through Listener.
 * Single-threaded: call Poll() from the engine's event loop. */

#ifndef rfxview_h
#define rfxview_h

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

namespace rfxview {

enum : uint16_t {
    kWelcome = 1, kFrame = 2, kState = 3, kFocus = 4, kScrollInfo = 5, kTabs = 6, kGesture = 7,
    kHello = 100, kNavigate = 101, kCommand = 102, kPointer = 103,
    kScroll = 104, kKey = 105, kText = 106, kResize = 107, kFrameAck = 108, kLog = 109,
    kTab = 110
};
// kFormatShared: the engine runs inside the app's process; a FRAME carries no pixels, the app
// copies the rectangle from the frame buffer the engine shares (FrameSink)
enum : uint8_t { kFormatBGRA = 0, kFormatRGB565 = 1, kFormatShared = 2 };
enum : uint8_t { kReload = 0, kBack = 1, kForward = 2, kStop = 3, kRestartEngine = 4 };
enum : uint8_t { kPointerDown = 0, kPointerMove = 1, kPointerUp = 2, kPointerCancel = 3 };
enum : uint8_t { kTabNew = 0, kTabSelect = 1, kTabClose = 2 };

static const uint16_t kDefaultPort = 8081;
static const uint16_t kProtocolVersion = 1;

class Listener {
public:
    virtual ~Listener() {}
    // New app connection; size is the surface in physical pixels.
    virtual void OnHello(int width, int height, int scalePercent) {}
    virtual void OnNavigate(const std::string& url) {}
    virtual void OnCommand(uint8_t command) {}
    virtual void OnPointer(uint8_t action, int x, int y) {}
    virtual void OnScroll(int dx, int dy) {}
    virtual void OnKey(uint8_t action, uint16_t virtualKey) {}
    virtual void OnText(const std::string& utf8) {}
    // A line for the engine's log: the app has no log of its own on the phone
    virtual void OnLog(const std::string& utf8) {}
    // Tabs: open one (url empty = the home page), switch to one, close one
    virtual void OnTab(uint8_t action, uint32_t id, const std::string& url) {}
    virtual void OnResize(int width, int height) {}
    virtual void OnDisconnect() {}
};

// Where the page is scrolled, for the app's own scrolling (it moves the picture it has at once,
// then lets frames catch up). Physical pixels.
struct ScrollInfo {
    int32_t appliedX, appliedY;   // sum of the app's SCROLL deltas handled so far
    int32_t x, y;                 // the page's (root) scroll position
    int32_t maxX, maxY;           // the furthest it can scroll
    bool inner;                   // the last scroll moved something inside the page, not the page
};

// Where frames go when the app runs the engine in its own process (kFormatShared): Write copies
// the rectangle into memory the app reads, and returns false if it can't (the frame then goes
// over the connection as BGRA).
class FrameSink {
public:
    virtual ~FrameSink() {}
    virtual bool Write(const uint8_t* surface, int stride, int width, int height,
                       int x, int y, int w, int h) = 0;
};

// A view of the engine's current surface (B8G8R8A8 premultiplied, top-down).
struct Surface {
    const uint8_t* data;
    int width, height, stride;
};

class Server {
public:
    Server();
    ~Server();

    // Listens on 0.0.0.0:port. Returns false (and logs) on failure.
    bool Listen(uint16_t port = kDefaultPort);
    // Waits up to timeoutMs for activity, accepts a new app (replacing the old
    // one), reads input and calls the listener. timeoutMs 0 = don't wait.
    void Poll(int timeoutMs, Listener* listener);

    bool Connected() const;
    int Width() const { return mWidth; }      // surface size the app asked for
    int Height() const { return mHeight; }

    // Marks part of the surface as changed. Frames go out from SendFrameIfReady.
    void Invalidate(int x, int y, int w, int h);
    void InvalidateAll() { Invalidate(0, 0, mWidth, mHeight); }
    // Sends the changed rectangle if the app isn't 2 frames behind. Returns
    // true if a frame went out.
    bool SendFrameIfReady(const Surface& surface);

    void SendState(const std::string& json);
    void SendFocus(bool focused);
    // In-process: frames for an app that asked for kFormatShared go through this
    void SetFrameSink(FrameSink* sink) { mSink = sink; }
    void SendTabs(const std::string& json);
    // Who handles the touch drag in progress: 0 no touch, 1 not decided, 2 the browser scrolls,
    // 3 the page handles it (the app stops moving the picture)
    void SendGesture(uint8_t gesture);
    // Sends the scroll state if it changed, tagged with the frame that shows it (the frame
    // sent last, or the next one while changes are still waiting to go out).
    void SendScrollInfo(const ScrollInfo& info);

    // For the engine's speed report (it resets them): scroll messages in, acks in and how long
    // the app took to acknowledge (ms, summed), and how often a frame was ready but waited for
    // an ack
    struct Stats {
        uint32_t scrolls = 0, acks = 0, ackWaits = 0;
        double ackMs = 0;
    } stats;

    // For tests: the last error text.
    const std::string& LastError() const { return mError; }

private:
    void CloseClient();
    bool SendMsg(uint16_t type, const void* payload, size_t length);
    bool SendAll(const void* data, size_t length);
    void Dispatch(uint16_t type, const uint8_t* p, size_t n, Listener* l);

    intptr_t mListen;
    intptr_t mClient;
    std::vector<uint8_t> mIn;
    std::vector<uint8_t> mOut;
    bool mHelloDone;
    int mWidth, mHeight;
    uint8_t mFormat;
    uint32_t mSerial, mAcked;
    bool mDirty;
    int mDirtyX0, mDirtyY0, mDirtyX1, mDirtyY1;
    FrameSink* mSink = nullptr;
    double mSentAt[8] = {};     // when each recent frame went out (by serial % 8)
    ScrollInfo mScrollSent;
    uint32_t mScrollSentSerial;
    std::string mError;
};

} // namespace rfxview

#endif
