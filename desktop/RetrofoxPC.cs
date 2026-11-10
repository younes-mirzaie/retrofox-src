// Retrofox for PC: a window around the desktop (x86) build of the Retrofox engine.
//
// It starts rfxengine.exe (next to it in engine\, or in C:\rfx\obj-desktop\dist\bin) on a free
// loopback port and talks to it with the same protocol as the phone app (viewer/PROTOCOL.md):
// frames come in as BGRA rectangles, mouse, wheel and keyboard go out. The look follows Internet
// Explorer on Windows Phone 8.1: the address bar at the bottom with the round tabs button and
// the small "..." that opens a menu, and a full-window tabs view.
//
// Build: desktop\build.ps1 (the .NET Framework 4 compiler that ships with Windows, no SDK).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace RetrofoxPC
{
    // ---- The connection to the engine (rfxview protocol, version 1) ----

    sealed class Engine : IDisposable
    {
        const ushort Hello = 100, Navigate = 101, Command = 102, Pointer = 103, Scroll = 104, Key = 105,
                     Text = 106, Resize = 107, FrameAck = 108, Tab = 110;
        public const byte CmdReload = 0, CmdBack = 1, CmdForward = 2, CmdStop = 3;
        public const byte PointerDown = 0, PointerMove = 1, PointerUp = 2;
        public const byte TabNew = 0, TabSelect = 1, TabClose = 2;

        readonly TcpClient _tcp;
        readonly NetworkStream _net;
        readonly object _sendLock = new object();
        public event Action<uint, int, int, int, int, int, int, byte[]> FrameReceived; // serial, sw, sh, x, y, w, h, pixels
        public event Action<string> StateReceived, TabsReceived;
        public event Action Closed;

        Engine(TcpClient tcp) { _tcp = tcp; _tcp.NoDelay = true; _net = tcp.GetStream(); }

        public static Engine Connect(int port, int width, int height, int scalePercent)
        {
            var tcp = new TcpClient();
            tcp.Connect(IPAddress.Loopback, port);
            var e = new Engine(tcp);
            var p = new byte[9];
            W16(p, 0, 1); W16(p, 2, width); W16(p, 4, height); W16(p, 6, scalePercent); p[8] = 0; // BGRA
            e.Send(Hello, p);
            var t = new Thread(e.ReadLoop) { IsBackground = true, Name = "engine reader" };
            t.Start();
            return e;
        }

        static void W16(byte[] b, int o, int v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); }
        static void W32(byte[] b, int o, uint v) { W16(b, o, (int)(v & 0xFFFF)); W16(b, o + 2, (int)(v >> 16)); }
        static int R16(byte[] b, int o) { return b[o] | (b[o + 1] << 8); }
        static uint R32(byte[] b, int o) { return (uint)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)); }

        void Send(ushort type, byte[] payload)
        {
            var head = new byte[8];
            W16(head, 0, type);
            W32(head, 4, (uint)payload.Length);
            try
            {
                lock (_sendLock)
                {
                    _net.Write(head, 0, 8);
                    if (payload.Length > 0) _net.Write(payload, 0, payload.Length);
                }
            }
            catch (Exception) { }
        }

        public void SendNavigate(string url) { Send(Navigate, Encoding.UTF8.GetBytes(url)); }
        public void SendCommand(byte c) { Send(Command, new[] { c }); }
        public void SendText(string s) { Send(Text, Encoding.UTF8.GetBytes(s)); }
        public void SendKey(int vk) { var p = new byte[4]; W16(p, 2, vk); Send(Key, p); }
        public void SendResize(int w, int h) { var p = new byte[4]; W16(p, 0, w); W16(p, 2, h); Send(Resize, p); }
        public void SendAck(uint serial) { var p = new byte[4]; W32(p, 0, serial); Send(FrameAck, p); }

        public void SendPointer(byte action, int x, int y)
        {
            var p = new byte[6];
            p[0] = action;
            W16(p, 2, (short)x); W16(p, 4, (short)y);
            Send(Pointer, p);
        }

        public void SendScroll(int dx, int dy)
        {
            var p = new byte[4];
            W16(p, 0, (short)dx); W16(p, 2, (short)dy);
            Send(Scroll, p);
        }

        public void SendTab(byte action, uint id, string url)
        {
            var u = Encoding.UTF8.GetBytes(url ?? "");
            var p = new byte[8 + u.Length];
            p[0] = action;
            W32(p, 4, id);
            Buffer.BlockCopy(u, 0, p, 8, u.Length);
            Send(Tab, p);
        }

        void ReadExactly(byte[] b, int n)
        {
            int got = 0;
            while (got < n)
            {
                int r = _net.Read(b, got, n - got);
                if (r <= 0) throw new IOException("closed");
                got += r;
            }
        }

        void ReadLoop()
        {
            try
            {
                var head = new byte[8];
                for (;;)
                {
                    ReadExactly(head, 8);
                    int type = R16(head, 0);
                    uint len = R32(head, 4);
                    if (len > 64 * 1024 * 1024) throw new IOException("bad length");
                    var p = new byte[len];
                    ReadExactly(p, (int)len);
                    switch (type)
                    {
                        case 2:     // FRAME
                            if (len < 20 || p[16] != 0) break;
                            int w = R16(p, 12), h = R16(p, 14);
                            if (len < 20 + (uint)(w * h * 4)) break;
                            var px = new byte[w * h * 4];
                            Buffer.BlockCopy(p, 20, px, 0, px.Length);
                            var f = FrameReceived;
                            if (f != null) f(R32(p, 0), R16(p, 4), R16(p, 6), R16(p, 8), R16(p, 10), w, h, px);
                            break;
                        case 3: { var s = StateReceived; if (s != null) s(Encoding.UTF8.GetString(p)); break; }
                        case 6: { var s = TabsReceived; if (s != null) s(Encoding.UTF8.GetString(p)); break; }
                    }
                }
            }
            catch (Exception) { }
            var c = Closed;
            if (c != null) c();
        }

        public void Dispose() { try { _tcp.Close(); } catch (Exception) { } }
    }

    // ---- The window ----

    sealed class TabInfo { public uint Id; public string Title, Url; }

    sealed class MainWindow : Window
    {
        static readonly Brush Chrome = new SolidColorBrush(Color.FromRgb(0x1F, 0x1F, 0x1F));
        static readonly Brush Fore = Brushes.White;
        static readonly Brush Mid = new SolidColorBrush(Color.FromRgb(0x99, 0x99, 0x99));
        static readonly Brush Accent = new SolidColorBrush(Color.FromRgb(0x1B, 0xA1, 0xE2));
        static readonly FontFamily Segoe = new FontFamily("Segoe UI");
        static readonly FontFamily SegoeLight = new FontFamily("Segoe UI Light");
        static readonly FontFamily Symbols = new FontFamily("Segoe UI Symbol");
        const string Home = "https://www.google.com/";

        Process _engineProcess;
        Engine _engine;
        int _port;
        readonly string _logPath;

        readonly Grid _pageArea = new Grid { Background = Brushes.White, ClipToBounds = true };
        readonly Image _page = new Image { Stretch = Stretch.None, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
        WriteableBitmap _bitmap;
        int _surfaceW, _surfaceH;
        double _dpi = 1;

        readonly TextBox _url = new TextBox();
        readonly TextBlock _urlHint = new TextBlock();
        readonly TextBlock _refreshGlyph = new TextBlock();
        readonly TextBlock _tabCount = new TextBlock();
        readonly ProgressBar _progress = new ProgressBar();
        readonly StackPanel _menu = new StackPanel { Visibility = Visibility.Collapsed };
        readonly TextBlock _menuBack = new TextBlock(), _menuForward = new TextBlock();
        readonly Grid _tabsView = new Grid { Visibility = Visibility.Collapsed };
        readonly WrapPanel _tabsGrid = new WrapPanel();
        readonly TextBlock _status = new TextBlock();

        readonly List<TabInfo> _tabs = new List<TabInfo>();
        readonly Dictionary<uint, BitmapSource> _thumbs = new Dictionary<uint, BitmapSource>();
        uint _activeTab;
        string _pageUrl = "", _pageTitle = "";
        bool _loading, _canBack, _canForward, _mouseDown;
        readonly DispatcherTimer _resizeTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };

        public MainWindow()
        {
            Title = "Retrofox";
            Width = 520; Height = 900;
            Background = Chrome;
            FontFamily = Segoe;
            _logPath = System.IO.Path.Combine(DataDir, "engine.log");

            var root = new Grid();
            root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            Content = root;

            _pageArea.Children.Add(_page);
            _status.Foreground = Brushes.Black; _status.FontSize = 18; _status.Margin = new Thickness(24);
            _status.TextWrapping = TextWrapping.Wrap; _status.Text = "Starting the Retrofox engine...";
            _pageArea.Children.Add(_status);
            root.Children.Add(_pageArea);
            root.Children.Add(BuildBar());
            Grid.SetRow(root.Children[1], 1);
            BuildTabsView();
            root.Children.Add(_tabsView);
            Grid.SetRowSpan(_tabsView, 2);

            _pageArea.MouseDown += Page_MouseDown;
            _pageArea.MouseMove += Page_MouseMove;
            _pageArea.MouseUp += Page_MouseUp;
            _pageArea.MouseWheel += Page_MouseWheel;
            _pageArea.SizeChanged += (s, e) => { _resizeTimer.Stop(); _resizeTimer.Start(); };
            _resizeTimer.Tick += (s, e) => { _resizeTimer.Stop(); ApplySize(); };
            PreviewKeyDown += Window_PreviewKeyDown;
            TextInput += Window_TextInput;
            Loaded += (s, e) =>
            {
                var src = PresentationSource.FromVisual(this);
                if (src != null) _dpi = src.CompositionTarget.TransformToDevice.M11;
                StartEngine();
            };
            Closed += (s, e) => StopEngine();
        }

        static string DataDir
        {
            get
            {
                var d = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RetrofoxPC");
                Directory.CreateDirectory(d);
                return d;
            }
        }

        // ---- The bottom bar, as IE on Windows Phone 8.1 ----

        UIElement BuildBar()
        {
            var bar = new StackPanel { Background = Chrome };
            var row = new Grid { Height = 60 };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            // Tabs: two pages in a circle, with the count when there's more than one
            var tabs = new Grid { Width = 62, Height = 60, Background = Brushes.Transparent, Cursor = Cursors.Hand, ToolTip = "Tabs (Ctrl+T new, Ctrl+W close)" };
            tabs.Children.Add(new Ellipse { Width = 40, Height = 40, Stroke = Fore, StrokeThickness = 2 });
            tabs.Children.Add(new Rectangle { Width = 13, Height = 15, Stroke = Fore, StrokeThickness = 2, Margin = new Thickness(0, 0, 5, 5) });
            tabs.Children.Add(new Rectangle { Width = 13, Height = 15, Fill = Fore, Margin = new Thickness(6, 6, 0, 0) });
            _tabCount.FontSize = 10; _tabCount.FontWeight = FontWeights.SemiBold; _tabCount.Foreground = Chrome;
            _tabCount.Margin = new Thickness(6, 6, 0, 0);
            _tabCount.HorizontalAlignment = HorizontalAlignment.Center; _tabCount.VerticalAlignment = VerticalAlignment.Center;
            tabs.Children.Add(_tabCount);
            tabs.MouseLeftButtonUp += (s, e) => OpenTabsView();
            row.Children.Add(tabs);

            // The address field, with refresh (or stop while loading) inside it
            var field = new Grid { VerticalAlignment = VerticalAlignment.Center };
            _url.Height = 40; _url.FontSize = 20; _url.BorderThickness = new Thickness(0);
            _url.Background = new SolidColorBrush(Color.FromRgb(0xD4, 0xD4, 0xD4));
            _url.Foreground = new SolidColorBrush(Color.FromRgb(0x33, 0x33, 0x33));
            _url.Padding = new Thickness(6, 4, 40, 4); _url.VerticalContentAlignment = VerticalAlignment.Center;
            _url.KeyDown += Url_KeyDown;
            _url.GotKeyboardFocus += (s, e) => { _url.Text = _pageUrl; _url.SelectAll(); UpdateHint(); };
            _url.LostKeyboardFocus += (s, e) => ShowAddress();
            _url.TextChanged += (s, e) => UpdateHint();
            _url.PreviewMouseLeftButtonDown += (s, e) => { if (!_url.IsKeyboardFocusWithin) { _url.Focus(); e.Handled = true; } };
            field.Children.Add(_url);
            _urlHint.Text = "browse or search"; _urlHint.FontSize = 20; _urlHint.IsHitTestVisible = false;
            _urlHint.Foreground = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x77));
            _urlHint.Margin = new Thickness(9, 0, 0, 0); _urlHint.VerticalAlignment = VerticalAlignment.Center;
            field.Children.Add(_urlHint);
            var refresh = new Grid { Width = 40, Height = 40, HorizontalAlignment = HorizontalAlignment.Right, Background = Brushes.Transparent, Cursor = Cursors.Hand, ToolTip = "Refresh (F5)" };
            _refreshGlyph.Text = "\uE149"; _refreshGlyph.FontFamily = Symbols; _refreshGlyph.FontSize = 20;
            _refreshGlyph.Foreground = new SolidColorBrush(Color.FromRgb(0x1F, 0x1F, 0x1F));
            _refreshGlyph.HorizontalAlignment = HorizontalAlignment.Center; _refreshGlyph.VerticalAlignment = VerticalAlignment.Center;
            refresh.Children.Add(_refreshGlyph);
            refresh.MouseLeftButtonUp += (s, e) => { if (_engine != null) _engine.SendCommand(_loading ? Engine.CmdStop : Engine.CmdReload); };
            field.Children.Add(refresh);
            Grid.SetColumn(field, 1);
            row.Children.Add(field);

            // "...": small, at the top right, as the app bar's
            var more = new Grid { Width = 50, Height = 60, Background = Brushes.Transparent, Cursor = Cursors.Hand, ToolTip = "More" };
            more.Children.Add(new TextBlock { Text = "\u2022\u2022\u2022", FontSize = 14, Foreground = Fore, Margin = new Thickness(0, 4, 0, 0), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Top });
            more.MouseLeftButtonUp += (s, e) => ToggleMenu();
            Grid.SetColumn(more, 2);
            row.Children.Add(more);

            var top = new Grid();
            top.Children.Add(row);
            _progress.Height = 3; _progress.VerticalAlignment = VerticalAlignment.Top; _progress.BorderThickness = new Thickness(0);
            _progress.Background = Brushes.Transparent; _progress.Foreground = Accent; _progress.Visibility = Visibility.Collapsed;
            top.Children.Add(_progress);
            bar.Children.Add(top);

            // The "..." menu, below the address row, as the phone's app bar menu
            _menu.Margin = new Thickness(0, 2, 0, 14);
            _menu.Children.Add(MenuItem(_menuBack, "back", () => Command(Engine.CmdBack)));
            _menu.Children.Add(MenuItem(_menuForward, "forward", () => Command(Engine.CmdForward)));
            _menu.Children.Add(MenuItem(new TextBlock(), "new tab", NewTab));
            _menu.Children.Add(MenuItem(new TextBlock(), "tabs", OpenTabsView));
            _menu.Children.Add(MenuItem(new TextBlock(), "home", () => Go(Home)));
            _menu.Children.Add(MenuItem(new TextBlock(), "open the engine log", () => { try { Process.Start("notepad.exe", "\"" + _logPath + "\""); } catch (Exception) { } }));
            bar.Children.Add(_menu);
            return bar;
        }

        UIElement MenuItem(TextBlock t, string text, Action action)
        {
            t.Text = text; t.FontSize = 25.33; t.FontFamily = SegoeLight; t.Foreground = Fore;
            t.Padding = new Thickness(20, 8, 20, 8); t.Background = Brushes.Transparent; t.Cursor = Cursors.Hand;
            t.MouseLeftButtonUp += (s, e) => { CloseMenu(); action(); };
            return t;
        }

        void ToggleMenu()
        {
            if (_menu.Visibility == Visibility.Visible) { CloseMenu(); return; }
            _menuBack.Opacity = _canBack ? 1 : 0.4;
            _menuForward.Opacity = _canForward ? 1 : 0.4;
            _menu.Visibility = Visibility.Visible;
        }

        void CloseMenu() { _menu.Visibility = Visibility.Collapsed; }

        void UpdateHint() { _urlHint.Visibility = _url.Text.Length == 0 ? Visibility.Visible : Visibility.Collapsed; }

        void ShowAddress()
        {
            // As IE: the page's domain when not editing
            Uri u;
            _url.Text = Uri.TryCreate(_pageUrl, UriKind.Absolute, out u) && (u.Scheme == "http" || u.Scheme == "https")
                ? u.Host.Replace("www.", "") : _pageUrl == "about:blank" ? "" : _pageUrl;
            UpdateHint();
        }

        void Url_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == System.Windows.Input.Key.Enter)
            {
                Go(ToUrl(_url.Text));
                _pageArea.Focus();
                Keyboard.Focus(this);
                e.Handled = true;
            }
            else if (e.Key == System.Windows.Input.Key.Escape)
            {
                Keyboard.Focus(this);
                e.Handled = true;
            }
        }

        static string ToUrl(string text)
        {
            text = text.Trim();
            if (text.Length == 0) return text;
            if (text.Contains("://") || text.StartsWith("about:", StringComparison.OrdinalIgnoreCase)) return text;
            if (!text.Contains(" ") && text.Contains(".")) return "https://" + text;
            return "https://html.duckduckgo.com/html/?q=" + Uri.EscapeDataString(text);
        }

        void Go(string url)
        {
            if (_engine == null || url.Length == 0) return;
            _engine.SendNavigate(url);
            _loading = true;
            UpdateLoading(0);
        }

        void Command(byte c) { if (_engine != null) _engine.SendCommand(c); }

        void UpdateLoading(double progress)
        {
            _progress.Value = progress;
            _progress.Visibility = _loading ? Visibility.Visible : Visibility.Collapsed;
            _refreshGlyph.Text = _loading ? "\uE10A" : "\uE149";
        }

        // ---- Tabs view ----

        void BuildTabsView()
        {
            _tabsView.Background = Chrome;
            var panel = new DockPanel { Margin = new Thickness(20, 14, 12, 0) };
            var head = new StackPanel();
            head.Children.Add(new TextBlock { Text = "RETROFOX", FontSize = 15, FontWeight = FontWeights.SemiBold, Foreground = Fore });
            head.Children.Add(new TextBlock { Text = "tabs", FontSize = 56, FontFamily = SegoeLight, Foreground = Fore, Margin = new Thickness(-2, -6, 0, 12) });
            DockPanel.SetDock(head, Dock.Top);
            panel.Children.Add(head);
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 8, 0, 16) };
            buttons.Children.Add(RoundButton("+", "new tab", NewTab));
            buttons.Children.Add(RoundButton("\u2190", "back to the page", CloseTabsView));
            DockPanel.SetDock(buttons, Dock.Bottom);
            panel.Children.Add(buttons);
            panel.Children.Add(new ScrollViewer { Content = _tabsGrid, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
            _tabsView.Children.Add(panel);
        }

        UIElement RoundButton(string glyph, string label, Action action)
        {
            var s = new StackPanel { Margin = new Thickness(18, 0, 18, 0), Cursor = Cursors.Hand, Background = Brushes.Transparent };
            var g = new Grid { Width = 48, Height = 48 };
            g.Children.Add(new Ellipse { Stroke = Fore, StrokeThickness = 2 });
            g.Children.Add(new TextBlock { Text = glyph, FontSize = 22, Foreground = Fore, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, -3, 0, 0) });
            s.Children.Add(g);
            s.Children.Add(new TextBlock { Text = label, FontSize = 12, Foreground = Fore, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 4, 0, 0) });
            s.MouseLeftButtonUp += (o, e) => action();
            return s;
        }

        void OpenTabsView()
        {
            CloseMenu();
            TakeThumbnail();
            ShowTabs();
            _tabsView.Visibility = Visibility.Visible;
        }

        void CloseTabsView() { _tabsView.Visibility = Visibility.Collapsed; }

        void TakeThumbnail()
        {
            if (_bitmap == null || _activeTab == 0) return;
            var thumb = new TransformedBitmap(_bitmap, new ScaleTransform(176.0 / Math.Max(1, _surfaceW), 176.0 / Math.Max(1, _surfaceW)));
            thumb.Freeze();
            _thumbs[_activeTab] = thumb;
        }

        void ShowTabs()
        {
            _tabsGrid.Children.Clear();
            foreach (var t in _tabs)
            {
                var tab = t;
                var tile = new StackPanel { Width = 176, Margin = new Thickness(0, 0, 16, 18), Cursor = Cursors.Hand, Background = Brushes.Transparent };
                var pic = new Grid { Width = 176, Height = 150, Background = new SolidColorBrush(Color.FromRgb(0xC4, 0xC4, 0xC4)), ClipToBounds = true };
                BitmapSource img;
                if (_thumbs.TryGetValue(t.Id, out img))
                    pic.Children.Add(new Image { Source = img, Stretch = Stretch.None, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top });
                if (t.Id == _activeTab)
                    pic.Children.Add(new Border { BorderThickness = new Thickness(3), BorderBrush = Accent });
                // The round x: 34 px to see, a 56 px square to hit
                var close = new Grid { Width = 56, Height = 56, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top, Background = Brushes.Transparent, ToolTip = "Close the tab" };
                var circle = new Grid { Width = 34, Height = 34, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 4, 4, 0) };
                circle.Children.Add(new Ellipse { Fill = Brushes.White, Stroke = Brushes.Black, StrokeThickness = 2 });
                circle.Children.Add(new TextBlock { Text = "\uE10A", FontFamily = Symbols, FontSize = 14, Foreground = Brushes.Black, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center });
                close.Children.Add(circle);
                close.MouseLeftButtonUp += (s, e) =>
                {
                    e.Handled = true;      // closing, not opening: the tabs view stays
                    if (_engine != null) _engine.SendTab(Engine.TabClose, tab.Id, null);
                    _tabs.RemoveAll(x => x.Id == tab.Id);
                    _thumbs.Remove(tab.Id);
                    ShowTabs();
                };
                pic.Children.Add(close);
                tile.Children.Add(pic);
                tile.Children.Add(new TextBlock { Text = string.IsNullOrEmpty(t.Title) ? "blank page" : t.Title, FontSize = 17, FontWeight = FontWeights.SemiBold, Foreground = Fore, TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 4, 0, 0) });
                tile.MouseLeftButtonUp += (s, e) =>
                {
                    if (e.Handled) return;
                    CloseTabsView();
                    if (tab.Id != _activeTab && _engine != null) _engine.SendTab(Engine.TabSelect, tab.Id, null);
                };
                _tabsGrid.Children.Add(tile);
            }
        }

        void NewTab()
        {
            CloseTabsView();
            if (_engine != null) _engine.SendTab(Engine.TabNew, 0, Home);
            _url.Focus();
        }

        // ---- The engine: start, connect, frames, state ----

        string EngineExe()
        {
            string here = AppDomain.CurrentDomain.BaseDirectory;
            foreach (var p in new[] { System.IO.Path.Combine(here, "engine", "rfxengine.exe"), @"C:\rfx\obj-desktop\dist\bin\rfxengine.exe" })
                if (File.Exists(p)) return p;
            return null;
        }

        void StartEngine()
        {
            string exe = EngineExe();
            if (exe == null) { _status.Text = "Can't find the Retrofox engine (engine\\rfxengine.exe next to this program)."; return; }
            var l = new TcpListener(IPAddress.Loopback, 0);
            l.Start();
            _port = ((IPEndPoint)l.LocalEndpoint).Port;
            l.Stop();
            string profile = System.IO.Path.Combine(DataDir, "profile");
            var psi = new ProcessStartInfo(exe,
                "--no-watchdog --port " + _port + " --profile \"" + profile + "\" --log \"" + _logPath + "\" --home " + Home)
            {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = System.IO.Path.GetDirectoryName(exe)
            };
            try { _engineProcess = Process.Start(psi); }
            catch (Exception e) { _status.Text = "Couldn't start the engine: " + e.Message; return; }
            var t = new Thread(ConnectLoop) { IsBackground = true };
            t.Start();
        }

        void ConnectLoop()
        {
            int w = 0, h = 0, scale = 100;
            Dispatcher.Invoke(new Action(() => { SurfaceSize(out w, out h); scale = (int)Math.Round(_dpi * 100); }));
            for (int i = 0; i < 100; i++)
            {
                if (_engineProcess == null || _engineProcess.HasExited) break;
                try
                {
                    var e = Engine.Connect(_port, w, h, scale);
                    e.FrameReceived += Engine_Frame;
                    e.StateReceived += json => Dispatcher.BeginInvoke(new Action(() => ApplyState(json)));
                    e.TabsReceived += json => Dispatcher.BeginInvoke(new Action(() => ApplyTabs(json)));
                    e.Closed += () => Dispatcher.BeginInvoke(new Action(() => EngineLost()));
                    Dispatcher.BeginInvoke(new Action(() => { _engine = e; _status.Visibility = Visibility.Collapsed; CreateSurface(w, h); }));
                    return;
                }
                catch (Exception) { Thread.Sleep(200); }
            }
            Dispatcher.BeginInvoke(new Action(() => _status.Text = "The engine didn't start. Its log: " + _logPath));
        }

        void EngineLost()
        {
            if (_engine == null) return;
            _engine = null;
            _status.Text = "The engine stopped (it may have crashed on that page). Its log: " + _logPath + "\n\nStarting it again...";
            _status.Visibility = Visibility.Visible;
            StopEngine();
            StartEngine();
        }

        void StopEngine()
        {
            if (_engine != null) { _engine.Dispose(); _engine = null; }
            try { if (_engineProcess != null && !_engineProcess.HasExited) _engineProcess.Kill(); } catch (Exception) { }
            _engineProcess = null;
        }

        void SurfaceSize(out int w, out int h)
        {
            w = Math.Max(64, (int)Math.Round(_pageArea.ActualWidth * _dpi));
            h = Math.Max(64, (int)Math.Round(_pageArea.ActualHeight * _dpi));
        }

        void CreateSurface(int w, int h)
        {
            _surfaceW = w; _surfaceH = h;
            _bitmap = new WriteableBitmap(w, h, 96 * _dpi, 96 * _dpi, PixelFormats.Pbgra32, null);
            _page.Source = _bitmap;
        }

        void ApplySize()
        {
            if (_engine == null) return;
            int w, h;
            SurfaceSize(out w, out h);
            if (w == _surfaceW && h == _surfaceH) return;
            CreateSurface(w, h);
            _engine.SendResize(w, h);
        }

        void Engine_Frame(uint serial, int sw, int sh, int x, int y, int w, int h, byte[] px)
        {
            Dispatcher.BeginInvoke(new Action(() =>
            {
                var e = _engine;
                if (_bitmap != null && sw == _surfaceW && sh == _surfaceH && x + w <= sw && y + h <= sh && w > 0 && h > 0)
                    _bitmap.WritePixels(new Int32Rect(0, 0, w, h), px, w * 4, x, y);
                if (e != null) e.SendAck(serial);
            }), DispatcherPriority.Render);
        }

        static Dictionary<string, object> Json(string json)
        {
            try { return new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(json); }
            catch (Exception) { return null; }
        }

        static string Str(Dictionary<string, object> o, string k) { object v; return o.TryGetValue(k, out v) && v != null ? v.ToString() : ""; }
        static bool Bool(Dictionary<string, object> o, string k) { object v; return o.TryGetValue(k, out v) && v is bool && (bool)v; }

        void ApplyState(string json)
        {
            var o = Json(json);
            if (o == null) return;
            _pageUrl = Str(o, "url");
            _pageTitle = Str(o, "title");
            _canBack = Bool(o, "back");
            _canForward = Bool(o, "forward");
            _loading = Bool(o, "loading");
            double progress;
            double.TryParse(Str(o, "progress"), out progress);
            UpdateLoading(progress);
            Title = (_pageTitle.Length > 0 ? _pageTitle + " - " : "") + "Retrofox";
            if (!_url.IsKeyboardFocusWithin) ShowAddress();
        }

        void ApplyTabs(string json)
        {
            var o = Json(json);
            if (o == null) return;
            uint active;
            uint.TryParse(Str(o, "active"), out active);
            if (active != _activeTab) TakeThumbnail();
            _activeTab = active;
            _tabs.Clear();
            object arr;
            if (o.TryGetValue("tabs", out arr) && arr is System.Collections.ArrayList)
                foreach (var item in (System.Collections.ArrayList)arr)
                {
                    var t = item as Dictionary<string, object>;
                    if (t == null) continue;
                    uint id;
                    uint.TryParse(Str(t, "id"), out id);
                    _tabs.Add(new TabInfo { Id = id, Title = Str(t, "title"), Url = Str(t, "url") });
                }
            _tabCount.Text = _tabs.Count > 1 ? _tabs.Count.ToString() : "";
            if (_tabsView.Visibility == Visibility.Visible) ShowTabs();
        }

        // ---- Input ----

        Point Surface(MouseEventArgs e)
        {
            var p = e.GetPosition(_pageArea);
            return new Point(p.X * _dpi, p.Y * _dpi);
        }

        void Page_MouseDown(object sender, MouseButtonEventArgs e)
        {
            CloseMenu();
            if (_engine == null || e.ChangedButton != MouseButton.Left) return;
            Keyboard.Focus(this);
            _pageArea.CaptureMouse();
            _mouseDown = true;
            var p = Surface(e);
            _engine.SendPointer(Engine.PointerDown, (int)p.X, (int)p.Y);
        }

        void Page_MouseMove(object sender, MouseEventArgs e)
        {
            if (!_mouseDown || _engine == null) return;
            var p = Surface(e);
            _engine.SendPointer(Engine.PointerMove, (int)p.X, (int)p.Y);
        }

        void Page_MouseUp(object sender, MouseButtonEventArgs e)
        {
            if (!_mouseDown || e.ChangedButton != MouseButton.Left) return;
            _mouseDown = false;
            _pageArea.ReleaseMouseCapture();
            if (_engine == null) return;
            var p = Surface(e);
            _engine.SendPointer(Engine.PointerUp, (int)p.X, (int)p.Y);
        }

        void Page_MouseWheel(object sender, MouseWheelEventArgs e)
        {
            if (_engine == null) return;
            int dy = (int)Math.Round(-e.Delta / 120.0 * 100 * _dpi);
            if ((Keyboard.Modifiers & ModifierKeys.Shift) != 0) _engine.SendScroll(dy, 0);
            else _engine.SendScroll(0, dy);
            e.Handled = true;
        }

        void Window_PreviewKeyDown(object sender, KeyEventArgs e)
        {
            var key = e.Key == System.Windows.Input.Key.System ? e.SystemKey : e.Key;
            var mods = Keyboard.Modifiers;
            // Shortcuts, wherever the focus is
            if (key == System.Windows.Input.Key.F5 || (mods == ModifierKeys.Control && key == System.Windows.Input.Key.R)) { Command(Engine.CmdReload); e.Handled = true; return; }
            if (mods == ModifierKeys.Alt && key == System.Windows.Input.Key.Left) { Command(Engine.CmdBack); e.Handled = true; return; }
            if (mods == ModifierKeys.Alt && key == System.Windows.Input.Key.Right) { Command(Engine.CmdForward); e.Handled = true; return; }
            if ((mods == ModifierKeys.Control && key == System.Windows.Input.Key.L) || key == System.Windows.Input.Key.F6) { _url.Focus(); e.Handled = true; return; }
            if (mods == ModifierKeys.Control && key == System.Windows.Input.Key.T) { NewTab(); e.Handled = true; return; }
            if (mods == ModifierKeys.Control && key == System.Windows.Input.Key.W && _engine != null && _activeTab != 0) { _engine.SendTab(Engine.TabClose, _activeTab, null); e.Handled = true; return; }
            if (key == System.Windows.Input.Key.Escape && _tabsView.Visibility == Visibility.Visible) { CloseTabsView(); e.Handled = true; return; }
            // Everything else goes to the page, unless the address field has the keyboard
            if (_url.IsKeyboardFocusWithin || _engine == null || _tabsView.Visibility == Visibility.Visible) return;
            switch (key)
            {
                case System.Windows.Input.Key.Back: case System.Windows.Input.Key.Enter: case System.Windows.Input.Key.Tab:
                case System.Windows.Input.Key.Delete: case System.Windows.Input.Key.Left: case System.Windows.Input.Key.Right:
                case System.Windows.Input.Key.Up: case System.Windows.Input.Key.Down: case System.Windows.Input.Key.Home:
                case System.Windows.Input.Key.End: case System.Windows.Input.Key.PageUp: case System.Windows.Input.Key.PageDown:
                case System.Windows.Input.Key.Escape: case System.Windows.Input.Key.Space:
                    if (key == System.Windows.Input.Key.Space) return;   // typed as text
                    _engine.SendKey(KeyInterop.VirtualKeyFromKey(key));
                    e.Handled = true;
                    break;
            }
        }

        void Window_TextInput(object sender, TextCompositionEventArgs e)
        {
            if (_url.IsKeyboardFocusWithin || _engine == null || string.IsNullOrEmpty(e.Text)) return;
            if (e.Text == "\r" || e.Text == "\b" || e.Text == "\t" || e.Text == "\x1b") return;
            _engine.SendText(e.Text);
            e.Handled = true;
        }
    }

    static class Program
    {
        [STAThread]
        static void Main()
        {
            var app = new Application();
            app.Run(new MainWindow());
        }
    }
}
