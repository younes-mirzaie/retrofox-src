using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading.Tasks;
using Windows.Data.Json;
using Windows.Graphics.Display;
using Windows.Networking;
using Windows.Networking.Connectivity;
using Windows.Phone.UI.Input;
using Windows.Storage;
using Windows.System;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;
using Windows.UI.Xaml.Navigation;

namespace Retrofox
{
    public sealed partial class MainPage : Page
    {
        const string EngineHostSetting = "engineHost";
        const double PanThreshold = 12;    // view pixels before a touch becomes a scroll

        EngineConnection _engine;
        bool _connecting;
        readonly DispatcherTimer _retryTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };

        // Surface in physical pixels, and its copy in BGRA
        int _surfaceW, _surfaceH;
        double _scale = 1;
        WriteableBitmap _bitmap;
        byte[] _pixels;
        readonly object _pixelLock = new object();

        // Changed area since the last blit, and the newest frame serial
        bool _blitQueued;
        int _dirtyX0, _dirtyY0, _dirtyX1, _dirtyY1;
        uint _latestSerial;

        // Scrolling moves the picture at once: by the SCROLL deltas sent that the frame on screen
        // doesn't show yet, within the page's scroll range. Frames then catch up.
        long _sentX, _sentY;            // SCROLL deltas sent since connecting
        ScrollInfo _shownScroll;        // scroll state of the frame on screen
        readonly List<ScrollInfo> _nextScroll = new List<ScrollInfo>();   // waiting for their frames
        uint _shownSerial;

        // Touch: a short touch is a tap, a moving one scrolls
        uint? _pointerId;
        Windows.Foundation.Point _pointerStart, _pointerLast;
        bool _panning;

        // Scrolling goes to the engine at most once per display frame (surface pixels, with the
        // fraction kept), and keeps going with friction after a flick, like the phone's own lists.
        const double FlingFriction = 325;     // ms: velocity falls to 1/e in this time
        const double FlingMinSpeed = 0.05;    // surface pixels per ms: below this it stops
        const double FlingSampleMs = 100;     // finger velocity is measured over this window
        double _scrollX, _scrollY;
        double _flingVX, _flingVY;
        bool _flinging, _renderHooked;
        readonly System.Diagnostics.Stopwatch _clock = System.Diagnostics.Stopwatch.StartNew();
        double _lastTick;
        readonly List<double[]> _moveSamples = new List<double[]>();   // { ms, dx, dy }

        bool _canGoBack, _canGoForward;
        string _pageUrl = "";

        public MainPage()
        {
            this.InitializeComponent();
            this.NavigationCacheMode = NavigationCacheMode.Required;
            _retryTimer.Tick += (s, e) => { var t = ConnectAsync(); };
            HardwareButtons.BackPressed += HardwareButtons_BackPressed;
            Windows.ApplicationModel.DataTransfer.DataTransferManager.GetForCurrentView().DataRequested += Share_DataRequested;
            var lists = LoadLists();
#if DEBUG
            DebugUiTour();
#endif
            CheckLastSession();
            InAppEngine.Failed += () =>
            {
                // The engine inside the app is gone (or never started): look for the separate one
                if (_engine != null)
                {
                    var old = _engine;
                    _engine = null;
                    old.Dispose();
                }
                _freshLaunch = true;
                var t = ConnectAsync();
            };
            InAppEngine.Start(_recoverUrl != null ? "about:blank" : HomePage);
            Application.Current.Suspending += (s, e) => MarkSession(false);
            Application.Current.Resuming += (s, e) => MarkSession(true);
            MarkSession(true);
            Application.Current.Resuming += (s, e) => { var t = ConnectAsync(); };
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            _scale = DisplayInformation.GetForCurrentView().RawPixelsPerViewPixel;
            OpenFromTile(e.Parameter as string);
        }

        // A pinned page's tile launches the app with the page's address
        public void OpenFromTile(string url)
        {
            if (string.IsNullOrEmpty(url) || !url.Contains("://"))
                return;
            if (_engine != null)
                Navigate(url);
            else
                _lastUrl = url;     // opened once the engine is up (AfterEngineCameBack / home)
        }

