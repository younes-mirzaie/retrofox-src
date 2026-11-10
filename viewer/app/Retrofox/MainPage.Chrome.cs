using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.ApplicationModel.DataTransfer;
using Windows.Data.Json;
using Windows.Storage;
using Windows.System;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media.Animation;

namespace Retrofox
{
    // A page in favorites, recent or tabs
    public sealed class PageEntry
    {
        public string Title { get; set; }
        public string Url { get; set; }
        public string Detail { get { return Url; } }
        public uint TabId { get; set; }             // tabs only
        public bool IsNewTab { get; set; }          // the "new tab" entry
        public bool IsActiveTab { get; set; }
        public Visibility CloseVisibility { get { return TabId != 0 ? Visibility.Visible : Visibility.Collapsed; } }
        public Windows.UI.Xaml.Media.ImageSource Thumbnail { get; set; }   // tabs: what the page looked like
        public Visibility ActiveVisibility { get { return IsActiveTab ? Visibility.Visible : Visibility.Collapsed; } }
        public Windows.UI.Xaml.Media.Brush TitleBrush
        {
            get
            {
                return (Windows.UI.Xaml.Media.Brush)Application.Current.Resources[
                    IsActiveTab || IsNewTab ? "PhoneAccentBrush" : "PhoneForegroundBrush"];
            }
        }
    }

    // The browser chrome, laid out like Internet Explorer on Windows Phone 8.1: address bar at the
    // bottom (the domain until tapped, refresh/stop inside), tabs, the "..." menu, full-screen
    // lists, find on page, and the bar sliding away while scrolling down.
    public sealed partial class MainPage
    {
        const string HomePageSetting = "homePage";
        const string DefaultHomePage = "https://www.google.com/";
        const int MaxRecent = 100;

        string _pageTitle = "";
        bool _barHidden;
        double _barScroll;              // scrolling since the bar last changed, surface pixels
        const double BarHideScroll = 48;
        readonly List<PageEntry> _favorites = new List<PageEntry>();
        readonly List<PageEntry> _recent = new List<PageEntry>();
        string _sheet;                  // which list the sheet shows, or null
        readonly List<PageEntry> _tabs = new List<PageEntry>();   // from the engine (TABS)
        uint _activeTab;
        bool _wasLoading;

        string HomePage
        {
            get
            {
                object v;
                if (ApplicationData.Current.LocalSettings.Values.TryGetValue(HomePageSetting, out v) &&
                    !string.IsNullOrWhiteSpace(v as string))
                    return (string)v;
                return DefaultHomePage;
            }
        }

        // ---- Address bar ----

        // What the address bar shows while not being edited: the site's domain, as IE does
        static string DisplayUrl(string url)
        {
            Uri u;
            if (string.IsNullOrEmpty(url) || url == "about:blank")
                return "";
            if (!Uri.TryCreate(url, UriKind.Absolute, out u) || string.IsNullOrEmpty(u.Host))
                return url;
            string host = u.Host;
            return host.StartsWith("www.", StringComparison.OrdinalIgnoreCase) ? host.Substring(4) : host;
        }

        void ShowPageInBar()
        {
            if (UrlBox.FocusState == FocusState.Unfocused)
            {
                UrlBox.Text = DisplayUrl(_pageUrl);
                // Refresh (or stop) is there once there's a page, as IE's
                RefreshButton.Visibility = UrlBox.Text.Length > 0 || _loading ? Visibility.Visible : Visibility.Collapsed;
            }
        }

        void UrlBox_GotFocus(object sender, RoutedEventArgs e)
        {
            CloseMenu();
            UrlBox.Text = _pageUrl == "about:blank" ? "" : _pageUrl;
            UrlBox.SelectAll();
            RefreshButton.Visibility = Visibility.Collapsed;
        }

        void UrlBox_LostFocus(object sender, RoutedEventArgs e)
        {
            ShowPageInBar();
        }

        // ---- Tilt: what you touch leans in a little, as everything on the phone does ----

