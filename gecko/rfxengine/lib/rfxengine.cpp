/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* rfxengine: Gecko (UXP) as a native program on Windows Phone 8.1.
 *
 * Embeds one nsWebBrowser in an offscreen widget/retrofox window and serves it
 * to the Retrofox phone app over viewer/PROTOCOL.md (rfxview). Also has a
 * headless screenshot mode for testing without the app:
 *
 *   rfxengine.exe                      wait for the app on port 8081
 *   rfxengine.exe --port 8082
 *   rfxengine.exe --shot URL out.bmp   load URL, save a screenshot, exit
 *
 * Needs the files of dist/bin next to it (xul.dll, omni.ja, greprefs, ...).
 * The profile lives in .\profile. */

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "nsXULAppAPI.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsCOMPtr.h"
#include "nsEmbedCID.h"
#include "nsComponentManagerUtils.h"
#include "nsIAppShell.h"
#include "nsIBaseWindow.h"
#include "nsIDirectoryService.h"
#include "nsIDocShell.h"
#include "nsIEmbeddingSiteWindow.h"
#include "nsIFile.h"
#include "nsIInterfaceRequestor.h"
#include "nsIInterfaceRequestorUtils.h"
#include "nsIPresShell.h"
#include "nsIScrollableFrame.h"
#include "nsIURI.h"
#include "nsIWebBrowser.h"
#include "nsIWebBrowserChrome.h"
#include "nsIWebBrowserFocus.h"
#include "nsIWebNavigation.h"
#include "nsIWebProgress.h"
#include "nsIWebProgressListener.h"
#include "nsIWidget.h"
#include "nsServiceManagerUtils.h"
#include "nsPresContext.h"
#include "nsIFrame.h"
#include "nsIContent.h"
#include "StickyScrollContainer.h"
#include "nsStyleStruct.h"
#include "gfxContext.h"
#include "mozilla/gfx/2D.h"
#include "nsPrintfCString.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsThreadUtils.h"
#include "nsWeakReference.h"
#include "nsWidgetsCID.h"
#include "mozilla/Preferences.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Types.h"
#include "prinrval.h"

#include "RetrofoxFrame.h"
#include "rfxview.h"

using namespace mozilla;
using namespace mozilla::widget;

static NS_DEFINE_CID(kAppShellCID, NS_APPSHELL_CID);

static void
Log(const char* aFormat, ...)
{
  va_list ap;
  va_start(ap, aFormat);
  SYSTEMTIME t;
  GetLocalTime(&t);
  printf("%02u:%02u:%02u rfxengine: ", t.wHour, t.wMinute, t.wSecond);
  vprintf(aFormat, ap);
  fputc('\n', stdout);
  fflush(stdout);
  va_end(ap);
}

// Tells the watchdog (main.cpp) the main loop is still going: it passes an event handle in
// RFXENGINE_HEARTBEAT and restarts an engine that stops signalling it (a hung page, a deadlock)
static void
Heartbeat()
{
  static HANDLE sEvent = [] {
    const char* v = getenv("RFXENGINE_HEARTBEAT");
    return v ? reinterpret_cast<HANDLE>(uintptr_t(strtoul(v, nullptr, 10))) : nullptr;
  }();
  if (sEvent) {
    SetEvent(sEvent);
  }
}

static std::string
JsonString(const nsACString& aUtf8)
{
  std::string out = "\"";
  for (uint32_t i = 0; i < aUtf8.Length(); ++i) {
    unsigned char c = aUtf8.CharAt(i);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += char(c);
    }
  }
  return out + "\"";
}

// ---- Profile directory ----

class RfxDirProvider final : public nsIDirectoryServiceProvider
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIDIRECTORYSERVICEPROVIDER

  explicit RfxDirProvider(nsIFile* aProfile) : mProfile(aProfile) {}

private:
  ~RfxDirProvider() {}
  nsCOMPtr<nsIFile> mProfile;
};

NS_IMPL_ISUPPORTS(RfxDirProvider, nsIDirectoryServiceProvider)

NS_IMETHODIMP
RfxDirProvider::GetFile(const char* aKey, bool* aPersistent, nsIFile** aResult)
{
  if (!strcmp(aKey, NS_APP_USER_PROFILE_50_DIR) ||
      !strcmp(aKey, NS_APP_USER_PROFILE_LOCAL_50_DIR) ||
      !strcmp(aKey, NS_APP_PROFILE_DIR_STARTUP) ||
      !strcmp(aKey, NS_APP_PROFILE_LOCAL_DIR_STARTUP)) {
    *aPersistent = true;
    return mProfile->Clone(aResult);
  }
  return NS_ERROR_FAILURE;
}

// ---- The browser: one nsWebBrowser in a top-level widget/retrofox window ----

class RfxBrowser final : public nsIWebBrowserChrome
                       , public nsIEmbeddingSiteWindow
                       , public nsIWebProgressListener
                       , public nsIInterfaceRequestor
                       , public nsSupportsWeakReference
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIWEBBROWSERCHROME
  NS_DECL_NSIEMBEDDINGSITEWINDOW
  NS_DECL_NSIWEBPROGRESSLISTENER
  NS_DECL_NSIINTERFACEREQUESTOR

  RfxBrowser()
    : mWidth(0), mHeight(0), mChromeFlags(nsIWebBrowserChrome::CHROME_DEFAULT)
    , mLoading(false), mLoadedOnce(false), mProgress(0), mStateChanged(true) {}

  nsresult Init(int32_t aWidth, int32_t aHeight);
  void Resize(int32_t aWidth, int32_t aHeight);
  void Load(const nsACString& aUrl);
  void Command(uint8_t aCommand);
  bool TakeStateChange() { bool c = mStateChanged; mStateChanged = false; return c; }
  std::string StateJson();
  bool Loading() const { return mLoading; }
  bool LoadedOnce() const { return mLoadedOnce; }
  // Tabs: the one on screen is active; the others are hidden and don't paint
  void SetActive(bool aActive);
  void Destroy();
  const nsCString& Url() const { return mUrl; }
  std::string Title() const { return std::string(NS_ConvertUTF16toUTF8(mTitle).get()); }
  // A tab loaded again goes back to where it was scrolled once the page has loaded
  void RestoreScrollAfterLoad(int32_t aX, int32_t aY) { mRestoreX = aX; mRestoreY = aY; mRestore = aX || aY; }
  void ScrollTo(int32_t aX, int32_t aY);
  uint32_t mId = 0;
  int32_t mRestoreX = 0, mRestoreY = 0;
  bool mRestore = false;
  // The page's scroll position and how far it can scroll, in physical pixels
  bool GetRootScroll(int32_t* aX, int32_t* aY, int32_t* aMaxX, int32_t* aMaxY);
  // Draws a rectangle of the page (physical pixels from the page's top left, whatever is on
  // screen) into aOut as BGRA rows; for the strips the app shows while panning
  bool RenderStrip(int32_t aX, int32_t aY, int32_t aW, int32_t aH, std::vector<uint8_t>& aOut);
  // The on-screen rectangles (x, y, w, h, physical pixels) of the page's position: fixed boxes
  void GetFixedRects(std::vector<int32_t>& aOut);

private:
  ~RfxBrowser() {}

  nsCOMPtr<nsIWidget> mWindow;
  nsCOMPtr<nsIWebBrowser> mBrowser;
  nsCOMPtr<nsIWebNavigation> mNav;
  int32_t mWidth, mHeight;
  uint32_t mChromeFlags;
  nsString mTitle;
  nsCString mUrl;
  bool mLoading;
  bool mLoadedOnce;
  int32_t mProgress;
  bool mStateChanged;
};

NS_IMPL_ISUPPORTS(RfxBrowser,
                  nsIWebBrowserChrome,
                  nsIEmbeddingSiteWindow,
                  nsIWebProgressListener,
                  nsIInterfaceRequestor,
                  nsISupportsWeakReference)

