// The app side of viewer/PROTOCOL.md: one TCP connection to rfxengine.exe.
using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Windows.Networking;
using Windows.Networking.Sockets;
using Windows.Storage.Streams;

namespace Retrofox
{
    public sealed class FrameMessage
    {
        public uint Serial;
        public int SurfaceWidth, SurfaceHeight;
        public int X, Y, Width, Height;
        public byte Format;          // 0 BGRA, 1 RGB565, 2 shared (no pixels: in the engine's buffer)
        public byte[] Pixels;        // Width*Height pixels, rows packed
    }

    // Where the page is scrolled in a frame (PROTOCOL.md, SCROLL); physical pixels
    // A strip of the page outside the screen, drawn ahead of time (STRIP): its place in the page
    // (physical pixels from the page's top left) and its BGRA pixels
    public sealed class StripMessage
    {
        public int PageX, PageY, Width, Height;
        public byte[] Pixels;
    }

    // The page's position: fixed boxes on screen in the frame Serial (x, y, w, h quads)
    public sealed class FixedMessage
    {
        public uint Serial;
        public int[] Rects;
    }

    public sealed class ScrollInfo
    {
        public uint Serial;              // the frame that shows this
        public int AppliedX, AppliedY;   // SCROLL deltas the engine has handled
        public int X, Y, MaxX, MaxY;
        public bool Inner;               // the last scroll moved a box inside the page
    }

    public sealed class EngineConnection : IDisposable
    {
        public static int Port = 8081;   // InAppEngine uses its own
        public const ushort Version = 1;

        // Message types
        const ushort Welcome = 1, Frame = 2, State = 3, Focus = 4, Scroll_ = 5, Tabs = 6, Gesture = 7, Strip = 8, Fixed = 9;
        const ushort Hello = 100, Navigate = 101, Command = 102, Pointer = 103,
                     Scroll = 104, Key = 105, Text = 106, Resize = 107, FrameAck = 108, Log = 109, Tab = 110;
        const uint MaxPayload = 16 * 1024 * 1024;

        public const byte CmdReload = 0, CmdBack = 1, CmdForward = 2, CmdStop = 3, CmdRestartEngine = 4;
        public const byte PointerDown = 0, PointerMove = 1, PointerUp = 2, PointerCancel = 3;
        public const byte TabNew = 0, TabSelect = 1, TabClose = 2;
        public const byte FormatShared = 2;

        StreamSocket _socket;
        DataReader _reader;
        DataWriter _writer;
        readonly SemaphoreSlim _sendLock = new SemaphoreSlim(1);
        bool _closed;

        // Raised on a background thread.
        public event Action<FrameMessage> FrameReceived;
        public event Action<string> StateReceived;
        public event Action<bool> FocusReceived;
        public event Action<ScrollInfo> ScrollReceived;
        public event Action<string> TabsReceived;
        // Who handles the touch drag: GestureNone, GestureUndecided, GestureBrowser, GesturePage
        public event Action<byte> GestureReceived;
        public event Action<StripMessage> StripReceived;
        public event Action<FixedMessage> FixedReceived;
        public const byte GestureNone = 0, GestureUndecided = 1, GestureBrowser = 2, GesturePage = 3;
        public event Action Disconnected;

        public string Host { get; private set; }

        // Tries each host in turn; returns false if none answers.
        public async Task<bool> ConnectAsync(IEnumerable<string> hosts, int width, int height,
                                             int scalePercent)
        {
            foreach (var host in hosts)
            {
                var socket = new StreamSocket();
                socket.Control.NoDelay = true;
                try
                {
                    var timeout = new CancellationTokenSource(1500);
                    await socket.ConnectAsync(new HostName(host), Port.ToString())
                                .AsTask(timeout.Token);
                }
                catch (Exception)
                {
                    socket.Dispose();
                    continue;
                }
                _socket = socket;
                Host = host;
                _reader = new DataReader(socket.InputStream) { ByteOrder = ByteOrder.LittleEndian };
                _writer = new DataWriter(socket.OutputStream) { ByteOrder = ByteOrder.LittleEndian };
                var hello = new byte[9];
                Put16(hello, 0, Version);
                Put16(hello, 2, width);
                Put16(hello, 4, height);
                Put16(hello, 6, scalePercent);
                // BGRA: same layout as WriteableBitmap, no conversion. With the engine inside the
                // app, frames come through shared memory instead (FormatShared).
                hello[8] = InAppEngine.Running && Port == InAppEngine.Port ? FormatShared : (byte)0;
                if (!await SendAsync(Hello, hello))
                    return false;
                var ignored = ReadLoop();
                return true;
            }
            return false;
        }