        void Tilt_Pressed(object sender, PointerRoutedEventArgs e)
        {
            var el = sender as UIElement;
            if (el == null)
                return;
            el.RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5);
            el.RenderTransform = new Windows.UI.Xaml.Media.ScaleTransform { ScaleX = 0.94, ScaleY = 0.94 };
        }

        void Tilt_Released(object sender, PointerRoutedEventArgs e)
        {
            var el = sender as UIElement;
            if (el != null)
                el.RenderTransform = null;
        }

        void RefreshButton_Tapped(object sender, TappedRoutedEventArgs e)
        {
            SendCommand(_loading ? EngineConnection.CmdStop : EngineConnection.CmdReload);
            e.Handled = true;
        }

        void UpdateLoadingUi(bool loading, double progress)
        {
            LoadProgress.Value = progress;
            LoadProgress.Visibility = loading ? Visibility.Visible : Visibility.Collapsed;
            RefreshGlyph.Text = loading ? "" : "";   // stop while loading, else refresh
            if (loading && !_wasLoading)
                ShowBar();
            if (!loading && _wasLoading)
                AddRecent(_pageUrl, _pageTitle);
            _wasLoading = loading;
        }

        // ---- The bar slides away while scrolling down, and back on scrolling up ----

        void BarScrolled(double dy)
        {
            if (MenuPanel.Visibility == Visibility.Visible || FindRow.Visibility == Visibility.Visible ||
                UrlBox.FocusState != FocusState.Unfocused)
                return;
            if ((dy > 0) != (_barScroll > 0))
                _barScroll = 0;         // changed direction: start counting again
            _barScroll += dy;
            if (_barScroll > BarHideScroll && !_barHidden)
                HideBar();
            else if (_barScroll < -BarHideScroll && _barHidden)
                ShowBar();
        }

        void ShowBar() { SlideBar(false); }
        void HideBar() { SlideBar(true); }

        void SlideBar(bool hide)
        {
            if (_barHidden == hide)
                return;
            _barHidden = hide;
            _barScroll = 0;
            var a = new DoubleAnimation
            {
                To = hide ? AddressRow.ActualHeight : 0,
                Duration = TimeSpan.FromMilliseconds(200),
                EasingFunction = new ExponentialEase { EasingMode = EasingMode.EaseOut, Exponent = 4 }
            };
            AnimateBarShift(a);
        }

        Storyboard _barStoryboard;

        // Stops a running bar animation where it is (Stop alone would jump back to the start)
        void HoldBarShift()
        {
            if (_barStoryboard == null)
                return;
            double y = BarShift.Y;
            _barStoryboard.Stop();
            _barStoryboard = null;
            BarShift.Y = y;
        }

        void AnimateBarShift(DoubleAnimation a, Action done = null)
        {
            HoldBarShift();
            Storyboard.SetTarget(a, BarShift);
            Storyboard.SetTargetProperty(a, "Y");
            var sb = new Storyboard();
            sb.Children.Add(a);
            if (done != null)
                sb.Completed += (s, e) => done();
            _barStoryboard = sb;
            sb.Begin();
        }

        // ---- Dragging the bar up opens the "..." menu, as the app bar does ----
        // The menu opens under the address row and the whole bar starts pushed down by the menu's
        // height, so nothing moves until the finger does; then the bar follows the finger and
        // snaps open or closed on release. Dragging the open menu down closes it the same way.

        double _menuDragBase, _menuDragHeight;
        bool _menuDragging;

        void Bar_ManipulationStarted(object sender, ManipulationStartedRoutedEventArgs e)
        {
            if (FindRow.Visibility == Visibility.Visible)
                return;
            _menuDragging = true;
            HoldBarShift();
            _barHidden = false;
            bool wasOpen = MenuPanel.Visibility == Visibility.Visible;
            if (!wasOpen)
                OpenMenu();
            BottomBar.UpdateLayout();
            _menuDragHeight = MenuPanel.ActualHeight;
            _menuDragBase = wasOpen ? 0 : _menuDragHeight;
            BarShift.Y = _menuDragBase;
        }

