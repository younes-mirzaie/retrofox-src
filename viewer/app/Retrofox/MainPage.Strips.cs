using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Media.Imaging;

namespace Retrofox
{
    // Smooth panning, the way Firefox's async pan/zoom does it:
    //  - Strips: the engine draws the page just above and below the screen while it's idle
    //    (STRIP) and the app keeps them under the page picture, placed by their page position, so
    //    a pan or flick shows the page right away instead of blank while the next frame is drawn.
    //  - Fixed parts: the engine says where the page's position: fixed boxes are (FIXED), and the
    //    app shows copies of them from the last frame at those places, above the moving picture,
    //    so headers and bars stay put instead of moving with the page and snapping back.
    public sealed partial class MainPage
    {
        sealed class Strip
        {
            public int PageX, PageY, Width, Height;
            public WriteableBitmap Bitmap;
        }

        Strip _stripAbove, _stripBelow;
        int[] _fixedRects = new int[0];
        uint _fixedSerial;
        readonly List<Image> _fixedImages = new List<Image>();

        void HookStrips(EngineConnection engine)
        {
            engine.StripReceived += s => RunOnUi(() => ApplyStrip(s));
            engine.FixedReceived += f => RunOnUi(() => ApplyFixed(f));
        }

        // UI thread
        void ApplyStrip(StripMessage m)
        {
            var s = _shownScroll;
            if (s == null || m.Width <= 0 || m.Height <= 0)
                return;
            var strip = new Strip { PageX = m.PageX, PageY = m.PageY, Width = m.Width, Height = m.Height };
            strip.Bitmap = new WriteableBitmap(m.Width, m.Height);
            using (Stream st = strip.Bitmap.PixelBuffer.AsStream())
                st.Write(m.Pixels, 0, m.Pixels.Length);
            strip.Bitmap.Invalidate();
            // Below the page on screen, or above it
            if (m.PageY >= s.Y)
                _stripBelow = strip;
            else
                _stripAbove = strip;
            ShowStrip(StripImageA, _stripAbove);
            ShowStrip(StripImageB, _stripBelow);
            UpdatePageShift();
        }

        void ShowStrip(Image image, Strip strip)
        {
            if (strip == null)
            {
                image.Source = null;
                image.Visibility = Visibility.Collapsed;
                return;
            }
            image.Source = strip.Bitmap;
            image.Width = strip.Width / _scale;
            image.Height = strip.Height / _scale;
            image.Visibility = Visibility.Visible;
        }

        // A new page or tab: the strips belong to the old one
        void ClearStrips()
        {
            _stripAbove = _stripBelow = null;
            ShowStrip(StripImageA, null);
            ShowStrip(StripImageB, null);
            _fixedRects = new int[0];
            UpdateFixedLayer();
        }

        // Called from UpdatePageShift: each strip sits where it belongs in the page, relative to
        // the frame on screen (whose scroll position is s) moved by the pan in progress (dx, dy)
        void PlaceStrips(ScrollInfo s, double dx, double dy)
        {
            PlaceStrip(StripShiftA, _stripAbove, s, dx, dy);
            PlaceStrip(StripShiftB, _stripBelow, s, dx, dy);
        }

        void PlaceStrip(Windows.UI.Xaml.Media.TranslateTransform shift, Strip strip, ScrollInfo s, double dx, double dy)
        {
            if (strip == null || s == null)
                return;
            shift.X = (strip.PageX - s.X - dx) / _scale;
            shift.Y = (strip.PageY - s.Y - dy) / _scale;
        }

        // UI thread
        void ApplyFixed(FixedMessage f)
        {
            _fixedRects = f.Rects ?? new int[0];
            _fixedSerial = f.Serial;
            UpdateFixedLayer();
        }

        // After each blit (UI thread): copies of the fixed boxes from the frame on screen, in place
        void UpdateFixedLayer()
        {
            int n = _fixedRects.Length / 4;
            // Only while the page is actually moved, and only for boxes of the frame on screen
            bool show = n > 0 && _pixels != null && _fixedSerial <= _shownSerial;
            while (_fixedImages.Count < n)
            {
                var img = new Image { Stretch = Windows.UI.Xaml.Media.Stretch.Fill, IsHitTestVisible = false };
                _fixedImages.Add(img);
                FixedLayer.Children.Add(img);
            }
            for (int i = 0; i < _fixedImages.Count; i++)
            {
                var img = _fixedImages[i];
                img.Tag = null;   // usable copy of a box of the frame on screen (set below)
                if (!show || i >= n)
                {
                    img.Visibility = Visibility.Collapsed;
                    continue;
                }
                int x = Math.Max(0, _fixedRects[i * 4]), y = Math.Max(0, _fixedRects[i * 4 + 1]);
                int w = Math.Min(_surfaceW - x, _fixedRects[i * 4 + 2]), h = Math.Min(_surfaceH - y, _fixedRects[i * 4 + 3]);
                // A box covering most of the screen (a full-page overlay) is left to the page
                if (w <= 0 || h <= 0 || (long)w * h > (long)_surfaceW * _surfaceH / 2)
                {
                    img.Visibility = Visibility.Collapsed;
                    continue;
                }
                var bmp = img.Source as WriteableBitmap;
                if (bmp == null || bmp.PixelWidth != w || bmp.PixelHeight != h)
                {
                    bmp = new WriteableBitmap(w, h);
                    img.Source = bmp;
                }
                var row = new byte[w * 4];
                lock (_pixelLock)
                {
                    using (Stream st = bmp.PixelBuffer.AsStream())
                        for (int r = 0; r < h; r++)
                        {
                            Buffer.BlockCopy(_pixels, ((y + r) * _surfaceW + x) * 4, row, 0, row.Length);
                            st.Write(row, 0, row.Length);
                        }
                }
                bmp.Invalidate();
                img.Width = w / _scale;
                img.Height = h / _scale;
                Canvas.SetLeft(img, x / _scale);
                Canvas.SetTop(img, y / _scale);
                img.Tag = true;
                img.Visibility = PageShift.X != 0 || PageShift.Y != 0 ? Visibility.Visible : Visibility.Collapsed;
            }
        }

        // While a pan moves the picture, the fixed copies show; when it's still, the page's own
        void UpdateFixedVisibility()
        {
            bool moved = PageShift.X != 0 || PageShift.Y != 0;
            int n = _fixedRects.Length / 4;
            for (int i = 0; i < _fixedImages.Count; i++)
            {
                var img = _fixedImages[i];
                // Not a copy left from an earlier page or a box skipped as too big
                bool usable = i < n && img.Tag != null && _fixedSerial <= _shownSerial;
                img.Visibility = moved && usable ? Visibility.Visible : Visibility.Collapsed;
            }
        }
    }
}