        async Task ReadLoop()
        {
            try
            {
                while (!_closed)
                {
                    await LoadExact(8);
                    ushort type = _reader.ReadUInt16();
                    _reader.ReadUInt16();
                    uint length = _reader.ReadUInt32();
                    if (length > MaxPayload)
                        throw new InvalidOperationException("oversized message");
                    var payload = new byte[length];
                    if (length > 0)
                    {
                        await LoadExact(length);
                        _reader.ReadBytes(payload);
                    }
                    Dispatch(type, payload);
                }
            }
            catch (Exception)
            {
            }
            Close();
        }

        async Task LoadExact(uint count)
        {
            while (_reader.UnconsumedBufferLength < count)
            {
                uint got = await _reader.LoadAsync(count - _reader.UnconsumedBufferLength);
                if (got == 0)
                    throw new InvalidOperationException("engine closed the connection");
            }
        }

        void Dispatch(ushort type, byte[] p)
        {
            switch (type)
            {
                case Frame:
                    if (p.Length < 20) return;
                    var f = new FrameMessage
                    {
                        Serial = Get32(p, 0),
                        SurfaceWidth = Get16(p, 4),
                        SurfaceHeight = Get16(p, 6),
                        X = Get16(p, 8),
                        Y = Get16(p, 10),
                        Width = Get16(p, 12),
                        Height = Get16(p, 14),
                        Format = p[16],
                    };
                    int bpp = f.Format == 1 ? 2 : 4;
                    if (p[17] != 0) return;
                    if (f.Format != FormatShared)
                    {
                        if (p.Length < 20 + f.Width * f.Height * bpp) return;
                        f.Pixels = new byte[f.Width * f.Height * bpp];
                        System.Buffer.BlockCopy(p, 20, f.Pixels, 0, f.Pixels.Length);
                    }
                    var fh = FrameReceived;
                    if (fh != null) fh(f);
                    break;
                case State:
                    var sh = StateReceived;
                    if (sh != null) sh(Encoding.UTF8.GetString(p, 0, p.Length));
                    break;
                case Focus:
                    var fo = FocusReceived;
                    if (fo != null && p.Length >= 1) fo(p[0] != 0);
                    break;
                case Gesture:
                    var gh = GestureReceived;
                    if (gh != null && p.Length >= 1) gh(p[0]);
                    break;
                case Strip:
                    var sr = StripReceived;
                    if (sr == null || p.Length < 12) return;
                    var st = new StripMessage
                    {
                        PageX = (int)Get32(p, 0),
                        PageY = (int)Get32(p, 4),
                        Width = Get16(p, 8),
                        Height = Get16(p, 10),
                    };
                    if (p.Length < 12 + st.Width * st.Height * 4) return;
                    st.Pixels = new byte[st.Width * st.Height * 4];
                    System.Buffer.BlockCopy(p, 12, st.Pixels, 0, st.Pixels.Length);
                    sr(st);
                    break;
                case Fixed:
                    var fx = FixedReceived;
                    if (fx == null || p.Length < 4) return;
                    int n = (p.Length - 4) / 8;
                    var rects = new int[n * 4];
                    for (int i = 0; i < n * 4; i++)
                        rects[i] = (short)Get16(p, 4 + i * 2);
                    fx(new FixedMessage { Serial = Get32(p, 0), Rects = rects });
                    break;
                case Tabs:
                    var th = TabsReceived;
                    if (th != null) th(Encoding.UTF8.GetString(p, 0, p.Length));
                    break;
                case Scroll_:
                    var so = ScrollReceived;
                    if (so == null || p.Length < 29) return;
                    so(new ScrollInfo
                    {
                        Serial = Get32(p, 0),
                        AppliedX = (int)Get32(p, 4),
                        AppliedY = (int)Get32(p, 8),
                        X = (int)Get32(p, 12),
                        Y = (int)Get32(p, 16),
                        MaxX = (int)Get32(p, 20),
                        MaxY = (int)Get32(p, 24),
                        Inner = p[28] != 0,
                    });
                    break;
                default:
                    break;   // Welcome and unknown types
            }
        }