        void Bar_ManipulationDelta(object sender, ManipulationDeltaRoutedEventArgs e)
        {
            if (!_menuDragging)
                return;
            BarShift.Y = Math.Max(0, Math.Min(_menuDragHeight, _menuDragBase + e.Cumulative.Translation.Y));
            e.Handled = true;
        }

        void Bar_ManipulationCompleted(object sender, ManipulationCompletedRoutedEventArgs e)
        {
            if (!_menuDragging)
                return;
            _menuDragging = false;
            double v = e.Velocities.Linear.Y;   // px per ms, up is negative
            bool open = v < -0.3 || (v <= 0.3 && BarShift.Y < _menuDragHeight / 2);
            var a = new DoubleAnimation
            {
                To = open ? 0 : _menuDragHeight,
                Duration = TimeSpan.FromMilliseconds(180),
                EasingFunction = new ExponentialEase { EasingMode = EasingMode.EaseOut, Exponent = 4 }
            };
            AnimateBarShift(a, open ? (Action)null : () =>
            {
                // Closed: the menu goes and the bar is back in its place, without a jump
                CloseMenu();
                HoldBarShift();
                BarShift.Y = 0;
            });
            e.Handled = true;
        }

        // ---- "..." menu ----

        void MoreButton_Tapped(object sender, TappedRoutedEventArgs e)
        {
            if (MenuPanel.Visibility == Visibility.Visible)
                CloseMenu();
            else
                OpenMenu();
            e.Handled = true;
        }

        bool HasPage { get { return !string.IsNullOrEmpty(_pageUrl) && _pageUrl != "about:blank"; } }

        void OpenMenu()
        {
            ShowBar();
            MenuForward.Visibility = _canGoForward ? Visibility.Visible : Visibility.Collapsed;
            bool isFavorite = _favorites.Exists(f => f.Url == _pageUrl);
            MenuAddFavorite.Text = isFavorite ? "remove from favorites" : "add to favorites";
            // What needs a page is dimmed without one (disabled text: 40 percent)
            double pageItems = HasPage ? 1 : 0.4;
            MenuAddFavorite.Opacity = MenuFind.Opacity = MenuShare.Opacity = MenuPin.Opacity = pageItems;
            MenuPanel.Visibility = Visibility.Visible;
            MenuDismiss.Visibility = Visibility.Visible;
        }

        bool CloseMenu()
        {
            if (MenuPanel.Visibility != Visibility.Visible)
                return false;
            MenuPanel.Visibility = Visibility.Collapsed;
            MenuDismiss.Visibility = Visibility.Collapsed;
            return true;
        }

        void MenuDismiss_Tapped(object sender, TappedRoutedEventArgs e) { CloseMenu(); }

        void MenuForward_Tapped(object sender, TappedRoutedEventArgs e)
        {
            if (!_canGoForward)
                return;
            CloseMenu();
            SendCommand(EngineConnection.CmdForward);
        }

        void MenuRecent_Tapped(object sender, TappedRoutedEventArgs e)
        {
            CloseMenu();
            OpenSheet("recent");
        }

        void MenuFavorites_Tapped(object sender, TappedRoutedEventArgs e)
        {
            CloseMenu();
            OpenSheet("favorites");
        }

        void MenuAddFavorite_Tapped(object sender, TappedRoutedEventArgs e)
        {
            CloseMenu();
            if (string.IsNullOrEmpty(_pageUrl) || _pageUrl == "about:blank")
                return;
            int i = _favorites.FindIndex(f => f.Url == _pageUrl);
            if (i >= 0)
                _favorites.RemoveAt(i);
            else
                _favorites.Insert(0, new PageEntry { Title = TitleOrDomain(_pageTitle, _pageUrl), Url = _pageUrl });
            var t = SaveList("favorites.json", _favorites);
        }

        void TabsButton_Tapped(object sender, TappedRoutedEventArgs e)
        {
            CloseMenu();
            OpenTabsPage();
            e.Handled = true;
        }

        // ---- Tabs page: a pivot with a grid of thumbnails, + new and "..." close all ----

