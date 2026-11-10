using System;
using System.Diagnostics;
using System.Threading.Tasks;
using Windows.Storage;

namespace Retrofox
{
    // Speed: how fast frames reach the screen while scrolling, and where the app's time goes.
    //  - Every 5 s with frames coming in, app-engine.log gets a line: frames in, blits to the screen
    //    (several frames can share one), and the ms spent copying frames and blitting them.
    //  - The bench: with a file bench.txt in the app's LocalState holding an address, the app
    //    opens it after connecting, flicks down and up a few times the way a finger does, and logs
    //    the totals. The file is renamed to bench.done so it runs once.
    public sealed partial class MainPage
    {
        readonly Stopwatch _statClock = Stopwatch.StartNew();
        double _statSince;
        int _statFrames, _statBlits;
        long _statFramePixels;
        double _statCopyMs, _statBlitMs;

        // Background thread, in the pixel lock
        void CountFrame(int w, int h, double copyMs)
        {
            _statFrames++;
            _statFramePixels += (long)w * h;
            _statCopyMs += copyMs;
        }

        // UI thread
        void CountBlit(double ms)
        {
            _statBlits++;
            _statBlitMs += ms;
            ReportSpeed(false);
        }

        double StatNow()
        {
            return _statClock.Elapsed.TotalMilliseconds;
        }

        void ReportSpeed(bool force)
        {
            double now = StatNow();
            double span = now - _statSince;
            if (!force && span < 5000)
                return;
            int frames, blits;
            long pixels;
            double copyMs, blitMs;
            lock (_pixelLock)
            {
                frames = _statFrames; blits = _statBlits; pixels = _statFramePixels;
                copyMs = _statCopyMs; blitMs = _statBlitMs;
                _statFrames = _statBlits = 0;
                _statFramePixels = 0;
                _statCopyMs = _statBlitMs = 0;
            }
            _statSince = now;
            if (frames == 0 && blits == 0)
                return;
            InAppEngine.AppLog(string.Format(
                "speed: {0} frames ({1:F1}/s, {2:F0}% of the screen each, copy {3:F1} ms each), " +
                "{4} blits ({5:F1}/s, {6:F1} ms each) in {7:F0} ms; {8}",
                frames, frames * 1000.0 / span,
                frames > 0 ? 100.0 * pixels / frames / Math.Max(1, _surfaceW * _surfaceH) : 0,
                frames > 0 ? copyMs / frames : 0,
                blits, blits * 1000.0 / span, blits > 0 ? blitMs / blits : 0, span,
                InAppEngine.Running && EngineConnection.Port == InAppEngine.Port
                    ? "engine inside the app" : "separate engine on port " + EngineConnection.Port));
        }

        bool _benchChecked;

        async void MaybeRunBench()
        {
            if (_benchChecked)
                return;
            _benchChecked = true;
            string url;
            try
            {
                var f = await ApplicationData.Current.LocalFolder.GetFileAsync("bench.txt");
                url = (await FileIO.ReadTextAsync(f)).Trim();
                await f.RenameAsync("bench.done", NameCollisionOption.ReplaceExisting);
            }
            catch (Exception)
            {
                return;     // no bench asked for
            }
            if (url.Length == 0)
                return;
            InAppEngine.AppLog("bench: opening " + url);
            Navigate(url);
            // Until it has loaded (or 30 s), then let it settle
            await Task.Delay(1500);
            for (int i = 0; i < 285 && _loading; i++)
                await Task.Delay(100);
            await Task.Delay(2000);
            ReportSpeed(true);
            InAppEngine.AppLog("bench: flicking");
            double start = StatNow();
            for (int i = 0; i < 8; i++)
            {
                // Down four times, then back up: a quick flick, as a finger leaves it
                Flick(i < 4 ? 3.0 : -3.0);
                await Task.Delay(2500);
            }
            ReportSpeed(true);
            InAppEngine.AppLog(string.Format("bench: done in {0:F0} ms", StatNow() - start));
        }

        // UI thread: a flick at this speed (surface pixels per ms, + scrolls down)
        void Flick(double speed)
        {
            if (_engine == null)
                return;
            _pageHasGesture = false;
            _flingVX = 0;
            _flingVY = speed;
            _flinging = true;
            _lastTick = _clock.Elapsed.TotalMilliseconds;
            HookRendering();
        }
    }
}