        public Task<bool> SendNavigate(string url) { return SendAsync(Navigate, Encoding.UTF8.GetBytes(url)); }
        public Task<bool> SendCommand(byte command) { return SendAsync(Command, new[] { command }); }
        public Task<bool> SendText(string text) { return SendAsync(Text, Encoding.UTF8.GetBytes(text)); }
        public Task<bool> SendLog(string line) { return SendAsync(Log, Encoding.UTF8.GetBytes(line)); }

        // action TabNew (url: what to open), TabSelect or TabClose (id: which tab)
        public Task<bool> SendTab(byte action, uint id, string url = "")
        {
            byte[] u = Encoding.UTF8.GetBytes(url ?? "");
            var b = new byte[8 + u.Length];
            b[0] = action;
            Put32(b, 4, id);
            System.Buffer.BlockCopy(u, 0, b, 8, u.Length);
            return SendAsync(Tab, b);
        }

        public Task<bool> SendPointer(byte action, int x, int y)
        {
            var b = new byte[6];
            b[0] = action;
            Put16(b, 2, x);
            Put16(b, 4, y);
            return SendAsync(Pointer, b);
        }

        public Task<bool> SendScroll(int dx, int dy)
        {
            var b = new byte[4];
            Put16(b, 0, dx);
            Put16(b, 2, dy);
            return SendAsync(Scroll, b);
        }

        public Task<bool> SendKey(bool down, int virtualKey)
        {
            var b = new byte[4];
            b[0] = (byte)(down ? 0 : 1);
            Put16(b, 2, virtualKey);
            return SendAsync(Key, b);
        }

        public Task<bool> SendResize(int width, int height)
        {
            var b = new byte[4];
            Put16(b, 0, width);
            Put16(b, 2, height);
            return SendAsync(Resize, b);
        }

        public Task<bool> SendFrameAck(uint serial)
        {
            var b = new byte[4];
            Put32(b, 0, serial);
            return SendAsync(FrameAck, b);
        }

        async Task<bool> SendAsync(ushort type, byte[] payload)
        {
            if (_closed || _writer == null)
                return false;
            await _sendLock.WaitAsync();
            try
            {
                _writer.WriteUInt16(type);
                _writer.WriteUInt16(0);
                _writer.WriteUInt32((uint)payload.Length);
                _writer.WriteBytes(payload);
                await _writer.StoreAsync();
                return true;
            }
            catch (Exception)
            {
                Close();
                return false;
            }
            finally
            {
                _sendLock.Release();
            }
        }

        void Close()
        {
            if (_closed)
                return;
            _closed = true;
            try { if (_socket != null) _socket.Dispose(); } catch (Exception) { }
            var d = Disconnected;
            if (d != null) d();
        }

        public void Dispose() { Close(); }

        static void Put16(byte[] b, int o, int v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); }
        static void Put32(byte[] b, int o, uint v) { Put16(b, o, (int)(v & 0xFFFF)); Put16(b, o + 2, (int)(v >> 16)); }
        static int Get16(byte[] b, int o) { return b[o] | (b[o + 1] << 8); }
        static uint Get32(byte[] b, int o) { return (uint)(Get16(b, o) | (Get16(b, o + 2) << 16)); }
    }
}