        // What each tab last looked like (taken when the tabs page opens or the tab is left)
        readonly Dictionary<uint, Windows.UI.Xaml.Media.Imaging.WriteableBitmap> _thumbs =
            new Dictionary<uint, Windows.UI.Xaml.Media.Imaging.WriteableBitmap>();
        const int ThumbW = 173, ThumbH = 148;   // physical pixels (the tile is 144x123)

        void TakeThumbnail(uint tabId)
        {
            if (tabId == 0 || _pixels == null || _surfaceW == 0)
                return;
            var bmp = new Windows.UI.Xaml.Media.Imaging.WriteableBitmap(ThumbW, ThumbH);
            var dst = new byte[ThumbW * ThumbH * 4];
            lock (_pixelLock)
            {
                // The top of the page, scaled to the tile's width (nearest pixel)
                double step = (double)_surfaceW / ThumbW;
                for (int y = 0; y < ThumbH; y++)
                {
                    int sy = Math.Min(_surfaceH - 1, (int)(y * step));
                    for (int x = 0; x < ThumbW; x++)
                    {
                        int sx = Math.Min(_surfaceW - 1, (int)(x * step));
                        System.Buffer.BlockCopy(_pixels, (sy * _surfaceW + sx) * 4, dst, (y * ThumbW + x) * 4, 4);
                    }
                }
            }
            using (var s = System.Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions.AsStream(bmp.PixelBuffer))
                s.Write(dst, 0, dst.Length);
            bmp.Invalidate();
            _thumbs[tabId] = bmp;
        }

        void OpenTabsPage()
        {
            TakeThumbnail(_activeTab);
            ShowTabs();
            TabsMenu.Visibility = Visibility.Collapsed;
            TabsPivot.SelectedIndex = 0;
            TabsPage.Visibility = Visibility.Visible;
            PageArea.Visibility = Visibility.Collapsed;   // nothing of the page under the status bar
        }

        void ShowTabs()
        {
            foreach (var t in _tabs)
            {
                Windows.UI.Xaml.Media.Imaging.WriteableBitmap thumb;
                t.Thumbnail = _thumbs.TryGetValue(t.TabId, out thumb) ? thumb : null;
            }
            TabsGrid.ItemsSource = new List<PageEntry>(_tabs);
        }

        bool CloseTabsPage()
        {
            if (TabsPage.Visibility != Visibility.Visible)
                return false;
            if (TabsMenu.Visibility == Visibility.Visible)
            {
                TabsMenu.Visibility = Visibility.Collapsed;
                return true;
            }
            TabsPage.Visibility = Visibility.Collapsed;
            PageArea.Visibility = Visibility.Visible;
            FocusSink.Focus(FocusState.Programmatic);
            return true;
        }

        // A press on a tab's round x: the grid still raises its click for the item underneath,
        // which would open that tab and leave the tabs view. The press marks it so that click is
        // ignored.
        DateTime _tabClosePressed;