        // ---- Connection ----

        IEnumerable<string> EngineHosts()
        {
            var hosts = new List<string>();
            object custom;
            if (ApplicationData.Current.LocalSettings.Values.TryGetValue(EngineHostSetting, out custom) &&
                !string.IsNullOrWhiteSpace(custom as string))
                hosts.Add(((string)custom).Trim());
            // Packaged apps can't reach 127.0.0.1 on WP 8.1 (as with IE and rfxproxy), so the
            // phone's own addresses come next.
            hosts.Add("127.0.0.1");
            foreach (var h in NetworkInformation.GetHostNames())
                if (h.Type == HostNameType.Ipv4 && !hosts.Contains(h.CanonicalName))
                    hosts.Add(h.CanonicalName);
            return hosts;
        }

        async Task ConnectAsync()
        {
            if (_connecting || _engine != null || _surfaceW == 0)
                return;
            _connecting = true;
            _retryTimer.Stop();
            // Which engine: the one inside the app (port 8091) once it is starting, or the separate
            // one (8081)
            await InAppEngine.Decided;
            ShowStatus(_engineWasUp ? RestartingText : "Connecting to the Retrofox engine...");
            var engine = new EngineConnection();
            engine.FrameReceived += Engine_FrameReceived;
            engine.ScrollReceived += Engine_ScrollReceived;
            engine.TabsReceived += json => RunOnUi(() => ApplyTabs(json));
            engine.GestureReceived += g => RunOnUi(() => ApplyGesture(g));
            lock (_pixelLock)
            {
                _sentX = _sentY = 0;
                _shownScroll = null;
                _nextScroll.Clear();
                _shownSerial = _latestSerial = 0;
            }
            UpdatePageShift();
            engine.StateReceived += json => RunOnUi(() => ApplyState(json));
            engine.FocusReceived += focused => RunOnUi(() => SetKeyboard(focused));
            engine.Disconnected += () => RunOnUi(() => OnEngineLost(engine));
            bool ok = await engine.ConnectAsync(EngineHosts(), _surfaceW, _surfaceH,
                                                (int)Math.Round(_scale * 100));
            _connecting = false;
            if (ok)
            {
                _engine = engine;
                _failedConnects = 0;
                HideStatus();
                InAppEngine.AppLog("connected to the " + (EngineConnection.Port == InAppEngine.Port
                    ? "engine inside the app" : "separate engine on port " + EngineConnection.Port));
                MaybeRunBench();
                if (_freshLaunch && InAppEngine.Running)
                {
                    // The engine inside the app started with the app: it is fresh already
                    _freshLaunch = false;
                    if (_recoverUrl != null)
                        OfferRecovery(engine);
                    else if (_lastUrl.Length > 0)
                        Navigate(_lastUrl);     // launched from a pinned page's tile
                }
                else if (_freshLaunch)
                {
                    // The engine lives outside the app and keeps running between launches: a
                    // fresh launch (not a resume) asks for a clean one, which starts at the home
                    // page
                    _freshLaunch = false;
                    _restartRequested = true;
                    ShowStatus("Starting a fresh engine...");
                    var t = engine.SendCommand(EngineConnection.CmdRestartEngine);
                }
                else
                {
                    bool restarted = _restartRequested;
                    _restartRequested = false;
                    if (_engineWasUp)
                        AfterEngineCameBack(engine);
                    else if (restarted && _lastUrl.Length > 0)
                        Navigate(_lastUrl);     // launched from a pinned page's tile
                    else if (restarted && HomePage != DefaultHomePage)
                        Navigate(HomePage);     // the fresh engine opens the default home page
                }
                _engineWasUp = false;
            }
            else
            {
                engine.Dispose();
                if (++_failedConnects >= MaxFailedConnects)
                {
                    // Stop here rather than retry forever; the button starts over
                    ShowStatus("The Retrofox engine isn't answering.\n\nIf it doesn't come back, " +
                               "start rfxengine.exe on the phone again. Port " + EngineConnection.Port + ".",
                               "retry", RetryConnecting);
                    return;
                }
                if (_engineWasUp || _restartRequested)
                    ShowStatus(_restartRequested ? "Starting a fresh engine..." : RestartingText);
                else
                    ShowStatus("The Retrofox engine isn't running.\n\nStart rfxengine.exe on the phone " +
                               "(or rfxview-demo.exe to test). Port " + EngineConnection.Port +
                               ". Trying again every 2 seconds.");
                _retryTimer.Start();
            }
        }

