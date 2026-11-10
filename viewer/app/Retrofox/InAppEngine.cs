using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.Storage;

namespace Retrofox
{
    // The engine inside the app: Retrofox carries Gecko's files at its package root (msbuild
    // /p:InAppEngine=true, the default; scripts/stage-inapp-engine.sh gathers them) and runs it on a
    // thread of its own (Gecko's main thread), talking to it over loopback inside the same process.
    // If the files aren't there or the engine can't start, the app falls back to a separate
    // rfxengine.exe on port 8081, the way it worked before.
    static class InAppEngine
    {
        public const int Port = 8091;           // not the separate engine's 8081
        public const int ExternalPort = 8081;

        [DllImport("xul.dll", CallingConvention = CallingConvention.Cdecl)]
        static extern int RetrofoxEngineMain(int argc,
            [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] argv);

        [DllImport("xul.dll", CallingConvention = CallingConvention.Cdecl)]
        static extern int RetrofoxFrameLock(out IntPtr data, out int width, out int height);

        [DllImport("xul.dll", CallingConvention = CallingConvention.Cdecl)]
        static extern void RetrofoxFrameUnlock();

        // Copies a rectangle of the engine's shared frame buffer into the app's copy of the
        // surface (same size, BGRA, rows packed). False if the buffer doesn't match.
        public static bool CopyFrame(int x, int y, int w, int h, byte[] dest, int destWidth, int destHeight)
        {
            IntPtr data;
            int width, height;
            int stride = RetrofoxFrameLock(out data, out width, out height);
            try
            {
                if (data == IntPtr.Zero || width != destWidth || height != destHeight ||
                    x + w > width || y + h > height)
                    return false;
                for (int row = y; row < y + h; row++)
                {
                    int off = row * stride + x * 4;
                    Marshal.Copy(IntPtr.Add(data, off), dest, (row * destWidth + x) * 4, w * 4);
                }
                return true;
            }
            finally
            {
                RetrofoxFrameUnlock();
            }
        }

        static bool _started;

        // Done once the app knows which engine it talks to (EngineConnection.Port is set): the
        // app connects only after this, or it can reach a separate engine still running on the
        // old port before the one inside it is up
        static readonly TaskCompletionSource<bool> _decided = new TaskCompletionSource<bool>();
        public static Task Decided { get { return _decided.Task; } }

        // The engine runs in this process (as far as the app can tell: it was started and hasn't
        // returned or failed)
        public static bool Running { get; private set; }

        // Called on the UI thread when the engine inside the app couldn't start (the app then
        // looks for the separate one)
        public static event Action Failed;

        public static string LogPath
        {
            get { return Path.Combine(ApplicationData.Current.LocalFolder.Path, "engine.log"); }
        }

        public static async void Start(string home)
        {
            if (_started)
                return;
            _started = true;
            // The log first, on disk before anything native runs, so a crash in the engine still
            // leaves a trace (D:\WPSystem\AppData\Local\Packages\<package>\LocalState when the app
            // is on the SD card)
            await AppLogAsync("app started");
            bool packaged = false;
            try
            {
                await Package.Current.InstalledLocation.GetFileAsync("xul.dll");
                packaged = true;
            }
            catch (Exception)
            {
            }
            if (!packaged)
            {
                await AppLogAsync("no engine in the package: using the separate rfxengine.exe");
                _decided.TrySetResult(false);
                return;     // EngineConnection.Port stays the separate engine's
            }
            EngineConnection.Port = Port;
            Running = true;
            _decided.TrySetResult(true);
            string local = ApplicationData.Current.LocalFolder.Path;
            string[] args =
            {
                "rfxengine", "--in-app", "--port", Port.ToString(),
                "--gre", Package.Current.InstalledLocation.Path,
                "--profile", Path.Combine(local, "profile"),
                "--log", LogPath,
                "--home", home
            };
            await AppLogAsync("starting the engine inside the app: " + string.Join(" ", args));
            var dispatcher = Windows.ApplicationModel.Core.CoreApplication.MainView.CoreWindow.Dispatcher;
            var unused = Task.Factory.StartNew(() =>
            {
                string end;
                try
                {
                    int code = RetrofoxEngineMain(args.Length, args);
                    end = "RetrofoxEngineMain returned " + code;
                }
                catch (Exception e)
                {
                    // The DLL didn't load, or an entry point is missing
                    end = "the engine inside the app failed to start: " + e;
                }
                Running = false;
                AppLog(end);
                EngineConnection.Port = ExternalPort;
                var d = dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Normal, () =>
                {
                    var h = Failed;
                    if (h != null)
                        h();
                });
            }, TaskCreationOptions.LongRunning);
        }

        // The app's side of the story, next to the engine's own log (engine.log)
        public static async void AppLog(string line)
        {
            await AppLogAsync(line);
        }

        static async Task AppLogAsync(string line)
        {
            try
            {
                var f = await ApplicationData.Current.LocalFolder.CreateFileAsync("app-engine.log",
                    CreationCollisionOption.OpenIfExists);
                await FileIO.AppendTextAsync(f, DateTime.Now.ToString("HH:mm:ss ") + line + "\r\n");
            }
            catch (Exception)
            {
            }
        }
    }
}