        void TabClose_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            _tabClosePressed = DateTime.UtcNow;
        }

        void TabsGrid_ItemClick(object sender, ItemClickEventArgs e)
        {
            if ((DateTime.UtcNow - _tabClosePressed).TotalMilliseconds < 1500)
                return;     // the x was pressed: closing, not opening
            var p = e.ClickedItem as PageEntry;
            CloseTabsPage();
            if (p != null && p.TabId != _activeTab && _engine != null)
            {
                var t = _engine.SendTab(EngineConnection.TabSelect, p.TabId);
            }
        }

        void NewTab_Tapped(object sender, TappedRoutedEventArgs e)
        {
            CloseTabsPage();
            if (_engine != null)
            {
                var t = _engine.SendTab(EngineConnection.TabNew, 0, HomePage);
            }
            UrlBox.Focus(FocusState.Programmatic);    // as IE: type where to go
        }

        void TabsMore_Tapped(object sender, TappedRoutedEventArgs e)
        {
            TabsMenu.Visibility = TabsMenu.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible;
        }

        void CloseAllTabs_Tapped(object sender, TappedRoutedEventArgs e)
        {
            if (_engine != null)
            {
                foreach (var tab in new List<PageEntry>(_tabs))
                {
                    var t = _engine.SendTab(EngineConnection.TabClose, tab.TabId);
                }
            }
            _thumbs.Clear();
            TabsMenu.Visibility = Visibility.Collapsed;
            CloseTabsPage();
        }

        // The round x on a tab's thumbnail
        void TabClose_Tapped(object sender, TappedRoutedEventArgs e)
        {
            e.Handled = true;    // not a tap on the tab itself
            var p = (sender as FrameworkElement) != null ? ((FrameworkElement)sender).DataContext as PageEntry : null;
            if (p == null || p.TabId == 0 || _engine == null)
                return;
            var t = _engine.SendTab(EngineConnection.TabClose, p.TabId);
            // Show it gone right away; the engine's TABS follows. The tabs view stays open: which
            // tab to go to next is the user's choice
            _tabs.RemoveAll(x => x.TabId == p.TabId);
            _thumbs.Remove(p.TabId);
            ShowTabs();
        }

        // ---- Pin to start: a tile that opens this page in Retrofox ----

        async void MenuPin_Tapped(object sender, TappedRoutedEventArgs e)
        {
            CloseMenu();
            if (!HasPage)
                return;
            string id = "page" + ((uint)_pageUrl.GetHashCode()).ToString();
            var tile = new Windows.UI.StartScreen.SecondaryTile(id, TitleOrDomain(_pageTitle, _pageUrl),
                _pageUrl, new Uri("ms-appx:///Assets/Logo.png"), Windows.UI.StartScreen.TileSize.Square150x150);
            tile.VisualElements.ShowNameOnSquare150x150Logo = true;
            try
            {
                await tile.RequestCreateAsync();
            }
            catch (Exception)
            {
                // the phone said no: nothing to do
            }
        }

        void MenuShare_Tapped(object sender, TappedRoutedEventArgs e)
        {
            CloseMenu();
            if (string.IsNullOrEmpty(_pageUrl) || _pageUrl == "about:blank")
                return;
            DataTransferManager.ShowShareUI();
        }

        void Share_DataRequested(DataTransferManager sender, DataRequestedEventArgs e)
        {
            Uri u;
            if (!Uri.TryCreate(_pageUrl, UriKind.Absolute, out u))
                return;
            e.Request.Data.Properties.Title = TitleOrDomain(_pageTitle, _pageUrl);
            e.Request.Data.SetWebLink(u);
        }

        void MenuSettings_Tapped(object sender, TappedRoutedEventArgs e)
        {
            CloseMenu();
            OpenSheet("settings");
        }

        // ---- Find on page (the page's own window.find, through a javascript: URL) ----

        void MenuFind_Tapped(object sender, TappedRoutedEventArgs e)
        {
            CloseMenu();
            ShowBar();
            AddressRow.Visibility = Visibility.Collapsed;
            FindRow.Visibility = Visibility.Visible;
            FindBox.Text = "";
            FindBox.Focus(FocusState.Programmatic);
        }

        void FindBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key != VirtualKey.Enter)
                return;
            e.Handled = true;
            FindNext();
            FocusSink.Focus(FocusState.Programmatic);   // hides the keyboard to show the match
        }

        void FindNext_Tapped(object sender, TappedRoutedEventArgs e) { FindNext(); }

        void FindNext()
        {
            string text = FindBox.Text;
            if (text.Length == 0 || _engine == null)
                return;
            // find(text, caseSensitive, backwards, wrapAround)
            string js = "javascript:void(window.find(" + JsonValue.CreateStringValue(text).Stringify() +
                        ",false,false,true))";
            var t = _engine.SendNavigate(js);
        }

        void FindClose_Tapped(object sender, TappedRoutedEventArgs e) { CloseFind(); }

        bool CloseFind()
        {
            if (FindRow.Visibility != Visibility.Visible)
                return false;
            FindRow.Visibility = Visibility.Collapsed;
            AddressRow.Visibility = Visibility.Visible;
            FocusSink.Focus(FocusState.Programmatic);
            return true;
        }

        // ---- Full-screen lists: recent, favorites, tabs; and settings ----

        // What Retrofox is built on, and the terms it uses it under. Shown under settings.
        const string SourceUrl = "https://github.com/younes-mirzaie/retrofox-src";
        const string AboutLegal =
            "Retrofox runs on the UXP browser engine, which is based on Mozilla's Gecko. " +
            "That code is used under the Mozilla Public License 2.0 (MPL 2.0), which lets anyone " +
            "use, change and share it. Retrofox's own code is under the same license, and its " +
            "source code is available at " + SourceUrl + ".\n\n" +
            "The engine also includes other open-source parts, such as NSS, ICU, HarfBuzz and " +
            "FreeType, each under its own license. Tap \"license notices\" to read them all.\n\n" +
            "Retrofox is an independent project. It is not made, endorsed or supported by Mozilla. " +
            "Mozilla and Firefox are trademarks of the Mozilla Foundation.";

        void Licenses_Click(object sender, RoutedEventArgs e)
        {
            CloseSheet();
            Navigate("about:license");
        }

        void OpenSheet(string which)
        {
            _sheet = which;
            // App name above a lowercase page title; settings are titled as IE's ("SETTINGS",
            // then the app's name)
            SheetApp.Text = which == "settings" ? "SETTINGS" : "RETROFOX";
            SheetTitle.Text = which == "settings" ? "Retrofox" : which;
            SettingsPanel.Visibility = which == "settings" ? Visibility.Visible : Visibility.Collapsed;
            SheetList.Visibility = which == "settings" ? Visibility.Collapsed : Visibility.Visible;
            List<PageEntry> items = null;
            string empty = null;
            if (which == "recent")
            {
                items = _recent;
                empty = "Pages you visit show up here.";
            }
            else if (which == "favorites")
            {
                items = _favorites;
                empty = "Add pages with \"add to favorites\" in the ... menu. Hold one to remove it.";
            }
            else
            {
                HomeBox.Text = HomePage;
                var v = Package.Current.Id.Version;
                AboutText.Text = "Retrofox " + v.Major + "." + v.Minor + "." + v.Build +
                                 "\nA web browser for Windows Phone 8.1.\n\n" + AboutLegal;
            }
            SheetList.ItemsSource = items != null ? new List<PageEntry>(items) : null;
            SheetEmpty.Text = empty ?? "";
            SheetEmpty.Visibility = items != null && items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            Sheet.Visibility = Visibility.Visible;
            PageArea.Visibility = Visibility.Collapsed;   // nothing of the page under the status bar
        }

        bool CloseSheet()
        {
            if (Sheet.Visibility != Visibility.Visible)
                return false;
            Sheet.Visibility = Visibility.Collapsed;
            PageArea.Visibility = Visibility.Visible;
            _sheet = null;
            FocusSink.Focus(FocusState.Programmatic);
            return true;
        }

        void SheetList_ItemClick(object sender, ItemClickEventArgs e)
        {
            var p = e.ClickedItem as PageEntry;
            CloseSheet();
            if (p != null && p.Url != _pageUrl)
                Navigate(p.Url);
        }

        // UI thread: the engine's tab list (TABS)
        void ApplyTabs(string json)
        {
            JsonObject o;
            if (!JsonObject.TryParse(json, out o))
                return;
            uint active = (uint)o.GetNamedNumber("active", 0);
            if (active != _activeTab)
                TakeThumbnail(_activeTab);    // the tab being left, as it looked
            _activeTab = active;
            _tabs.Clear();
            foreach (var v in o.GetNamedArray("tabs", new JsonArray()))
            {
                if (v.ValueType != JsonValueType.Object)
                    continue;
                var t = v.GetObject();
                uint id = (uint)t.GetNamedNumber("id", 0);
                string url = t.GetNamedString("url", "");
                bool blank = url.Length == 0 || url == "about:blank";
                _tabs.Add(new PageEntry
                {
                    TabId = id,
                    Url = blank ? "" : url,
                    Title = blank ? "blank page" : TitleOrDomain(t.GetNamedString("title", ""), url),
                    IsActiveTab = id == _activeTab
                });
            }
            TabCount.Text = _tabs.Count > 1 ? _tabs.Count.ToString() : "";
            if (TabsPage.Visibility == Visibility.Visible)
                ShowTabs();
        }

        // Holding an entry in favorites or recent removes it
        void SheetItem_Holding(object sender, HoldingRoutedEventArgs e)
        {
            if (e.HoldingState != Windows.UI.Input.HoldingState.Started)
                return;
            var p = (sender as FrameworkElement) != null ? ((FrameworkElement)sender).DataContext as PageEntry : null;
            if (p == null)
                return;
            List<PageEntry> list = _sheet == "favorites" ? _favorites : _sheet == "recent" ? _recent : null;
            if (list == null)
                return;
            list.RemoveAll(x => x.Url == p.Url);
            var t = SaveList(_sheet == "favorites" ? "favorites.json" : "recent.json", list);
            OpenSheet(_sheet);
        }

        void HomeBox_LostFocus(object sender, RoutedEventArgs e)
        {
            string url = ToUrl(HomeBox.Text);
            if (url.Length == 0)
                url = DefaultHomePage;
            ApplicationData.Current.LocalSettings.Values[HomePageSetting] = url;
            HomeBox.Text = url;
        }

        void ClearHistory_Click(object sender, RoutedEventArgs e)
        {
            _recent.Clear();
            var t = SaveList("recent.json", _recent);
        }

        void ClearFavorites_Click(object sender, RoutedEventArgs e)
        {
            _favorites.Clear();
            var t = SaveList("favorites.json", _favorites);
        }

        // ---- Back key: closes what's open before going back a page ----

        bool CloseOverlays()
        {
            return CloseMenu() || CloseTabsPage() || CloseSheet() || CloseFind();
        }

        // ---- Favorites and recent, kept as JSON in the app's local folder ----

        static string TitleOrDomain(string title, string url)
        {
            return string.IsNullOrWhiteSpace(title) ? DisplayUrl(url) : title.Trim();
        }

        void AddRecent(string url, string title)
        {
            if (string.IsNullOrEmpty(url) || url == "about:blank" ||
                url.StartsWith("javascript:", StringComparison.OrdinalIgnoreCase))
                return;
            _recent.RemoveAll(x => x.Url == url);
            _recent.Insert(0, new PageEntry { Title = TitleOrDomain(title, url), Url = url });
            if (_recent.Count > MaxRecent)
                _recent.RemoveRange(MaxRecent, _recent.Count - MaxRecent);
            var t = SaveList("recent.json", _recent);
        }

        async Task LoadLists()
        {
            await LoadList("favorites.json", _favorites);
            await LoadList("recent.json", _recent);
        }

        static async Task LoadList(string file, List<PageEntry> into)
        {
            try
            {
                var f = await ApplicationData.Current.LocalFolder.GetFileAsync(file);
                string text = await FileIO.ReadTextAsync(f);
                JsonArray a;
                if (!JsonArray.TryParse(text, out a))
                    return;
                into.Clear();
                foreach (var v in a)
                {
                    if (v.ValueType != JsonValueType.Object)
                        continue;
                    var o = v.GetObject();
                    into.Add(new PageEntry { Title = o.GetNamedString("title", ""), Url = o.GetNamedString("url", "") });
                }
            }
            catch (Exception)
            {
                // none saved yet
            }
        }

        static async Task SaveList(string file, List<PageEntry> list)
        {
            var a = new JsonArray();
            foreach (var p in list)
            {
                var o = new JsonObject();
                o["title"] = JsonValue.CreateStringValue(p.Title ?? "");
                o["url"] = JsonValue.CreateStringValue(p.Url ?? "");
                a.Add(o);
            }
            string text = a.Stringify();
            // One save at a time: two writes of the same file at once fail
            await SaveLock.WaitAsync();
            try
            {
                var f = await ApplicationData.Current.LocalFolder.CreateFileAsync(file, CreationCollisionOption.ReplaceExisting);
                await FileIO.WriteTextAsync(f, text);
            }
            catch (Exception)
            {
                // storage full or similar: the next change saves again
            }
            finally
            {
                SaveLock.Release();
            }
        }

        static readonly System.Threading.SemaphoreSlim SaveLock = new System.Threading.SemaphoreSlim(1);
    }
}