nsresult
RfxBrowser::Init(int32_t aWidth, int32_t aHeight)
{
  nsresult rv;
  mWidth = aWidth;
  mHeight = aHeight;

  // The top-level offscreen window (widget/retrofox). The page's content view
  // creates its own child window inside it.
  mWindow = do_CreateInstance("@mozilla.org/widgets/window/win;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  nsWidgetInitData init;
  init.mWindowType = eWindowType_toplevel;
  rv = mWindow->Create(nullptr, nullptr, LayoutDeviceIntRect(0, 0, aWidth, aHeight), &init);
  NS_ENSURE_SUCCESS(rv, rv);

  mBrowser = do_CreateInstance(NS_WEBBROWSER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = mBrowser->SetContainerWindow(this);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIBaseWindow> base = do_QueryInterface(mBrowser);
  rv = base->InitWindow(nullptr, mWindow, 0, 0, aWidth, aHeight);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = base->Create();
  NS_ENSURE_SUCCESS(rv, rv);

  nsWeakPtr weak = do_GetWeakReference(static_cast<nsIWebProgressListener*>(this));
  rv = mBrowser->AddWebBrowserListener(weak, NS_GET_IID(nsIWebProgressListener));
  NS_ENSURE_SUCCESS(rv, rv);

  base->SetVisibility(true);
  mWindow->Show(true);
  // An embedding has to say its window is the active one; until then Gecko keeps focused text
  // fields inactive (no IME state, so the app never got FOCUS and the keyboard never came up)
  nsCOMPtr<nsIWebBrowserFocus> focus = do_QueryInterface(mBrowser);
  if (focus) {
    focus->Activate();
  }
  mNav = do_QueryInterface(mBrowser);
  return mNav ? NS_OK : NS_ERROR_FAILURE;
}

void
RfxBrowser::Resize(int32_t aWidth, int32_t aHeight)
{
  if (aWidth == mWidth && aHeight == mHeight) {
    return;
  }
  mWidth = aWidth;
  mHeight = aHeight;
  mWindow->Resize(aWidth, aHeight, true);
  nsCOMPtr<nsIBaseWindow> base = do_QueryInterface(mBrowser);
  base->SetPositionAndSize(0, 0, aWidth, aHeight, 0);
}

void
RfxBrowser::SetActive(bool aActive)
{
  nsCOMPtr<nsIBaseWindow> base = do_QueryInterface(mBrowser);
  if (base) {
    base->SetVisibility(aActive);
  }
  // A hidden tab's timers and animations slow down, as in a background tab on the desktop
  nsCOMPtr<nsIDocShell> docShell = do_GetInterface(mBrowser);
  if (docShell) {
    docShell->SetIsActive(aActive);
  }
  // Showing the window makes it the one that paints and gets input (widget/retrofox)
  mWindow->Show(aActive);
  nsCOMPtr<nsIWebBrowserFocus> focus = do_QueryInterface(mBrowser);
  if (focus) {
    if (aActive) {
      focus->Activate();
    } else {
      focus->Deactivate();
    }
  }
}

void
RfxBrowser::Destroy()
{
  nsCOMPtr<nsIBaseWindow> base = do_QueryInterface(mBrowser);
  if (base) {
    base->Destroy();
  }
  if (mWindow) {
    mWindow->Destroy();
  }
  mNav = nullptr;
  mBrowser = nullptr;
  mWindow = nullptr;
}

void
RfxBrowser::ScrollTo(int32_t aX, int32_t aY)
{
  nsCOMPtr<nsIDocShell> docShell = do_GetInterface(mBrowser);
  nsIPresShell* presShell = docShell ? docShell->GetPresShell() : nullptr;
  nsPresContext* presContext = presShell ? presShell->GetPresContext() : nullptr;
  nsIScrollableFrame* scroll = presShell ? presShell->GetRootScrollFrameAsScrollable() : nullptr;
  if (presContext && scroll) {
    scroll->ScrollTo(nsPoint(presContext->DevPixelsToAppUnits(aX), presContext->DevPixelsToAppUnits(aY)),
                     nsIScrollableFrame::INSTANT);
  }
}

bool
RfxBrowser::GetRootScroll(int32_t* aX, int32_t* aY, int32_t* aMaxX, int32_t* aMaxY)
{
  nsCOMPtr<nsIDocShell> docShell = do_GetInterface(mBrowser);
  nsIPresShell* presShell = docShell ? docShell->GetPresShell() : nullptr;
  nsPresContext* presContext = presShell ? presShell->GetPresContext() : nullptr;
  nsIScrollableFrame* scroll = presShell ? presShell->GetRootScrollFrameAsScrollable() : nullptr;
  if (!presContext || !scroll) {
    return false;
  }
  nsPoint pos = scroll->GetScrollPosition();
  nsRect range = scroll->GetScrollRange();
  *aX = presContext->AppUnitsToDevPixels(pos.x);
  *aY = presContext->AppUnitsToDevPixels(pos.y);
  *aMaxX = presContext->AppUnitsToDevPixels(range.XMost());
  *aMaxY = presContext->AppUnitsToDevPixels(range.YMost());
  return true;
}

void
RfxBrowser::GetFixedRects(std::vector<int32_t>& aOut)
{
  aOut.clear();
  nsCOMPtr<nsIDocShell> docShell = do_GetInterface(mBrowser);
  nsIPresShell* presShell = docShell ? docShell->GetPresShell() : nullptr;
  nsPresContext* presContext = presShell ? presShell->GetPresContext() : nullptr;
  nsIFrame* root = presShell ? presShell->GetRootFrame() : nullptr;   // the viewport frame
  if (!presContext || !root) {
    return;
  }
  nsIntRect view = root->GetRect().ToOutsidePixels(presContext->AppUnitsPerDevPixel());
  int64_t viewArea = int64_t(view.width) * view.height;
  auto add = [&](const nsRect& aRect) {
    nsIntRect d = aRect.ToOutsidePixels(presContext->AppUnitsPerDevPixel());
    // A box over half the screen isn't a header or a footer: a full-screen background or
    // overlay. Holding it in place while panning would freeze the whole page, so leave it out.
    if (d.width > 0 && d.height > 0 && int64_t(d.width) * d.height * 2 <= viewArea) {
      aOut.push_back(d.x);
      aOut.push_back(d.y);
      aOut.push_back(d.width);
      aOut.push_back(d.height);
    }
  };
  for (nsIFrame* f : root->GetChildList(nsIFrame::kFixedList)) {
    // Gecko's own hidden full-screen layer (for selection handles and the like) is fixed too
    nsIContent* content = f->GetContent();
    if (content && content->IsInNativeAnonymousSubtree()) {
      continue;
    }
    if (f->StyleVisibility()->IsVisible()) {
      add(f->GetVisualOverflowRect() + f->GetPosition());
    }
  }
  // position: sticky boxes act like fixed ones while they're stuck (moved from where the page
  // would put them): those too, where they are on screen
  nsIFrame* scrollFrame = presShell->GetRootScrollFrame();
  mozilla::StickyScrollContainer* sticky = scrollFrame ?
    mozilla::StickyScrollContainer::GetStickyScrollContainerForScrollFrame(scrollFrame) : nullptr;
  if (sticky) {
    for (nsIFrame* f : sticky->Frames()) {
      if (f->GetPosition() != f->GetNormalPosition() && f->StyleVisibility()->IsVisible()) {
        add(f->GetVisualOverflowRect() + f->GetOffsetTo(root));
      }
    }
  }
}

bool
RfxBrowser::RenderStrip(int32_t aX, int32_t aY, int32_t aW, int32_t aH, std::vector<uint8_t>& aOut)
{
  using namespace mozilla::gfx;
  nsCOMPtr<nsIDocShell> docShell = do_GetInterface(mBrowser);
  nsIPresShell* presShell = docShell ? docShell->GetPresShell() : nullptr;
  nsPresContext* presContext = presShell ? presShell->GetPresContext() : nullptr;
  if (!presContext || aW <= 0 || aH <= 0) {
    return false;
  }
  aOut.assign(size_t(aW) * aH * 4, 0xFF);
  RefPtr<DrawTarget> dt = Factory::CreateDrawTargetForData(
    BackendType::SKIA, aOut.data(), IntSize(aW, aH), aW * 4, SurfaceFormat::B8G8R8A8);
  if (!dt) {
    return false;
  }
  RefPtr<gfxContext> ctx = gfxContext::CreateOrNull(dt);
  if (!ctx) {
    return false;
  }
  // RenderDocument draws one CSS pixel per unit: scale up to physical pixels
  double scale = double(nsPresContext::AppUnitsPerCSSPixel()) / presContext->AppUnitsPerDevPixel();
  ctx->SetMatrix(gfxMatrix::Scaling(scale, scale));
  nsRect rect(presContext->DevPixelsToAppUnits(aX), presContext->DevPixelsToAppUnits(aY),
              presContext->DevPixelsToAppUnits(aW), presContext->DevPixelsToAppUnits(aH));
  // The page as if not scrolled, so the rectangle is in page coordinates; images that aren't
  // decoded yet are left for later rather than decoded now
  uint32_t flags = nsIPresShell::RENDER_IGNORE_VIEWPORT_SCROLLING |
                   nsIPresShell::RENDER_DOCUMENT_RELATIVE |
                   nsIPresShell::RENDER_ASYNC_DECODE_IMAGES |
                   nsIPresShell::RENDER_DRAWWINDOW_NOT_FLUSHING;
  nsresult rv = presShell->RenderDocument(rect, flags, NS_RGB(255, 255, 255), ctx);
  return NS_SUCCEEDED(rv);
}

void
RfxBrowser::Load(const nsACString& aUrl)
{
  Log("load %s", PromiseFlatCString(aUrl).get());
  nsresult rv = mNav->LoadURI(NS_ConvertUTF8toUTF16(aUrl).get(),
                              nsIWebNavigation::LOAD_FLAGS_NONE,
                              nullptr, nullptr, nullptr);
  if (NS_FAILED(rv)) {
    Log("LoadURI failed: 0x%08x", unsigned(rv));
  }
}

void
RfxBrowser::Command(uint8_t aCommand)
{
  switch (aCommand) {
    case rfxview::kReload: mNav->Reload(nsIWebNavigation::LOAD_FLAGS_NONE); break;
    case rfxview::kBack: mNav->GoBack(); break;
    case rfxview::kForward: mNav->GoForward(); break;
    case rfxview::kStop: mNav->Stop(nsIWebNavigation::STOP_ALL); break;
  }
}

std::string
RfxBrowser::StateJson()
{
  bool back = false, forward = false;
  mNav->GetCanGoBack(&back);
  mNav->GetCanGoForward(&forward);
  char tail[128];
  snprintf(tail, sizeof(tail), ",\"back\":%s,\"forward\":%s,\"loading\":%s,\"progress\":%d}",
           back ? "true" : "false", forward ? "true" : "false",
           mLoading ? "true" : "false", int(mProgress));
  return "{\"url\":" + JsonString(mUrl) +
         ",\"title\":" + JsonString(NS_ConvertUTF16toUTF8(mTitle)) + tail;
}

// nsIWebProgressListener

NS_IMETHODIMP
RfxBrowser::OnStateChange(nsIWebProgress* aWebProgress, nsIRequest* aRequest,
                          uint32_t aStateFlags, nsresult aStatus)
{
  if (!(aStateFlags & STATE_IS_NETWORK)) {
    return NS_OK;
  }
  if (aStateFlags & STATE_START) {
    mLoading = true;
    mProgress = 0;
    mStateChanged = true;
  } else if (aStateFlags & STATE_STOP) {
    mLoading = false;
    mLoadedOnce = true;
    mProgress = 100;
    mStateChanged = true;
    Log("loaded (status 0x%08x)", unsigned(aStatus));
    if (mRestore) {
      mRestore = false;
      ScrollTo(mRestoreX, mRestoreY);
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
RfxBrowser::OnProgressChange(nsIWebProgress* aWebProgress, nsIRequest* aRequest,
                             int32_t aCurSelfProgress, int32_t aMaxSelfProgress,
                             int32_t aCurTotalProgress, int32_t aMaxTotalProgress)
{
  if (aMaxTotalProgress > 0) {
    int32_t p = int32_t(int64_t(aCurTotalProgress) * 100 / aMaxTotalProgress);
    if (p != mProgress) {
      mProgress = p;
      mStateChanged = true;
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
RfxBrowser::OnLocationChange(nsIWebProgress* aWebProgress, nsIRequest* aRequest,
                             nsIURI* aLocation, uint32_t aFlags)
{
  bool topLevel = true;
  if (aWebProgress) {
    aWebProgress->GetIsTopLevel(&topLevel);
  }
  if (topLevel && aLocation) {
    aLocation->GetSpec(mUrl);
    mStateChanged = true;
  }
  return NS_OK;
}

NS_IMETHODIMP
RfxBrowser::OnStatusChange(nsIWebProgress*, nsIRequest*, nsresult, const char16_t*)
{
  return NS_OK;
}

NS_IMETHODIMP
RfxBrowser::OnSecurityChange(nsIWebProgress*, nsIRequest*, uint32_t)
{
  return NS_OK;
}

// nsIWebBrowserChrome

NS_IMETHODIMP RfxBrowser::SetStatus(uint32_t, const char16_t*) { return NS_OK; }

NS_IMETHODIMP
RfxBrowser::GetWebBrowser(nsIWebBrowser** aWebBrowser)
{
  NS_IF_ADDREF(*aWebBrowser = mBrowser);
  return NS_OK;
}

NS_IMETHODIMP
RfxBrowser::SetWebBrowser(nsIWebBrowser* aWebBrowser)
{
  mBrowser = aWebBrowser;
  return NS_OK;
}

NS_IMETHODIMP
RfxBrowser::GetChromeFlags(uint32_t* aFlags)
{
  *aFlags = mChromeFlags;
  return NS_OK;
}

NS_IMETHODIMP
RfxBrowser::SetChromeFlags(uint32_t aFlags)
{
  mChromeFlags = aFlags;
  return NS_OK;
}

NS_IMETHODIMP RfxBrowser::DestroyBrowserWindow() { return NS_OK; }
NS_IMETHODIMP RfxBrowser::SizeBrowserTo(int32_t, int32_t) { return NS_OK; }
NS_IMETHODIMP RfxBrowser::ShowAsModal() { return NS_ERROR_NOT_IMPLEMENTED; }

NS_IMETHODIMP
RfxBrowser::IsWindowModal(bool* aResult)
{
  *aResult = false;
  return NS_OK;
}

NS_IMETHODIMP RfxBrowser::ExitModalEventLoop(nsresult) { return NS_OK; }

// nsIEmbeddingSiteWindow

NS_IMETHODIMP RfxBrowser::SetDimensions(uint32_t, int32_t, int32_t, int32_t, int32_t) { return NS_OK; }

NS_IMETHODIMP
RfxBrowser::GetDimensions(uint32_t aFlags, int32_t* aX, int32_t* aY, int32_t* aCX, int32_t* aCY)
{
  if (aX) *aX = 0;
  if (aY) *aY = 0;
  if (aCX) *aCX = mWidth;
  if (aCY) *aCY = mHeight;
  return NS_OK;
}

NS_IMETHODIMP RfxBrowser::SetFocus() { return NS_OK; }

NS_IMETHODIMP
RfxBrowser::GetVisibility(bool* aVisibility)
{
  *aVisibility = true;
  return NS_OK;
}

NS_IMETHODIMP RfxBrowser::SetVisibility(bool) { return NS_OK; }

NS_IMETHODIMP
RfxBrowser::GetTitle(char16_t** aTitle)
{
  *aTitle = ToNewUnicode(mTitle);
  return NS_OK;
}

NS_IMETHODIMP
RfxBrowser::SetTitle(const char16_t* aTitle)
{
  mTitle = aTitle ? aTitle : u"";
  mStateChanged = true;
  return NS_OK;
}

NS_IMETHODIMP
RfxBrowser::GetSiteWindow(void** aSiteWindow)
{
  *aSiteWindow = nullptr;   // no native window on the phone
  return NS_OK;
}

NS_IMETHODIMP RfxBrowser::Blur() { return NS_OK; }

// nsIInterfaceRequestor

NS_IMETHODIMP
RfxBrowser::GetInterface(const nsIID& aIID, void** aResult)
{
  return QueryInterface(aIID, aResult);
}

// ---- The connection to the phone app ----

// Running inside the Retrofox app (--in-app) rather than as rfxengine.exe
static bool sInApp = false;

class AppLink final : public rfxview::Listener
{
public:
  RefPtr<RfxBrowser> mBrowser;              // the tab on screen

  // Tabs. There's no limit on how many (as IE on Windows Phone 8.1), but only a few keep a live
  // page (the one on screen and the most recently used); the others are unloaded to their
  // address, title and scroll position, and load again when switched to.
  struct Tab {
    uint32_t id = 0;
    RefPtr<RfxBrowser> browser;     // null while unloaded
    nsCString url;
    std::string title;
    int32_t scrollX = 0, scrollY = 0;
    uint32_t lastUsed = 0;
  };
  std::vector<Tab> mTabs;
  uint32_t mNextTabId = 1;
  uint32_t mUseClock = 0;
  bool mTabsChanged = false;
  std::string mHome = "about:blank";
  rfxview::Server* mServer = nullptr;
  bool mHello = false;
  int mWidth = 480, mHeight = 696, mScale = 150;

  // How many tabs keep a live page: 3, fewer when the phone runs short of memory
  static uint32_t LiveTabLimit()
  {
    MEMORYSTATUSEX m = { sizeof(m) };
    if (!GlobalMemoryStatusEx(&m)) {
      return 3;
    }
    uint64_t freeMb = m.ullAvailPhys / (1024 * 1024);
    return freeMb < 120 ? 1 : freeMb < 200 ? 2 : 3;
  }

  Tab* FindTab(uint32_t aId)
  {
    for (Tab& t : mTabs) {
      if (t.id == aId) {
        return &t;
      }
    }
    return nullptr;
  }

  Tab* ActiveTab()
  {
    for (Tab& t : mTabs) {
      if (t.browser && t.browser == mBrowser) {
        return &t;
      }
    }
    return nullptr;
  }

  void AddTab(RfxBrowser* aBrowser)
  {
    Tab t;
    t.id = mNextTabId++;
    t.browser = aBrowser;
    t.lastUsed = ++mUseClock;
    aBrowser->mId = t.id;
    mTabs.push_back(t);
    mTabsChanged = true;
  }

  // Keeps the tab's address, title and scroll position, and frees its page
  void Unload(Tab& aTab)
  {
    RfxBrowser* b = aTab.browser;
    if (!b) {
      return;
    }
    aTab.url = b->Url();
    aTab.title = b->Title();
    int32_t maxX, maxY;
    if (!b->GetRootScroll(&aTab.scrollX, &aTab.scrollY, &maxX, &maxY)) {
      aTab.scrollX = aTab.scrollY = 0;
    }
    b->Destroy();
    aTab.browser = nullptr;
    Log("tab %u unloaded (%s)", unsigned(aTab.id), aTab.url.get());
  }

  void UnloadBeyondLimit()
  {
    uint32_t limit = LiveTabLimit();
    for (;;) {
      uint32_t live = 0;
      Tab* oldest = nullptr;
      for (Tab& t : mTabs) {
        if (!t.browser) {
          continue;
        }
        ++live;
        if (t.browser != mBrowser && (!oldest || t.lastUsed < oldest->lastUsed)) {
          oldest = &t;
        }
      }
      if (live <= limit || !oldest) {
        return;
      }
      Unload(*oldest);
    }
  }

  // Puts a tab on screen, loading it again if it was unloaded. The whole surface is sent again.
  void Activate(Tab& aTab)
  {
    aTab.lastUsed = ++mUseClock;
    if (!aTab.browser) {
      RefPtr<RfxBrowser> b = new RfxBrowser();
      if (NS_FAILED(b->Init(mWidth, mHeight))) {
        Log("tab %u: can't create a page", unsigned(aTab.id));
        return;
      }
      b->SetActive(false);      // Init shows it; it's shown below like any tab
      b->mId = aTab.id;
      aTab.browser = b;
      b->RestoreScrollAfterLoad(aTab.scrollX, aTab.scrollY);
      if (aTab.url.IsEmpty()) {
        b->Load(nsDependentCString(mHome.c_str()));
      } else {
        b->Load(aTab.url);
      }
      Log("tab %u loaded again", unsigned(aTab.id));
    }
    if (mBrowser != aTab.browser) {
      if (mBrowser) {
        mBrowser->SetActive(false);
      }
      mBrowser = aTab.browser;
      mBrowser->SetActive(true);
      if (mServer) {
        mServer->InvalidateAll();
      }
    }
    mStateResend = true;
    mTabsChanged = true;
    UnloadBeyondLimit();
  }

  void OnTab(uint8_t aAction, uint32_t aId, const std::string& aUrl) override
  {
    if (aAction == rfxview::kTabNew) {
      Tab t;
      t.id = mNextTabId++;
      t.url.Assign(aUrl.empty() ? mHome.c_str() : aUrl.c_str());
      mTabs.push_back(t);
      Activate(mTabs.back());
      Log("tab %u opened (%u tabs)", unsigned(t.id), unsigned(mTabs.size()));
    } else if (aAction == rfxview::kTabSelect) {
      if (Tab* t = FindTab(aId)) {
        Activate(*t);
      }
    } else if (aAction == rfxview::kTabClose) {
      Tab* t = FindTab(aId);
      if (!t) {
        return;
      }
      if (mTabs.size() == 1) {
        // The last tab stays, with the home page in it
        if (t->browser) {
          t->browser->Load(nsDependentCString(mHome.c_str()));
        }
        return;
      }
      size_t i = t - &mTabs[0];
      if (t->browser && t->browser == mBrowser) {
        // Next to it becomes the one on screen (the most recently used would also do)
        Activate(mTabs[i + 1 < mTabs.size() ? i + 1 : i - 1]);
      }
      if (mTabs[i].browser) {
        mTabs[i].browser->Destroy();
      }
      mTabs.erase(mTabs.begin() + i);
      mTabsChanged = true;
      Log("tab %u closed (%u tabs)", unsigned(aId), unsigned(mTabs.size()));
    }
  }

  std::string TabsJson()
  {
    char head[48];
    snprintf(head, sizeof(head), "{\"active\":%u,\"tabs\":[",
             unsigned(mBrowser ? mBrowser->mId : 0));
    std::string json = head;
    for (size_t i = 0; i < mTabs.size(); ++i) {
      const Tab& t = mTabs[i];
      char id[32];
      snprintf(id, sizeof(id), "{\"id\":%u,", unsigned(t.id));
      std::string url = t.browser ? std::string(t.browser->Url().get()) : std::string(t.url.get());
      std::string title = t.browser ? t.browser->Title() : t.title;
      json += (i ? "," : "") + std::string(id) + "\"title\":" +
              JsonString(nsDependentCString(title.c_str())) + ",\"url\":" +
              JsonString(nsDependentCString(url.c_str())) + "}";
    }
    return json + "]}";
  }

  void OnHello(int aWidth, int aHeight, int aScale) override
  {
    Log("app connected: %dx%d at %d%%", aWidth, aHeight, aScale);
    mHello = true;
    mWidth = aWidth;
    mHeight = aHeight;
    mScale = aScale;
    mScroll = rfxview::ScrollInfo();   // the app counts its scrolling from zero again
    for (Tab& tab : mTabs) {
      if (tab.browser) {
        tab.browser->Resize(aWidth, aHeight);
      }
    }
    mStateResend = true;
    mTabsChanged = true;
  }
  void OnResize(int aWidth, int aHeight) override
  {
    for (Tab& tab : mTabs) {
      if (tab.browser) {
        tab.browser->Resize(aWidth, aHeight);
      }
    }
  }
  void OnNavigate(const std::string& aUrl) override
  {
    if (mBrowser) {
      mBrowser->Load(nsDependentCString(aUrl.c_str(), aUrl.size()));
    }
  }
  void OnCommand(uint8_t aCommand) override
  {
    if (aCommand == rfxview::kRestartEngine && sInApp) {
      // Inside the app there's no watchdog, and exiting would close the app: a fresh launch
      // starts a fresh engine anyway
      Log("restart requested by the app: already fresh (in the app)");
      return;
    }
    if (aCommand == rfxview::kRestartEngine) {
      // The app wants a clean engine (fresh launch, or its Restart button): exit, and the
      // watchdog (main.cpp) starts a new one
      Log("restart requested by the app");
      fflush(stdout);
      TerminateProcess(GetCurrentProcess(), 0);
    }
    if (mBrowser) {
      mBrowser->Command(aCommand);
    }
  }
  void OnPointer(uint8_t aAction, int aX, int aY) override
  {
    mLastInput = PR_IntervalNow();
    RetrofoxPointerAction a = aAction == rfxview::kPointerDown ? eRetrofoxPointerDown :
                              aAction == rfxview::kPointerUp ? eRetrofoxPointerUp :
                              eRetrofoxPointerMove;
    if (aAction == rfxview::kPointerCancel) {
      // The app turned this touch into a pan: scroll what is under the finger
      SetRetrofoxPointerPosition(aX, aY);
    } else {
      DispatchRetrofoxPointer(a, aX, aY);
    }
  }
  // Scrolling waits while a touch is undecided (the page may still take the drag) and is dropped
  // while the page handles it
  int32_t mHeldDX = 0, mHeldDY = 0;

  void OnScroll(int aDX, int aDY) override
  {
    mLastInput = PR_IntervalNow();
    RetrofoxGesture g = GetRetrofoxGesture();
    if (g == eRetrofoxGesturePage) {
      DropHeldScroll();
      mScroll.appliedX += aDX;     // handled, as far as the app's prediction goes: nothing moved
      mScroll.appliedY += aDY;
      return;
    }
    if (g == eRetrofoxGestureUndecided) {
      mHeldDX += aDX;
      mHeldDY += aDY;
      return;
    }
    // Held scrolling applies now that the browser has the drag (counted below with the rest)
    aDX += mHeldDX;
    aDY += mHeldDY;
    mHeldDX = mHeldDY = 0;
    // Every step the app sent since the last turn goes to Gecko as one scroll, before its
    // events run (ApplyPendingScroll): one wheel event, one scroll event for the page's scripts
    mPendingDX += aDX;
    mPendingDY += aDY;
  }
  int32_t mPendingDX = 0, mPendingDY = 0;
  void ApplyPendingScroll()
  {
    int32_t aDX = mPendingDX, aDY = mPendingDY;
    mPendingDX = mPendingDY = 0;
    if (!aDX && !aDY) {
      return;
    }
    int32_t x0, y0, x1, y1, maxX, maxY;
    bool before = mBrowser && mBrowser->GetRootScroll(&x0, &y0, &maxX, &maxY);
    DispatchRetrofoxScroll(aDX, aDY);
    mScroll.appliedX += aDX;
    mScroll.appliedY += aDY;
    // Did the page itself move? If it could have but didn't, the scroll went to a box inside it,
    // and the app shouldn't move the whole picture for the rest of this pan
    if (before && mBrowser->GetRootScroll(&x1, &y1, &maxX, &maxY)) {
      bool moved = x1 != x0 || y1 != y0;
      bool couldMove = (aDY > 0 && y0 < maxY) || (aDY < 0 && y0 > 0) ||
                       (aDX > 0 && x0 < maxX) || (aDX < 0 && x0 > 0);
      if (moved) {
        mScroll.inner = false;
      } else if (couldMove) {
        mScroll.inner = true;
      }
    }
  }
  void OnKey(uint8_t aAction, uint16_t aVirtualKey) override
  {
    if (aAction == 0) {   // the whole down/press/up sequence goes out on "down"
      Log("key %u", unsigned(aVirtualKey));
      DispatchRetrofoxKey(aVirtualKey);
    }
  }
  void OnText(const std::string& aUtf8) override
  {
    Log("text \"%s\"", aUtf8.c_str());
    NS_ConvertUTF8toUTF16 text(aUtf8.c_str(), aUtf8.size());
    DispatchRetrofoxText(text.get(), text.Length());
  }
  void OnDisconnect() override { Log("app disconnected"); }
  void OnLog(const std::string& aUtf8) override { Log("app: %s", aUtf8.c_str()); }

  bool mStateResend = false;
  rfxview::ScrollInfo mScroll = rfxview::ScrollInfo();
  // When the app last sent a scroll or a touch: strips are drawn only in the quiet after that
  PRIntervalTime mLastInput = PR_IntervalNow();

  // A touch the page took (or one that ended undecided): its held scrolling never applies, but the
  // app counts it as handled
  void DropHeldScroll()
  {
    mScroll.appliedX += mHeldDX;
    mScroll.appliedY += mHeldDY;
    mHeldDX = mHeldDY = 0;
  }
};

static rfxview::Server* sServer = nullptr;

// The frame buffer shared with the app when the engine runs inside it (HELLO format 2): the
// engine copies each changed rectangle here, the app copies it out (RetrofoxFrameLock/Unlock)
// before acknowledging the frame, so the next one never overwrites what the app hasn't taken.
class SharedFrameSink final : public rfxview::FrameSink
{
public:
  SharedFrameSink() { InitializeCriticalSection(&mLock); }

  bool Write(const uint8_t* aSurface, int aStride, int aWidth, int aHeight,
             int aX, int aY, int aW, int aH) override
  {
    EnterCriticalSection(&mLock);
    if (aWidth != mWidth || aHeight != mHeight) {
      mWidth = aWidth;
      mHeight = aHeight;
      mPixels.assign(size_t(aWidth) * aHeight * 4, 0xFF);
    }
    for (int y = aY; y < aY + aH; y++) {
      memcpy(&mPixels[(size_t(y) * aWidth + aX) * 4], aSurface + size_t(y) * aStride + size_t(aX) * 4,
             size_t(aW) * 4);
    }
    LeaveCriticalSection(&mLock);
    return true;
  }

  int32_t Lock(uint8_t** aData, int32_t* aWidth, int32_t* aHeight)
  {
    EnterCriticalSection(&mLock);
    *aData = mPixels.empty() ? nullptr : mPixels.data();
    *aWidth = mWidth;
    *aHeight = mHeight;
    return mWidth * 4;
  }

  void Unlock() { LeaveCriticalSection(&mLock); }

private:
  CRITICAL_SECTION mLock;
  std::vector<uint8_t> mPixels;
  int32_t mWidth = 0, mHeight = 0;
};

static SharedFrameSink* sSharedFrames = nullptr;

// For the app's process (P/Invoke): the shared frame buffer, top-down BGRA, locked until
// RetrofoxFrameUnlock. Returns the stride, or 0 with no buffer yet.
extern "C" __declspec(dllexport) int32_t
RetrofoxFrameLock(uint8_t** aData, int32_t* aWidth, int32_t* aHeight)
{
  if (!sSharedFrames) {
    *aData = nullptr;
    *aWidth = *aHeight = 0;
    return 0;
  }
  return sSharedFrames->Lock(aData, aWidth, aHeight);
}

extern "C" __declspec(dllexport) void
RetrofoxFrameUnlock()
{
  if (sSharedFrames) {
    sSharedFrames->Unlock();
  }
}

static void
OnTextFocus(bool aFocused)
{
  Log("text field %s", aFocused ? "focused: keyboard on" : "blurred: keyboard off");
  if (sServer) {
    sServer->SendFocus(aFocused);
  }
}

// ---- Startup ----

// Prefs Gecko has to see from the start, written to the profile's user.js before it starts: the
// HTTP user agent is built once at startup, so setting the override later (SetPrefs) changed
// navigator.userAgent nowhere.
//
// Present as what this is, a Windows Phone 8.1 touch phone on ARM, so sites serve their mobile
// pages. Without the overrides navigator.platform, oscpu and appVersion come from the OS build
// (Win32, Windows NT 6.3), which reads as a desktop. appName stays "Netscape", as in every modern
// browser. Touch: touch events, plus navigator.maxTouchPoints and (pointer: coarse) from
// widget/retrofox. The Firefox version is a current ESR's: with 52 (the Gecko generation UXP
// started from) sites call the browser outdated and YouTube refuses to load.
static const char kUserJs[] =
  "// Written by rfxengine at every start; edits are lost\n"
  // UXP's name for it (nsHttpHandler UA_PREF); general.useragent.override.<site> is per site
  "user_pref(\"network.http.useragent.global_override\", \"Mozilla/5.0 (Windows Phone 8.1; ARM; Mobile; "
  "Lumia; rv:153.0) Gecko/20100101 Firefox/153.0 Retrofox/0.1\");\n"
  "user_pref(\"general.platform.override\", \"ARM\");\n"
  "user_pref(\"general.oscpu.override\", \"Windows Phone 8.1\");\n"
  "user_pref(\"general.appversion.override\", \"5.0 (Windows Phone 8.1)\");\n"
  "user_pref(\"dom.w3c_touch_events.enabled\", 1);\n";

static void
WriteUserJs(nsIFile* aProfile)
{
  nsAutoString path;
  aProfile->GetPath(path);
  path.AppendLiteral(u"\\user.js");
  FILE* f = _wfopen(reinterpret_cast<const wchar_t*>(static_cast<const char16_t*>(path.get())),
                    L"wb");
  if (!f) {
    Log("can't write user.js");
    return;
  }
  fwrite(kUserJs, 1, sizeof(kUserJs) - 1, f);
  fclose(f);
}

static void
SetPrefs(int aScalePercent)
{
  // Software rendering only: no GPU compositing, no D2D/D3D on the phone
  Preferences::SetBool("layers.acceleration.disabled", true);
  Preferences::SetBool("gfx.direct2d.disabled", true);
  // Fonts come from DirectWrite (Skia draws the glyphs, grayscale antialiased): the phone has no
  // GDI text, so the GDI font path only gave fallbacks
  Preferences::SetBool("gfx.font_rendering.directwrite.enabled", true);
  Preferences::SetInt("gfx.font_rendering.cleartype_params.rendering_mode", 5);  // natural symmetric
  // Armenian pages (lang="hy") ask for Sylfaen and Arial AMU, which the phone hasn't got: Segoe UI
  // has Armenian (as do Tahoma and Verdana)
  for (const char* generic : { "serif", "sans-serif", "monospace" }) {
    nsPrintfCString name("font.name.%s.x-armn", generic);
    nsPrintfCString list("font.name-list.%s.x-armn", generic);
    Preferences::SetCString(name.get(), "Segoe UI");
    Preferences::SetCString(list.get(), "Segoe UI, Tahoma, Verdana");
  }
  Preferences::SetCString("gfx.canvas.azure.backends", "skia");
  Preferences::SetCString("gfx.content.azure.backends", "skia");
  // Lay out pages at phone width (<meta name=viewport>), at the phone's scale
  Preferences::SetBool("dom.meta-viewport.enabled", true);
  char scale[16];
  snprintf(scale, sizeof(scale), "%.2f", aScalePercent / 100.0);
  Preferences::SetCString("layout.css.devPixelsPerPx", scale);
  Preferences::SetBool("general.smoothScroll", false);
  // CSS that UXP has but keeps off by default, which current sites rely on: :has() (implemented,
  // with restyling), and font-display (text shows in a fallback font while web fonts download,
  // instead of staying invisible on a slow phone)
  Preferences::SetBool("layout.css.has-pseudo.enabled", true);
  // touch-action decides whether a drag pans or goes to the page (widget/retrofox)
  Preferences::SetBool("layout.css.touch_action.enabled", true);
  Preferences::SetBool("layout.css.font-display.enabled", true);
  // A SCROLL pixel moves the page one pixel (the app predicts where the page goes): no Windows
  // scroll speed override, no wheel acceleration
  Preferences::SetBool("mousewheel.system_scroll_override_on_root_content.enabled", false);
  Preferences::SetInt("mousewheel.acceleration.start", -1);
  // The identity prefs are in user.js (WriteUserJs): the HTTP user agent is built at startup
  // Memory: the Lumia has 1 GB
  Preferences::SetInt("browser.cache.memory.capacity", 8192);
  Preferences::SetInt("image.mem.surfacecache.max_size_kb", 32768);
  Preferences::SetBool("browser.dom.window.dump.enabled", true);
  // Connect directly. The system proxy setting points IE at rfxproxy (127.0.0.1:8080, the HTTPS
  // fixer for IE), which Gecko neither needs (it does TLS 1.3 itself) nor trusts.
  Preferences::SetInt("network.proxy.type", 0);
  // The phone reports the network as down while it idles (screen off, app in the background), and
  // Gecko then failed every load (unknown host, connection refused) until it was used again. The
  // engine only loads what the app asks for, so it never takes itself offline.
  Preferences::SetBool("network.manage-offline-status", false);
  // A failed load shows an error page instead of an alert (an embedding has no dialogs)
  Preferences::SetBool("browser.xul.error_pages.enabled", true);
}

static bool
WriteBmp(const char* aPath, const RetrofoxFrame& aFrame)
{
  FILE* f = fopen(aPath, "wb");
  if (!f) {
    return false;
  }
  uint32_t rowBytes = uint32_t(aFrame.mWidth) * 4;
  uint32_t imageSize = rowBytes * aFrame.mHeight;
  BITMAPFILEHEADER fh = {};
  BITMAPINFOHEADER ih = {};
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(fh) + sizeof(ih);
  fh.bfSize = fh.bfOffBits + imageSize;
  ih.biSize = sizeof(ih);
  ih.biWidth = aFrame.mWidth;
  ih.biHeight = -aFrame.mHeight;   // top-down
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  ih.biCompression = BI_RGB;
  ih.biSizeImage = imageSize;
  fwrite(&fh, sizeof(fh), 1, f);
  fwrite(&ih, sizeof(ih), 1, f);
  for (int32_t y = 0; y < aFrame.mHeight; ++y) {
    fwrite(aFrame.mData + size_t(y) * aFrame.mStride, 1, rowBytes, f);
  }
  fclose(f);
  return true;
}

// There's no debugger on the phone: on a crash, print where it happened as module+offset (the
// offsets map to source lines with llvm-symbolizer and the module's .pdb on the PC), plus the
// return addresses found on the stack.
static void
PrintAddress(const char* aWhat, uintptr_t aAddr)
{
  MEMORY_BASIC_INFORMATION info;
  wchar_t name[MAX_PATH] = L"?";
  uintptr_t base = 0;
  if (VirtualQuery(reinterpret_cast<void*>(aAddr), &info, sizeof(info)) && info.AllocationBase) {
    base = reinterpret_cast<uintptr_t>(info.AllocationBase);
    if (GetModuleFileNameW(static_cast<HMODULE>(info.AllocationBase), name, MAX_PATH)) {
      const wchar_t* slash = wcsrchr(name, L'\\');
      if (slash) {
        memmove(name, slash + 1, (wcslen(slash + 1) + 1) * sizeof(wchar_t));
      }
    }
  }
  printf("rfxengine:   %s %08x = %S+0x%x\n", aWhat, unsigned(aAddr), name, unsigned(aAddr - base));
}

static bool
IsCode(uintptr_t aAddr)
{
  MEMORY_BASIC_INFORMATION info;
  return VirtualQuery(reinterpret_cast<void*>(aAddr), &info, sizeof(info)) &&
         info.State == MEM_COMMIT &&
         (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
}

static void
ReportException(const char* aWhat, EXCEPTION_POINTERS* aInfo)
{
  const EXCEPTION_RECORD* rec = aInfo->ExceptionRecord;
  const CONTEXT* ctx = aInfo->ContextRecord;
  printf("rfxengine: %s %08x on thread %u", aWhat, unsigned(rec->ExceptionCode),
         unsigned(GetCurrentThreadId()));
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    printf(" (%s %08x)", rec->ExceptionInformation[0] ? "writing" : "reading",
           unsigned(rec->ExceptionInformation[1]));
  }
  printf("\n");
  // A deliberate crash (MOZ_CRASH, MOZ_RELEASE_ASSERT) says why: the linker folds identical
  // crash code together, so the address alone can name the wrong function
  if (gMozCrashReason) {
    printf("rfxengine:   reason: %s\n", gMozCrashReason);
  }
#ifndef _M_ARM
  // The desktop test build (x86): where it happened, then the code addresses found on the stack
  // (a scan, not an unwind: some are stale, but the callers are among them)
#ifdef _M_IX86
  PrintAddress("eip", ctx->Eip);
  const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(ctx->Esp);
  int found = 0;
  for (int i = 0; i < 2048 && found < 30; i++) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(sp + i, &info, sizeof(info)) || info.State != MEM_COMMIT) {
      break;
    }
    if (IsCode(sp[i])) {
      PrintAddress("stack", sp[i]);
      found++;
    }
  }
#else
  (void)ctx;
#endif
  fflush(stdout);
}
#else
  PrintAddress("pc", ctx->Pc);
  PrintAddress("lr", ctx->Lr);
  printf("rfxengine:   sp %08x r0 %08x r1 %08x r2 %08x r3 %08x r12 %08x cpsr %08x (%s)\n",
         unsigned(ctx->Sp), unsigned(ctx->R0), unsigned(ctx->R1), unsigned(ctx->R2),
         unsigned(ctx->R3), unsigned(ctx->R12), unsigned(ctx->Cpsr),
         (ctx->Cpsr & 0x20) ? "thumb" : "ARM state");
  // The instruction words at pc, to decode on the PC
  if (IsCode(ctx->Pc)) {
    const uint16_t* at = reinterpret_cast<const uint16_t*>(ctx->Pc & ~uintptr_t(1));
    printf("rfxengine:   code at pc: %04x %04x %04x %04x\n", at[0], at[1], at[2], at[3]);
  }
  // Walk the stack with the modules' unwind tables (.pdata)
  CONTEXT frame = *ctx;
  for (int depth = 0; depth < 40; depth++) {
    DWORD imageBase = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(frame.Pc, &imageBase, nullptr);
    if (fn) {
      void* handlerData;
      DWORD establisher;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, frame.Pc, fn, &frame, &handlerData,
                       &establisher, nullptr);
    } else {
      frame.Pc = frame.Lr;    // a leaf function: it returns to lr
    }
    if (!frame.Pc) {
      break;
    }
    if (!IsCode(frame.Pc)) {
      printf("rfxengine:   frame %08x (not code)\n", unsigned(frame.Pc));
      break;
    }
    // The return address is the instruction after the call: back up into the call itself
    PrintAddress("frame", (frame.Pc & ~uintptr_t(1)) - 2);
  }
  fflush(stdout);
}
#endif

static LONG WINAPI
OnCrash(EXCEPTION_POINTERS* aInfo)
{
  ReportException("crash", aInfo);
  return EXCEPTION_EXECUTE_HANDLER;
}

// Sees exceptions first, on every thread, even where something else handles them later (a
// crash in code with its own exception handling never reaches OnCrash). Reports the first few
// serious ones; some may be handled and harmless.
static LONG WINAPI
OnFirstChance(EXCEPTION_POINTERS* aInfo)
{
  static LONG sReported = 0;
  switch (aInfo->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case STATUS_STACK_BUFFER_OVERRUN:
      if (InterlockedIncrement(&sReported) <= 3) {
        ReportException("exception", aInfo);
      }
      break;
    default:
      break;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// Called by rfxengine.exe (../main.cpp). This code is part of libxul so it can use Gecko's
// internal API (the widget/retrofox frame, preferences, strings), which xul.dll doesn't export.
extern "C" MOZ_EXPORT int
RetrofoxEngineMain(int argc, char** argv)
{
  SetUnhandledExceptionFilter(OnCrash);
  AddVectoredExceptionHandler(1, OnFirstChance);
  const char* shotUrl = nullptr;
  const char* shotPath = nullptr;
  const char* home = "about:blank";
  const char* greArg = nullptr;       // Gecko's files (default: next to the exe)
  const char* profileArg = nullptr;   // the profile (default: .\profile next to them)
  uint16_t port = rfxview::kDefaultPort;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--in-app")) {
      // Running inside the Retrofox app's process (a thread of it), not as rfxengine.exe
      sInApp = true;
    } else if (!strcmp(argv[i], "--gre") && i + 1 < argc) {
      greArg = argv[++i];
    } else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
      profileArg = argv[++i];
    } else if (!strcmp(argv[i], "--log") && i + 1 < argc) {
      // No console inside the app: the log goes to a file
      freopen(argv[++i], "a", stdout);
      setvbuf(stdout, nullptr, _IONBF, 0);
    } else if (!strcmp(argv[i], "--shot") && i + 2 < argc) {
      shotUrl = argv[++i];
      shotPath = argv[++i];
    } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
      port = uint16_t(atoi(argv[++i]));
    } else if (!strcmp(argv[i], "--home") && i + 1 < argc) {
      home = argv[++i];
    } else if (!strcmp(argv[i], "--full-paint")) {
      // Redraw the whole screen on every paint, as before 0.3.5 (to compare)
      SetRetrofoxFullPaint(true);
    } else {
      printf("usage: rfxengine [--port N] [--home URL] [--gre DIR] [--profile DIR] [--log FILE]"
             " [--in-app] [--full-paint] | --shot URL out.bmp\n");
      return 2;
    }
  }

  rfxview::Server server;
  AppLink link;
  if (sInApp) {
    // The app reads frames from memory instead of the connection
    sSharedFrames = new SharedFrameSink();
    server.SetFrameSink(sSharedFrames);
  }
  if (!shotUrl) {
    if (!server.Listen(port)) {
      Log("%s", server.LastError().c_str());
      return 1;
    }
    sServer = &server;
    // Start Gecko at the app's screen size: wait for the first app to say hello
    Log("waiting for the Retrofox app on port %u", unsigned(port));
    while (!link.mHello) {
      Heartbeat();
      server.Poll(1000, &link);
    }
  }

  // widget/retrofox reads the screen size from here (nsScreenManager.cpp)
  char screen[64];
  snprintf(screen, sizeof(screen), "RETROFOX_SCREEN=%d,%d,%.2f",
           link.mWidth, link.mHeight, link.mScale / 100.0);
  _putenv(screen);

  Log("starting Gecko");
  // Gecko's files are next to the exe (or --gre); the profile in .\profile (or --profile)
  nsCOMPtr<nsIFile> dir;
  nsresult rv;
  if (greArg) {
    rv = NS_NewLocalFile(NS_ConvertUTF8toUTF16(greArg), false, getter_AddRefs(dir));
  } else {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) {
      *slash = 0;
    }
    rv = NS_NewLocalFile(nsDependentString(reinterpret_cast<char16_t*>(exePath)),
                         false, getter_AddRefs(dir));
  }
  if (NS_FAILED(rv)) {
    Log("bad Gecko directory");
    return 1;
  }
  nsCOMPtr<nsIFile> profile;
  if (profileArg) {
    rv = NS_NewLocalFile(NS_ConvertUTF8toUTF16(profileArg), false, getter_AddRefs(profile));
    if (NS_FAILED(rv)) {
      Log("bad profile directory");
      return 1;
    }
  } else {
    dir->Clone(getter_AddRefs(profile));
    profile->Append(NS_LITERAL_STRING("profile"));
  }
  bool exists = false;
  if (NS_SUCCEEDED(profile->Exists(&exists)) && !exists) {
    profile->Create(nsIFile::DIRECTORY_TYPE, 0700);
  }

  WriteUserJs(profile);

  Log("XRE_InitEmbedding2");
  RefPtr<RfxDirProvider> provider = new RfxDirProvider(profile);
  rv = XRE_InitEmbedding2(dir, dir, provider);
  if (NS_FAILED(rv)) {
    Log("XRE_InitEmbedding2 failed: 0x%08x", unsigned(rv));
    return 1;
  }
  Log("profile");
  XRE_NotifyProfile();
  SetPrefs(link.mScale);

  {
    // The app shell hooks the main thread's event loop (widget/retrofox/nsAppShell)
    nsCOMPtr<nsIAppShell> appShell = do_GetService(kAppShellCID);

    Log("creating the browser");
    RefPtr<RfxBrowser> browser = new RfxBrowser();
    rv = browser->Init(link.mWidth, link.mHeight);
    if (NS_FAILED(rv)) {
      Log("browser init failed: 0x%08x", unsigned(rv));
      XRE_TermEmbedding();
      return 1;
    }
    Log("browser ready, loading");
    link.mBrowser = browser;
    link.mServer = &server;
    link.mHome = home;
    link.AddTab(browser);
    SetRetrofoxTextFocusCallback(OnTextFocus);
    browser->Load(nsDependentCString(shotUrl ? shotUrl : home));

    nsCOMPtr<nsIThread> thread = do_GetCurrentThread();
    uint32_t lastSerial = 0;
    PRIntervalTime start = PR_IntervalNow();
    // How fast the engine draws, every 5 s while it does (to judge scrolling)
    // and where each loop's time goes: Gecko's events, painting, sending, waiting for the app
    struct {
      uint32_t paints = 0, frames = 0, loops = 0;
      double paintMs = 0, eventsMs = 0, sendMs = 0, pollMs = 0;
      PRIntervalTime since = PR_IntervalNow();
      void Report(rfxview::Server& aServer)
      {
        uint32_t ms = PR_IntervalToMilliseconds(PR_IntervalNow() - since);
        if (ms < 5000) {
          return;
        }
        rfxview::Server::Stats& s = aServer.stats;
        if (paints || s.scrolls) {
          Log("drawing: %u paints (%u ms each), %u frames sent in %u ms", paints,
              paints ? unsigned(paintMs / paints) : 0, frames, ms);
          Log("  loop: %u loops; ms in events %u, paint %u, send %u, poll %u; "
              "%u scroll msgs; %u acks (%u ms each); %u waits for an ack", loops,
              unsigned(eventsMs), unsigned(paintMs), unsigned(sendMs), unsigned(pollMs),
              s.scrolls, s.acks, s.acks ? unsigned(s.ackMs / s.acks) : 0, s.ackWaits);
        }
        // Every widget paint, including those run inside Gecko's events (the refresh driver's)
        uint32_t wPaints = 0, wShare = 0;
        double wMs = 0;
        TakeRetrofoxPaintStats(&wPaints, &wMs, &wShare);
        if (paints || s.scrolls || wPaints) {
          Log("  all paints: %u, %u ms in all (%u ms each), each redrew %u%% of the screen",
              wPaints, unsigned(wMs), wPaints ? unsigned(wMs / wPaints) : 0, wShare);
        }
        paints = frames = loops = 0;
        paintMs = eventsMs = sendMs = pollMs = 0;
        s = rfxview::Server::Stats();
        since = PR_IntervalNow();
      }
    } stats;
    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    auto nowMs = [&]() {
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      return double(t.QuadPart) * 1000.0 / double(qpf.QuadPart);
    };
    int exitCode = 0;
    for (;;) {
      Heartbeat();
      ++stats.loops;
      // Run Gecko for up to 16 ms, then paint and talk to the app
      double t0 = nowMs();
      link.ApplyPendingScroll();
      NS_ProcessPendingEvents(thread, PR_MillisecondsToInterval(16));
      double t1 = nowMs();
      stats.eventsMs += t1 - t0;
      if (PaintRetrofoxFrameNow()) {
        ++stats.paints;
        stats.paintMs += nowMs() - t1;
      }

      RetrofoxFrame frame;
      bool haveFrame = GetRetrofoxFrame(&frame);

      if (shotUrl) {
        // Done once the page has loaded and a paint after that has happened
        static uint32_t serialAtLoad = 0;
        if (browser->LoadedOnce() && !browser->Loading()) {
          if (!serialAtLoad) {
            serialAtLoad = haveFrame ? frame.mSerial + 1 : 1;
          } else if (haveFrame && frame.mSerial >= serialAtLoad) {
            bool ok = WriteBmp(shotPath, frame);
            Log(ok ? "wrote %s (%dx%d)" : "can't write %s", shotPath, frame.mWidth, frame.mHeight);
            exitCode = ok ? 0 : 1;
            break;
          }
        }
        if (PR_IntervalToSeconds(PR_IntervalNow() - start) > 120) {
          Log("timed out waiting for the page");
          if (haveFrame) {
            WriteBmp(shotPath, frame);
          }
          exitCode = 3;
          break;
        }
        continue;
      }

      if (haveFrame && frame.mSerial != lastSerial) {
        lastSerial = frame.mSerial;
        server.Invalidate(frame.mDirtyX, frame.mDirtyY, frame.mDirtyWidth, frame.mDirtyHeight);
      }
      if (haveFrame) {
        rfxview::Surface s = { frame.mData, frame.mWidth, frame.mHeight, frame.mStride };
        double t2 = nowMs();
        if (server.SendFrameIfReady(s)) {
          ++stats.frames;
        }
        stats.sendMs += nowMs() - t2;
      }
      // After the frame: where the page is scrolled in what was just painted
      RfxBrowser* active = link.mBrowser;
      rfxview::ScrollInfo scroll = link.mScroll;
      if (active->GetRootScroll(&scroll.x, &scroll.y, &scroll.maxX, &scroll.maxY)) {
        server.SendScrollInfo(scroll);
      }
      {
        static std::vector<int32_t> sFixed, sFixedLogged;
        active->GetFixedRects(sFixed);
        server.SendFixed(sFixed);
        if (sFixed != sFixedLogged) {
          sFixedLogged = sFixed;
          std::string list;
          for (size_t i = 0; i + 3 < sFixed.size(); i += 4) {
            char item[64];
            snprintf(item, sizeof(item), " %d,%d %dx%d", sFixed[i], sFixed[i + 1], sFixed[i + 2], sFixed[i + 3]);
            list += item;
          }
          Log("fixed parts:%s", list.empty() ? " none" : list.c_str());
        }
      }
      // Strips: the page just below and above the screen, drawn while nothing else is going on
      // (no input for a moment, the app has every frame, the page has loaded), so the app has
      // something to show at once when the finger moves the page (STRIP). One per loop.
      {
        static RfxBrowser* sStripPage = nullptr;
        static int32_t sBelowY = -1, sAboveY = -1, sStripX = -1;
        static uint32_t sStripSerial = 0;
        static PRIntervalTime sStripTime = 0;
        static std::vector<uint8_t> sStripPixels;
        int32_t sx = 0, sy = 0, maxX = 0, maxY = 0;
        int w = server.Width(), h = server.Height();
        if (active != sStripPage) {
          sStripPage = active;
          sBelowY = sAboveY = sStripX = -1;
        }
        static const bool kStrips = true;
        bool quiet = kStrips && PR_IntervalToMilliseconds(PR_IntervalNow() - link.mLastInput) > 250 &&
                     !server.FramesInFlight() && !active->Loading() && haveFrame;
        if (quiet && w > 0 && h > 0 && active->GetRootScroll(&sx, &sy, &maxX, &maxY)) {
          // The page changed under the strips (something repainted without scrolling): draw
          // them again, but not more often than every 2 s (animations)
          if (haveFrame && frame.mSerial != sStripSerial &&
              PR_IntervalToMilliseconds(PR_IntervalNow() - sStripTime) > 2000) {
            sBelowY = sAboveY = -1;
          }
          if (sx != sStripX) {
            sBelowY = sAboveY = -1;
          }
          int32_t belowY = sy + h, belowH = std::min<int32_t>(h, maxY - sy);
          int32_t aboveY = std::max<int32_t>(0, sy - h / 2), aboveH = sy - aboveY;
          bool belowStale = belowH > 0 && (sBelowY < 0 || std::abs(sBelowY - belowY) > h / 3);
          bool aboveStale = aboveH > 0 && (sAboveY < 0 || std::abs(sAboveY - aboveY) > h / 3);
          if (belowStale || aboveStale) {
            int32_t y = belowStale ? belowY : aboveY, sh = belowStale ? belowH : aboveH;
            PRIntervalTime t0 = PR_IntervalNow();
            if (active->RenderStrip(sx, y, w, sh, sStripPixels)) {
              server.SendStrip(sx, y, w, sh, sStripPixels.data());
              Log("strip %s: %d px at %d, %u ms", belowStale ? "below" : "above", sh, y,
                  PR_IntervalToMilliseconds(PR_IntervalNow() - t0));
            }
            (belowStale ? sBelowY : sAboveY) = y;
            sStripX = sx;
            sStripSerial = haveFrame ? frame.mSerial : 0;
            sStripTime = PR_IntervalNow();
          }
        }
      }
      stats.Report(server);
      // Each tab's title and address go in TABS; the tab on screen's whole state in STATE
      for (AppLink::Tab& tab : link.mTabs) {
        if (tab.browser && tab.browser->TakeStateChange()) {
          link.mTabsChanged = true;
          if (tab.browser == active) {
            link.mStateResend = true;
          }
        }
      }
      if (link.mStateResend) {
        link.mStateResend = false;
        server.SendState(active->StateJson());
      }
      // Who handles the touch in progress, for the app (GESTURE)
      static RetrofoxGesture sSentGesture = eRetrofoxGestureNone;
      RetrofoxGesture gesture = GetRetrofoxGesture();
      if (gesture != sSentGesture) {
        sSentGesture = gesture;
        server.SendGesture(uint8_t(gesture));
        if (gesture == eRetrofoxGesturePage || gesture == eRetrofoxGestureNone) {
          link.DropHeldScroll();
        }
      }
      if (link.mTabsChanged) {
        link.mTabsChanged = false;
        server.SendTabs(link.TabsJson());
      }
      double t3 = nowMs();
      server.Poll(NS_HasPendingEvents(thread) ? 0 : 10, &link);
      stats.pollMs += nowMs() - t3;
    }

    if (shotUrl) {
      // The screenshot is written. Gecko's shutdown currently hangs on the phone until its
      // watchdog (nsTerminator) crashes the process on purpose, so leave without it.
      // TODO: find the hang (it matters once the app can close the engine).
      fflush(stdout);
      TerminateProcess(GetCurrentProcess(), exitCode);
    }

    sServer = nullptr;
    SetRetrofoxTextFocusCallback(nullptr);
    link.mBrowser = nullptr;
    browser = nullptr;
    appShell = nullptr;
    XRE_TermEmbedding();
    return exitCode;
  }
}