        // The engine restarts itself after a crash or a hang (rfxengine.exe's watchdog) and the app
        // reconnects every 2 seconds, up to MaxFailedConnects times. A page that was still loading
        // when the engine went down is not opened again by itself (it may be what broke it): the
        // app offers Retry and Home instead. A page that had loaded is reopened once.
        const string RestartingText = "Engine restarting...";
        const int MaxFailedConnects = 15;
        bool _engineWasUp;
        bool _freshLaunch = true;
        bool _restartRequested;
        bool _loading;
        bool _lostWhileLoading;
        int _failedConnects;
        string _lastUrl = "";
        string _reopenedUrl = "";

        void OnEngineLost(EngineConnection engine)
        {
            if (_engine != engine)
                return;
            _engine = null;
            if (_restartRequested)
            {
                ShowStatus("Starting a fresh engine...");
            }
            else
            {
                _engineWasUp = true;
                _lostWhileLoading = _loading;
                ShowStatus(RestartingText);
            }
            _retryTimer.Start();
        }

        // ---- After a crash inside the app ----
        // The engine inside the app takes the app down with it when it crashes (a native crash
        // can't be caught). The app marks its session as running (cleared when it is suspended,
        // which is also how the phone closes it) with the page it shows; a launch that finds the
        // mark still set comes after a crash, and offers that page again rather than loading it
        // by itself, since it may be what crashed.
        const string SessionSetting = "sessionRunning", SessionUrlSetting = "sessionUrl";
        string _recoverUrl;

        void CheckLastSession()
        {
            var v = ApplicationData.Current.LocalSettings.Values;
            object running, url;
            if (v.TryGetValue(SessionSetting, out running) && running is bool && (bool)running &&
                v.TryGetValue(SessionUrlSetting, out url) && url is string && ((string)url).Length > 0)
                _recoverUrl = (string)url;
        }

        void MarkSession(bool running)
        {
            ApplicationData.Current.LocalSettings.Values[SessionSetting] = running;
        }

        void RememberSessionUrl(string url)
        {
            if (url.Length > 0 && url != "about:blank")
                ApplicationData.Current.LocalSettings.Values[SessionUrlSetting] = url;
        }

        void OfferRecovery(EngineConnection engine)
        {
            string url = _recoverUrl;
            _recoverUrl = null;
            ShowStatus("Retrofox closed unexpectedly while showing:\n" + url,
                       "retry", () => { HideStatus(); Navigate(url); },
                       "home", () => { HideStatus(); Navigate(HomePage); });
        }

        void AfterEngineCameBack(EngineConnection engine)
        {
            string url = _lastUrl;
            if (url.Length == 0)
                return;
            if (!_lostWhileLoading && url != _reopenedUrl)
            {
                _reopenedUrl = url;
                var t = engine.SendNavigate(url);
                return;
            }
            // Loading it broke the engine (or it already did once): ask instead of looping
            ShowStatus("This page stopped the engine:\n" + url,
                       "retry", () => { HideStatus(); Navigate(url); },
                       "home", () => { HideStatus(); Navigate(HomePage); });
        }

        void RetryConnecting()
        {
            _failedConnects = 0;
            var t = ConnectAsync();
        }

        Action _statusAction1, _statusAction2;

        void ShowStatus(string text, string button1 = null, Action action1 = null,
                        string button2 = null, Action action2 = null)
        {
            StatusText.Text = text;
            _statusAction1 = action1;
            _statusAction2 = action2;
            StatusButton1.Content = button1;
            StatusButton1.Visibility = button1 != null ? Visibility.Visible : Visibility.Collapsed;
            StatusButton2.Content = button2;
            StatusButton2.Visibility = button2 != null ? Visibility.Visible : Visibility.Collapsed;
            StatusPanel.Visibility = Visibility.Visible;
        }