#if DEBUG
namespace Retrofox
{
    // Debug builds only: walks through the chrome and saves a picture of each step to the app's
    // local folder (ui-*.png), to check the layout in the emulator, whose screen can't be captured
    public sealed partial class MainPage
    {
        async void DebugUiTour()
        {
            await Task.Delay(4000);
            ApplyState("{\"url\":\"https://en.wikipedia.org/wiki/Windows_Phone\",\"title\":\"Windows Phone - Wikipedia\"," +
                       "\"back\":true,\"forward\":true,\"loading\":true,\"progress\":40}");
            HideStatus();
            await DebugShot("ui-1-loading.png");
            ApplyState("{\"url\":\"https://en.wikipedia.org/wiki/Windows_Phone\",\"title\":\"Windows Phone - Wikipedia\"," +
                       "\"back\":true,\"forward\":true,\"loading\":false,\"progress\":100}");
            await DebugShot("ui-2-bar.png");
            OpenMenu();
            await DebugShot("ui-3-menu.png");
            CloseMenu();
            MenuFind_Tapped(null, null);
            await DebugShot("ui-4-find.png");
            CloseFind();
            OpenSheet("recent");
            await DebugShot("ui-5-recent.png");
            CloseSheet();
            ApplyTabs("{\"active\":2,\"tabs\":[{\"id\":1,\"title\":\"Google\",\"url\":\"https://www.google.com/\"}," +
                      "{\"id\":2,\"title\":\"Windows Phone - Wikipedia\",\"url\":\"https://en.wikipedia.org/wiki/Windows_Phone\"}," +
                      "{\"id\":3,\"title\":\"\",\"url\":\"about:blank\"}]}");
            OpenTabsPage();
            await DebugShot("ui-8-tabs.png");
            CloseTabsPage();
            OpenSheet("settings");
            await DebugShot("ui-6-settings.png");
            CloseSheet();
            HideBar();
            await Task.Delay(400);
            await DebugShot("ui-7-bar-hidden.png");
            ShowBar();
        }

