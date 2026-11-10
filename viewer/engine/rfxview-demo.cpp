/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* rfxview-demo: a stand-in for rfxengine, to test the phone app before Gecko
 * links. It draws a scrollable test pattern, marks taps, takes "URLs" and
 * typed text, and reports them back as page state.
 *   rfxview-demo [port]          (runs until killed) */

#include "rfxview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

using namespace rfxview;

struct Demo : Listener {
    Server server;
    std::vector<uint8_t> pixels;
    int w = 0, h = 0, scrollY = 0, tapX = -1, tapY = -1;
    uint32_t color = 0xFF2B5797;   // ARGB
    std::string url = "about:demo", typed;
    std::vector<std::string> history{ "about:demo" };
    size_t pos = 0;
    bool focused = false;

    void Resize(int nw, int nh) {
        w = nw; h = nh;
        pixels.assign(size_t(w) * h * 4, 0);
        Draw();
    }

    void Fill(int x0, int y0, int x1, int y1, uint32_t argb) {
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        for (int y = y0; y < y1; y++) {
            uint8_t* p = &pixels[(size_t(y) * w + x0) * 4];
            for (int x = x0; x < x1; x++, p += 4) {
                p[0] = uint8_t(argb); p[1] = uint8_t(argb >> 8);
                p[2] = uint8_t(argb >> 16); p[3] = 0xFF;
            }
        }
    }

    void Draw() {
        Fill(0, 0, w, h, color);
        // Stripes that move with scrolling, so panning is visible.
        for (int y = -(scrollY % 80); y < h; y += 80)
            Fill(0, y, w, y + 8, 0xFFFFFFFF);
        // A fake text field at the top: tap it to get the keyboard.
        Fill(16, 16, w - 16, 96, focused ? 0xFFFFFFE0 : 0xFFE0E0E0);
        // One block per typed character.
        for (size_t i = 0; i < typed.size() && i < 40; i++)
            Fill(24 + int(i) * 14, 40, 34 + int(i) * 14, 72, 0xFF000000);
        if (tapX >= 0)
            Fill(tapX - 20, tapY - 20, tapX + 20, tapY + 20, 0xFFE51400);
        server.InvalidateAll();
    }

    void SendState() {
        char buf[1024];
        snprintf(buf, sizeof(buf),
                 "{\"url\":\"%s\",\"title\":\"Demo: %s\",\"back\":%s,\"forward\":%s,"
                 "\"loading\":false,\"progress\":100}",
                 url.c_str(), typed.empty() ? "no text" : typed.c_str(),
                 pos > 0 ? "true" : "false", pos + 1 < history.size() ? "true" : "false");
        server.SendState(buf);
    }

    void Go(const std::string& u, bool push) {
        url = u;
        if (push) {
            history.resize(pos + 1);
            history.push_back(u);
            pos = history.size() - 1;
        }
        uint32_t hsh = 2166136261u;
        for (char c : u) hsh = (hsh ^ uint8_t(c)) * 16777619u;
        color = 0xFF000000 | (hsh & 0x007F7F7F);
        scrollY = 0;
        Draw();
        SendState();
    }

    void OnHello(int nw, int nh, int scale) override {
        printf("app connected: %dx%d, scale %d%%\n", nw, nh, scale);
        Resize(nw, nh);
        SendState();
    }
    void OnResize(int nw, int nh) override { printf("resize %dx%d\n", nw, nh); Resize(nw, nh); }
    void OnNavigate(const std::string& u) override { printf("navigate %s\n", u.c_str()); Go(u, true); }
    void OnCommand(uint8_t c) override {
        printf("command %u\n", c);
        if (c == kBack && pos > 0) Go(history[--pos], false);
        else if (c == kForward && pos + 1 < history.size()) Go(history[++pos], false);
        else if (c == kReload) Go(url, false);
    }
    void OnPointer(uint8_t action, int x, int y) override {
        printf("pointer %u at %d,%d\n", action, x, y);
        if (action != kPointerUp) return;
        tapX = x; tapY = y;
        bool f = y >= 16 && y < 96;
        if (f != focused) { focused = f; server.SendFocus(f); }
        Draw();
    }
    void OnScroll(int dx, int dy) override {
        scrollY += dy;
        if (scrollY < 0) scrollY = 0;
        Draw();
    }
    void OnKey(uint8_t action, uint16_t vk) override {
        printf("key %u vk %u\n", action, vk);
        if (action == 0 && vk == 8 && !typed.empty()) { typed.pop_back(); Draw(); SendState(); }
        if (action == 0 && vk == 13) { focused = false; server.SendFocus(false); Draw(); }
    }
    void OnText(const std::string& t) override {
        printf("text \"%s\"\n", t.c_str());
        typed += t;
        Draw();
        SendState();
    }
    void OnDisconnect() override { printf("app disconnected\n"); }
};

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    Demo d;
    uint16_t port = argc > 1 ? uint16_t(atoi(argv[1])) : kDefaultPort;
    if (!d.server.Listen(port)) {
        printf("rfxview-demo: %s\n", d.server.LastError().c_str());
        return 1;
    }
    printf("rfxview-demo listening on port %u\n", port);
    for (;;) {
        d.server.Poll(16, &d);
        if (d.w > 0) {
            Surface s = { d.pixels.data(), d.w, d.h, d.w * 4 };
            d.server.SendFrameIfReady(s);
        }
    }
}