        void StatusButton_Click(object sender, RoutedEventArgs e)
        {
            var a = sender == StatusButton1 ? _statusAction1 : _statusAction2;
            if (a != null)
                a();
        }

        void HideStatus() { StatusPanel.Visibility = Visibility.Collapsed; }

        // Opens a page the user chose (address bar, Retry, Home)
        void Navigate(string url)
        {
            _lastUrl = url;
            _reopenedUrl = "";
            if (_engine != null)
            {
                var t = _engine.SendNavigate(url);
            }
        }

        void RunOnUi(Action a)
        {
            var t = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () => a());
        }

        // ---- Surface and frames ----

        void PageArea_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            int w = (int)Math.Round(e.NewSize.Width * _scale);
            int h = (int)Math.Round(e.NewSize.Height * _scale);
            if (w <= 0 || h <= 0 || (w == _surfaceW && h == _surfaceH))
                return;
            bool first = _surfaceW == 0;
            // The picture moves while scrolling; nothing of it may show outside the page area
            PageArea.Clip = new RectangleGeometry { Rect = new Windows.Foundation.Rect(0, 0, e.NewSize.Width, e.NewSize.Height) };
            CreateSurface(w, h);
            if (first)
            {
                var t = ConnectAsync();
            }
            else if (_engine != null)
            {
                var t = _engine.SendResize(w, h);
            }
        }

        void CreateSurface(int w, int h)
        {
            lock (_pixelLock)
            {
                _surfaceW = w;
                _surfaceH = h;
                _pixels = new byte[w * h * 4];
                for (int i = 0; i < _pixels.Length; i++)
                    _pixels[i] = 0xFF;          // white until the first frame
            }
            _bitmap = new WriteableBitmap(w, h);
            PageImage.Source = _bitmap;
            QueueBlit(0, 0, w, h, 0);
        }

        // Background thread: copy the rectangle into _pixels, then blit on the UI thread.
        void Engine_FrameReceived(FrameMessage f)
        {
            lock (_pixelLock)
            {
                if (f.SurfaceWidth != _surfaceW || f.SurfaceHeight != _surfaceH ||
                    f.X + f.Width > _surfaceW || f.Y + f.Height > _surfaceH)
                {
                    // Stale size (we resized meanwhile); ack so the engine keeps going.
                    var e = _engine;
                    if (e != null) { var t = e.SendFrameAck(f.Serial); }
                    return;
                }
                int stride = _surfaceW * 4;
                double copyStart = StatNow();
                if (f.Format == EngineConnection.FormatShared)
                {
                    // The engine inside the app: straight from its frame buffer, then free it to
                    // draw the next one
                    InAppEngine.CopyFrame(f.X, f.Y, f.Width, f.Height, _pixels, _surfaceW, _surfaceH);
                    var e = _engine;
                    if (e != null) { var t = e.SendFrameAck(f.Serial); }
                }
                else if (f.Format == 0)
                {
                    for (int row = 0; row < f.Height; row++)
                        Buffer.BlockCopy(f.Pixels, row * f.Width * 4, _pixels,
                                         (f.Y + row) * stride + f.X * 4, f.Width * 4);
                }
                else
                {
                    int src = 0;
                    for (int row = 0; row < f.Height; row++)
                    {
                        int dst = (f.Y + row) * stride + f.X * 4;
                        for (int x = 0; x < f.Width; x++, src += 2, dst += 4)
                        {
                            int v = f.Pixels[src] | (f.Pixels[src + 1] << 8);
                            int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                            _pixels[dst] = (byte)((b << 3) | (b >> 2));
                            _pixels[dst + 1] = (byte)((g << 2) | (g >> 4));
                            _pixels[dst + 2] = (byte)((r << 3) | (r >> 2));
                            _pixels[dst + 3] = 0xFF;
                        }
                    }
                }
                CountFrame(f.Width, f.Height, StatNow() - copyStart);
                // In the same lock as the pixels: a blit in between would show this frame's
                // pixels with the previous frame's serial (and so its scroll position)
                QueueBlit(f.X, f.Y, f.Width, f.Height, f.Serial);
            }
        }

        void QueueBlit(int x, int y, int w, int h, uint serial)
        {
            bool schedule;
            lock (_pixelLock)
            {
                if (!_blitQueued)
                {
                    _dirtyX0 = x; _dirtyY0 = y; _dirtyX1 = x + w; _dirtyY1 = y + h;
                }
                else
                {
                    _dirtyX0 = Math.Min(_dirtyX0, x);
                    _dirtyY0 = Math.Min(_dirtyY0, y);
                    _dirtyX1 = Math.Max(_dirtyX1, x + w);
                    _dirtyY1 = Math.Max(_dirtyY1, y + h);
                }
                if (serial > _latestSerial)
                    _latestSerial = serial;
                schedule = !_blitQueued;
                _blitQueued = true;
            }
            if (schedule)
                RunOnUi(Blit);
        }

        // UI thread: copy the changed rows into the WriteableBitmap, then ack.
        void Blit()
        {
            uint serial;
            var bmp = _bitmap;
            double blitStart = StatNow();
            lock (_pixelLock)
            {
                _blitQueued = false;
                serial = _latestSerial;
                if (bmp == null || bmp.PixelWidth != _surfaceW || bmp.PixelHeight != _surfaceH)
                    return;
                int stride = _surfaceW * 4;
                int x0 = Math.Max(0, _dirtyX0), x1 = Math.Min(_surfaceW, _dirtyX1);
                int y0 = Math.Max(0, _dirtyY0), y1 = Math.Min(_surfaceH, _dirtyY1);
                if (x0 < x1 && y0 < y1)
                {
                    using (Stream s = bmp.PixelBuffer.AsStream())
                    {
                        for (int y = y0; y < y1; y++)
                        {
                            int off = y * stride + x0 * 4;
                            s.Seek(off, SeekOrigin.Begin);
                            s.Write(_pixels, off, (x1 - x0) * 4);
                        }
                    }
                }
            }
            bmp.Invalidate();
            lock (_pixelLock)
            {
                if (serial > _shownSerial)
                    _shownSerial = serial;
            }
            CountBlit(StatNow() - blitStart);
            TakeScrollInfo();
            if (serial != 0 && _engine != null)
            {
                var t = _engine.SendFrameAck(serial);
            }
        }

        // Background thread
        void Engine_ScrollReceived(ScrollInfo info)
        {
            lock (_pixelLock)
                _nextScroll.Add(info);
            RunOnUi(TakeScrollInfo);
        }

        // UI thread: the scroll state counts once the frame that shows it is on screen
        void TakeScrollInfo()
        {
            // The newest state whose frame is on screen; later ones wait (a state for frame 6 can
            // arrive before frame 5 is shown, and must not replace the one for frame 5)
            bool changed = false;
            lock (_pixelLock)
            {
                while (_nextScroll.Count > 0 && _nextScroll[0].Serial <= _shownSerial)
                {
                    _shownScroll = _nextScroll[0];
                    _nextScroll.RemoveAt(0);
                    changed = true;
                }
            }
            if (changed)
                UpdatePageShift();
        }

        // The touch's latest position, sent once per display frame
        double _pointerMoveX, _pointerMoveY;
        bool _pointerMoved;
        // The page handles the current drag itself (GESTURE from the engine): no scrolling here
        bool _pageHasGesture;

        void SendPendingMove()
        {
            if (!_pointerMoved || _engine == null || _pointerId == null)
                return;
            _pointerMoved = false;
            var s = ToSurface(new Windows.Foundation.Point(_pointerMoveX, _pointerMoveY));
            var t = _engine.SendPointer(EngineConnection.PointerMove, (int)s.X, (int)s.Y);
        }

        // UI thread: the engine decided who has the drag
        void ApplyGesture(byte g)
        {
            if (g != EngineConnection.GesturePage)
                return;
            // A swipe the page handles (a carousel, a pivot, a map): stop scrolling and drop the
            // picture shift for it (the engine counts those scroll steps as done, unapplied)
            _pageHasGesture = true;
            _flinging = false;
            _scrollX = _scrollY = 0;
            _moveSamples.Clear();
        }

        // UI thread: move the picture by the scrolling the engine hasn't drawn yet
        void UpdatePageShift()
        {
            double dx = 0, dy = 0;
            var s = _shownScroll;
            if (s != null && !s.Inner)
            {
                long wantX = Math.Max(0, Math.Min(s.MaxX, s.X + (_sentX - s.AppliedX)));
                long wantY = Math.Max(0, Math.Min(s.MaxY, s.Y + (_sentY - s.AppliedY)));
                dx = wantX - s.X;
                dy = wantY - s.Y;
            }
            PageShift.X = -dx / _scale;
            PageShift.Y = -dy / _scale;
        }

        // ---- Page state ----

        void ApplyState(string json)
        {
            JsonObject o;
            if (!JsonObject.TryParse(json, out o))
                return;
            _pageUrl = o.GetNamedString("url", "");
            if (_pageUrl.Length > 0 && _pageUrl != "about:blank")
                _lastUrl = _pageUrl;
            _pageTitle = o.GetNamedString("title", "");
            RememberSessionUrl(_pageUrl);
            ShowPageInBar();
            _canGoBack = o.GetNamedBoolean("back", false);
            _canGoForward = o.GetNamedBoolean("forward", false);
            bool loading = o.GetNamedBoolean("loading", false);
            _loading = loading;
            UpdateLoadingUi(loading, o.GetNamedNumber("progress", 0));
        }

        // ---- Address bar and buttons ----

        void UrlBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key != VirtualKey.Enter)
                return;
            e.Handled = true;
            string url = ToUrl(UrlBox.Text);
            if (url.Length == 0)
                return;
            UrlBox.Text = url;
            FocusSink.Focus(FocusState.Programmatic);   // hides the keyboard
            _loading = true;    // until the engine says otherwise: it may go down loading it
            Navigate(url);
        }

        static string ToUrl(string text)
        {
            text = text.Trim();
            if (text.Length == 0)
                return text;
            if (text.Contains("://") || text.StartsWith("about:", StringComparison.OrdinalIgnoreCase))
                return text;
            if (!text.Contains(" ") && text.Contains("."))
                return "https://" + text;
            return "https://html.duckduckgo.com/html/?q=" + Uri.EscapeDataString(text);
        }


        void SendCommand(byte c)
        {
            if (_engine != null)
            {
                var t = _engine.SendCommand(c);
            }
        }

        void HardwareButtons_BackPressed(object sender, BackPressedEventArgs e)
        {
            if (CloseOverlays())
            {
                e.Handled = true;
            }
            else if (KeyboardSink.FocusState != FocusState.Unfocused)
            {
                FocusSink.Focus(FocusState.Programmatic);
                e.Handled = true;
            }
            else if (_canGoBack && _engine != null)
            {
                SendCommand(EngineConnection.CmdBack);
                e.Handled = true;
            }
        }

        // Developer option, not in the menu: the app normally always uses the engine on this
        // phone. Press and hold refresh to point it at another one (e.g. a desktop engine).
        void RefreshButton_Holding(object sender, HoldingRoutedEventArgs e)
        {
            if (e.HoldingState == Windows.UI.Input.HoldingState.Started)
                EditEngineAddress();
        }

        async void EditEngineAddress()
        {
            object current;
            ApplicationData.Current.LocalSettings.Values.TryGetValue(EngineHostSetting, out current);
            var box = new TextBox
            {
                Text = (current as string) ?? "",
                PlaceholderText = "automatic (this phone)",
                InputScope = new InputScope { Names = { new InputScopeName(InputScopeNameValue.Url) } }
            };
            var dialog = new ContentDialog
            {
                Title = "Engine address",
                Content = box,
                PrimaryButtonText = "save",
                SecondaryButtonText = "cancel"
            };
            if (await dialog.ShowAsync() != ContentDialogResult.Primary)
                return;
            ApplicationData.Current.LocalSettings.Values[EngineHostSetting] = box.Text.Trim();
            if (_engine != null)
            {
                var old = _engine;
                _engine = null;
                old.Dispose();
            }
            var t = ConnectAsync();
        }

        // ---- Touch ----

        Windows.Foundation.Point ToSurface(Windows.Foundation.Point p)
        {
            return new Windows.Foundation.Point(p.X * _scale, p.Y * _scale);
        }

        void PageArea_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (_pointerId != null)
                return;
            _pointerId = e.Pointer.PointerId;
            PageArea.CapturePointer(e.Pointer);
            _pointerStart = _pointerLast = e.GetCurrentPoint(PageArea).Position;
            _panning = false;
            _pageHasGesture = false;
            // A touch catches a flick in progress, as in native lists
            _flinging = false;
            _moveSamples.Clear();
            // The page gets the touch as it happens (touch events); the engine decides whether a
            // drag scrolls or is the page's own (GESTURE)
            if (_engine != null)
            {
                var s = ToSurface(_pointerStart);
                var t = _engine.SendPointer(EngineConnection.PointerDown, (int)s.X, (int)s.Y);
            }
            e.Handled = true;
        }

        void PageArea_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (_pointerId != e.Pointer.PointerId)
                return;
            var p = e.GetCurrentPoint(PageArea).Position;
            // The touch's position goes to the page once per display frame (OnRendering)
            _pointerMoveX = p.X;
            _pointerMoveY = p.Y;
            _pointerMoved = true;
            HookRendering();
            if (!_panning)
            {
                double dx = p.X - _pointerStart.X, dy = p.Y - _pointerStart.Y;
                if (dx * dx + dy * dy < PanThreshold * PanThreshold)
                    return;
                _panning = true;
            }
            if (_pageHasGesture)
            {
                // The page moves things itself: no scrolling, no picture shift
                _pointerLast = p;
                e.Handled = true;
                return;
            }
            // Finger up = content up = scroll down
            double moveX = (_pointerLast.X - p.X) * _scale;
            double moveY = (_pointerLast.Y - p.Y) * _scale;
            _pointerLast = p;
            _scrollX += moveX;
            _scrollY += moveY;
            double now = _clock.Elapsed.TotalMilliseconds;
            _moveSamples.Add(new[] { now, moveX, moveY });
            while (_moveSamples.Count > 0 && now - _moveSamples[0][0] > FlingSampleMs)
                _moveSamples.RemoveAt(0);
            HookRendering();
            e.Handled = true;
        }

        void PageArea_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            if (_pointerId != e.Pointer.PointerId)
                return;
            _pointerId = null;
            PageArea.ReleasePointerCapture(e.Pointer);
            if (_engine != null)
            {
                SendPendingMove();
                var p = ToSurface(e.GetCurrentPoint(PageArea).Position);
                var t = _engine.SendPointer(EngineConnection.PointerUp, (int)p.X, (int)p.Y);
            }
            if (_panning && !_pageHasGesture)
                StartFling();
            e.Handled = true;
        }

        // Finger velocity over the last FlingSampleMs carries on after release
        void StartFling()
        {
            double now = _clock.Elapsed.TotalMilliseconds;
            while (_moveSamples.Count > 0 && now - _moveSamples[0][0] > FlingSampleMs)
                _moveSamples.RemoveAt(0);
            if (_moveSamples.Count < 2)
                return;
            double sx = 0, sy = 0;
            foreach (var s in _moveSamples) { sx += s[1]; sy += s[2]; }
            double span = Math.Max(now - _moveSamples[0][0], 16);
            _flingVX = sx / span;
            _flingVY = sy / span;
            if (Math.Abs(_flingVX) < FlingMinSpeed && Math.Abs(_flingVY) < FlingMinSpeed)
                return;
            _flinging = true;
            _lastTick = now;
            HookRendering();
        }

        void HookRendering()
        {
            if (_renderHooked)
                return;
            _renderHooked = true;
            CompositionTarget.Rendering += OnRendering;
        }

        // Once per display frame: advance a flick, then send whatever scrolling built up
        void OnRendering(object sender, object e)
        {
            double now = _clock.Elapsed.TotalMilliseconds;
            if (_flinging)
            {
                double dt = Math.Min(now - _lastTick, 50);
                _lastTick = now;
                _scrollX += _flingVX * dt;
                _scrollY += _flingVY * dt;
                double decay = Math.Exp(-dt / FlingFriction);
                _flingVX *= decay;
                _flingVY *= decay;
                if (Math.Abs(_flingVX) < FlingMinSpeed && Math.Abs(_flingVY) < FlingMinSpeed)
                    _flinging = false;
            }
            SendPendingMove();
            if (_pageHasGesture)
                _scrollX = _scrollY = 0;
            int sx = (int)_scrollX, sy = (int)_scrollY;
            if ((sx != 0 || sy != 0) && _engine != null)
            {
                _scrollX -= sx;
                _scrollY -= sy;
                _sentX += sx;
                _sentY += sy;
                BarScrolled(sy);
                var t = _engine.SendScroll(sx, sy);
                UpdatePageShift();
            }
            if (!_flinging && _pointerId == null)
            {
                _scrollX = _scrollY = 0;
                CompositionTarget.Rendering -= OnRendering;
                _renderHooked = false;
            }
        }

        void PageArea_PointerCanceled(object sender, PointerRoutedEventArgs e)
        {
            if (_pointerId == e.Pointer.PointerId)
                _pointerId = null;
        }

        // ---- Keyboard for the page's text fields ----

        void SetKeyboard(bool show)
        {
            if (show)
            {
                bool ok = KeyboardSink.Focus(FocusState.Programmatic);
                AppLog("keyboard on: focus " + (ok ? "taken" : "REFUSED") + ", state " + KeyboardSink.FocusState);
            }
            else if (KeyboardSink.FocusState != FocusState.Unfocused)
            {
                AppLog("keyboard off");
                FocusSink.Focus(FocusState.Programmatic);
            }
        }

        // A line in the engine's log (live.log on the phone): the app has none of its own
        void AppLog(string line)
        {
            if (_engine != null)
            {
                var t = _engine.SendLog(line);
            }
        }

        void KeyboardSink_LostFocus(object sender, RoutedEventArgs e)
        {
            AppLog("keyboard box lost focus");
        }

        // The hidden box keeps what was typed since it got focus, and only the difference goes to
        // the engine. Clearing it after every key (as before) cancelled the phone keyboard's
        // composition, so nothing ever arrived.
        bool _clearingSink;
        string _sinkText = "";

        void KeyboardSink_GotFocus(object sender, RoutedEventArgs e)
        {
            _clearingSink = true;
            KeyboardSink.Text = "";
            _clearingSink = false;
            _sinkText = "";
            AppLog("keyboard box got focus");
        }

        // TextChanged never fires for this hidden box on the phone (the text does land in it), so
        // the text is also compared after every key: on KeyUp, and once the KeyDown has been handled.
        void KeyboardSink_TextChanged(object sender, TextChangedEventArgs e) { SyncSink(); }

        void KeyboardSink_KeyUp(object sender, KeyRoutedEventArgs e) { SyncSink(); }

        // Sends what changed in the box since the last call: deletions as backspaces, then new text
        void SyncSink()
        {
            if (_clearingSink)
                return;
            string text = KeyboardSink.Text;
            if (text == _sinkText)
                return;
            int same = 0;
            while (same < text.Length && same < _sinkText.Length && text[same] == _sinkText[same])
                same++;
            int deleted = _sinkText.Length - same;
            string added = text.Substring(same);
            _sinkText = text;
            AppLog("keyboard box text: " + text.Length + " chars, +" + added.Length + " -" + deleted);
            if (_engine == null)
                return;
            for (int i = 0; i < deleted; i++)
                SendKeyPress(VirtualKey.Back);
            if (added.Length > 0)
            {
                var t = _engine.SendText(added);
            }
        }

        void KeyboardSink_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (_engine == null)
                return;
            var sync = Dispatcher.RunAsync(CoreDispatcherPriority.Low, SyncSink);
            // Backspace on an empty box changes no text, so it only shows up here
            if (e.Key == VirtualKey.Enter || (e.Key == VirtualKey.Back && KeyboardSink.Text.Length == 0))
            {
                SendKeyPress(e.Key);
                e.Handled = true;
            }
        }

        void SendKeyPress(VirtualKey key)
        {
            var t1 = _engine.SendKey(true, (int)key);
            var t2 = _engine.SendKey(false, (int)key);
        }
    }
}