        async Task DebugShot(string name)
        {
            await Task.Delay(700);
            try
            {
                var rtb = new Windows.UI.Xaml.Media.Imaging.RenderTargetBitmap();
                await rtb.RenderAsync(Root);
                var pixels = await rtb.GetPixelsAsync();
                var file = await ApplicationData.Current.LocalFolder.CreateFileAsync(name, CreationCollisionOption.ReplaceExisting);
                using (var stream = await file.OpenAsync(FileAccessMode.ReadWrite))
                {
                    var enc = await Windows.Graphics.Imaging.BitmapEncoder.CreateAsync(
                        Windows.Graphics.Imaging.BitmapEncoder.PngEncoderId, stream);
                    var info = Windows.Graphics.Display.DisplayInformation.GetForCurrentView();
                    enc.SetPixelData(Windows.Graphics.Imaging.BitmapPixelFormat.Bgra8,
                                     Windows.Graphics.Imaging.BitmapAlphaMode.Premultiplied,
                                     (uint)rtb.PixelWidth, (uint)rtb.PixelHeight, info.LogicalDpi, info.LogicalDpi,
                                     System.Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions.ToArray(pixels));
                    await enc.FlushAsync();
                }
            }
            catch (Exception ex)
            {
                var f = await ApplicationData.Current.LocalFolder.CreateFileAsync(name + ".txt", CreationCollisionOption.ReplaceExisting);
                await FileIO.WriteTextAsync(f, ex.ToString());
            }
        }
    }
}
#endif
