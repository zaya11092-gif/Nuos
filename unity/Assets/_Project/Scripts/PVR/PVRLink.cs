using System;
using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Diagnostics;
using System.Threading;
using UnityEngine;

namespace CardboardXR.PVR
{
    /// <summary>
    /// PhoneVR-link client (NuOS v2). Implements the wire protocol the board's
    /// nuospvr-server speaks:
    ///   - UDP-announces {'p','v','r',0x00,ver} to board:33333 every 500 ms
    ///   - board connects TCP back to phone:33333; sends PAIR_ACCEPT,
    ///     HEADER_NALS ("pvr"+type+u16len framing)
    ///   - phone opens TCP to board:15243; per frame a 64-byte header
    ///     (i64 pts | f32 quat[4] | u32 nal_size | f32 fps[5] | f32 delay[2]
    ///      | i64 wall) followed by one HEVC Annex-B AU
    ///   - phone sends 36-byte pose (quat wxyz, accel, i64 ns) UDP :51423 @60 Hz
    ///   - NuOS v2 control TLVs on the control TCP:
    ///     0x10 LIST_WINDOWS_REQ, 0x11 LAUNCH [u32 len][cmd], 0x12 STREAM_WINDOW [u32]
    /// </summary>
    public class PVRLink : MonoBehaviour
    {
        public static PVRLink Instance { get; private set; }

        [Tooltip("Board IP; empty = discovery via announce (board connects back).")]
        public string boardIp = "";

        public event Action<string> OnStatus;                 // human-readable state
        public event Action<byte[]> OnHeaderNals;             // Annex-B VPS/SPS/PPS
        public event Action<byte[]> OnAccessUnit;             // one HEVC AU (Annex-B)
        public event Action<string> OnWindowsList;            // JSON
        public event Action<IPAddress> OnBoardDiscovered;

        Thread _announceThread, _controlThread, _videoThread, _poseThread;
        readonly ConcurrentQueue<string> _status = new();
        TcpListener _phoneControlListener;
        volatile bool _running;
        volatile bool _paired;
        volatile string _boardIpFound = "";

        TcpClient _video;
        NetworkStream _videoStream;
        UdpClient _pose;
        readonly object _ctlLock = new object();
        TcpClient _ctl;

        byte[] _headerNals;

        void Awake()
        {
            if (Instance != null) { Destroy(gameObject); return; }
            Instance = this;
            DontDestroyOnLoad(gameObject);
        }

        void Start() => StartLink();

        public void StartLink()
        {
            if (_running) return;
            _running = true;
            _announceThread = new Thread(AnnounceLoop) { IsBackground = true };
            _controlThread = new Thread(ControlLoop) { IsBackground = true };
            _poseThread = new Thread(PoseLoop) { IsBackground = true };
            var handsThread = new Thread(HandsLoop) { IsBackground = true };
            _announceThread.Start();
            _controlThread.Start();
            _poseThread.Start();
            handsThread.Start();
            Status("link starting");
        }

        void Update()
        {
            while (_status.TryDequeue(out var s)) OnStatus?.Invoke(s);
            if (_videoThread == null && _paired && _boardIpFound != "")
            {
                _videoThread = new Thread(VideoLoop) { IsBackground = true };
                _videoThread.Start();
            }
        }

        void OnDestroy()
        {
            _running = false;
            try { _phoneControlListener?.Stop(); } catch { }
            try { _video?.Close(); } catch { }
            try { _pose?.Close(); } catch { }
            try { _ctl?.Close(); } catch { }
        }

        void Status(string s) { _status.Enqueue(s); UnityEngine.Debug.Log("[PVRLink] " + s); }

        // ------------------------------------------------------------ announce
        void AnnounceLoop()
        {
            var udp = new UdpClient();
            udp.EnableBroadcast = true;
            var pkt = Encoding.ASCII.GetBytes("pvr");
            var full = new byte[8];
            pkt.CopyTo(full, 0);
            // version u32: 2.0.0 -> rel<<24 | sub<<16 | patch (matches PhoneVR)
            uint ver = (2u << 24);
            full[4] = 0x00; // PAIR_HMD
            full[5] = (byte)(ver >> 24); full[6] = (byte)(ver >> 16);
            full[7] = (byte)(ver >> 8);
            // note: board currently ignores the 4th byte of version; close enough
            while (_running)
            {
                try
                {
                    var targets = string.IsNullOrEmpty(boardIp)
                        ? new[] { new IPEndPoint(IPAddress.Broadcast, 33333) }
                        : new[] { new IPEndPoint(IPAddress.Parse(boardIp), 33333) };
                    foreach (var t in targets) udp.Send(full, full.Length, t);
                }
                catch { /* radio hiccups */ }
                Thread.Sleep(500);
            }
        }

        // ------------------------------------------------------------ control
        void ControlLoop()
        {
            // Phone is the TCP *server* on 33333; the board connects back
            // (PhoneVR convention — keeps NAT happy).
            _phoneControlListener = new TcpListener(IPAddress.Any, 33333);
            _phoneControlListener.Start();
            Status("listening for board on 33333");
            while (_running)
            {
                TcpClient ctl;
                try { ctl = _phoneControlListener.AcceptTcpClient(); }
                catch { return; }
                ctl.NoDelay = true;
                var remote = ((IPEndPoint)ctl.Client.RemoteEndPoint).Address;
                _boardIpFound = remote.ToString();
                OnBoardDiscovered?.Invoke(remote);
                Status("board paired: " + _boardIpFound);
                BindControl(ctl);
                _paired = true;

                // Message pump: PAIR_ACCEPT, HEADER_NALS, ADDITIONAL_DATA...
                var stream = ctl.GetStream();
                var hdr = new byte[6];
                while (_running)
                {
                    if (!ReadExact(stream, hdr, 6)) break;
                    if (hdr[0] != 'p' || hdr[1] != 'v' || hdr[2] != 'r') break;
                    byte type = hdr[3];
                    int len = hdr[4] | (hdr[5] << 8);
                    var payload = new byte[len];
                    if (len > 0 && !ReadExact(stream, payload, len)) break;
                    switch (type)
                    {
                        case 3: // PAIR_ACCEPT
                            Status("PAIR_ACCEPT");
                            break;
                        case 5: // HEADER_NALS
                            _headerNals = payload;
                            Status($"HEADER_NALS {len} bytes");
                            UnityMainThread.Dispatch(() => OnHeaderNals?.Invoke(payload));
                            break;
                        case 4: // ADDITIONAL_DATA (our JSON replies)
                            var json = Encoding.UTF8.GetString(payload);
                            UnityMainThread.Dispatch(() => OnWindowsList?.Invoke(json));
                            break;
                    }
                }
                Status("board control closed");
                _paired = false;
            }
        }

        static bool ReadExact(NetworkStream s, byte[] buf, int n)
        {
            int got = 0;
            while (got < n)
            {
                int r = s.Read(buf, got, n - got);
                if (r <= 0) return false;
                got += r;
            }
            return true;
        }

        // ------------------------------------------------------------ video
        void VideoLoop()
        {
            // Wait for the header NALs (needed to configure the decoder).
            while (_running && _headerNals == null) Thread.Sleep(100);
            var ip = _boardIpFound != "" ? _boardIpFound : boardIp;
            Status("video -> " + ip + ":15243");
            while (_running)
            {
                try
                {
                    _video = new TcpClient();
                    _video.NoDelay = true;
                    _video.Connect(IPAddress.Parse(ip), 15243);
                    _videoStream = _video.GetStream();
                    Status("video connected");
                    var hdr = new byte[64];
                    while (_running)
                    {
                        if (!ReadExact(_videoStream, hdr, 64)) break;
                        uint nalSize = BitConverter.ToUInt32(hdr, 24);
                        if (nalSize == 0 || nalSize > 8_000_000) break;
                        var au = new byte[nalSize];
                        if (!ReadExact(_videoStream, au, (int)nalSize)) break;
                        var copy = au; // fresh array per AU
                        UnityMainThread.Dispatch(() => OnAccessUnit?.Invoke(copy));
                    }
                }
                catch (Exception e) { Status("video error: " + e.Message); }
                try { _video?.Close(); } catch { }
                Thread.Sleep(1000);
            }
        }

        // ------------------------------------------------------------ pose
        void PoseLoop()
        {
            _pose = new UdpClient();
            var pkt = new byte[36];
            while (_running)
            {
                var q = HeadQuat;
                Array.Copy(new[] { q.w, q.x, q.y, q.z }, pkt, 4);
                long ns = Stopwatch.GetTimestamp() * 1000000000L / Stopwatch.Frequency;
                var ts = BitConverter.GetBytes(ns);
                ts.CopyTo(pkt, 28);
                try
                {
                    var ip = _boardIpFound != "" ? _boardIpFound : boardIp;
                    if (ip != "")
                        _pose.Send(pkt, 36, new IPEndPoint(IPAddress.Parse(ip), 51423));
                }
                catch { }
                Thread.Sleep(16);
            }
        }

        /// <summary>Head orientation (room space); fed by the stereo rig.</summary>
        public Quaternion HeadQuat = Quaternion.identity;

        // Hand state for SteamVR controllers (fed by PVRHands).
        public static float LeftPinch, RightPinch;
        public static bool LeftPresent, RightPresent;

        UdpClient _hands;

        void HandsLoop()
        {
            _hands = new UdpClient();
            // NuOS hands packet: 'N','U','H','2' | flags u8 | Lpinch Lgrip
            // Rpinch Rgrip f32 | (21 bytes) — consumed by nuospvr-server.
            var pkt = new byte[21];
            pkt[0] = (byte)'N'; pkt[1] = (byte)'U'; pkt[2] = (byte)'H';
            pkt[3] = (byte)'2';
            while (_running)
            {
                pkt[4] = (byte)((LeftPresent ? 1 : 0) | (RightPresent ? 2 : 0));
                BitConverter.GetBytes(LeftPinch).CopyTo(pkt, 5);
                BitConverter.GetBytes(LeftPinch).CopyTo(pkt, 9);
                BitConverter.GetBytes(RightPinch).CopyTo(pkt, 13);
                BitConverter.GetBytes(RightPinch).CopyTo(pkt, 17);
                try
                {
                    var ip = _boardIpFound != "" ? _boardIpFound : boardIp;
                    if (ip != "")
                        _hands.Send(pkt, pkt.Length,
                            new IPEndPoint(IPAddress.Parse(ip), 51424));
                }
                catch { }
                Thread.Sleep(33);
            }
        }

        // ------------------------------------------------------------ control API
        public void ListWindows()
        {
            lock (_ctlLock) SendTlv(0x10, new byte[0]);
        }

        public void Launch(string command)
        {
            lock (_ctlLock) SendTlv(0x11, Encoding.UTF8.GetBytes(command));
        }

        public void StreamWindow(uint id)
        {
            var p = BitConverter.GetBytes(id);
            lock (_ctlLock) SendTlv(0x12, p);
        }

        void SendTlv(byte type, byte[] payload)
        {
            try
            {
                var ctlStream = _ctl?.GetStream();
                if (ctlStream == null) return;
                var msg = new byte[5 + payload.Length];
                msg[0] = type;
                msg[1] = (byte)payload.Length;
                msg[2] = (byte)(payload.Length >> 8);
                msg[3] = (byte)(payload.Length >> 16);
                msg[4] = (byte)(payload.Length >> 24);
                payload.CopyTo(msg, 5);
                ctlStream.Write(msg, 0, msg.Length);
            }
            catch (Exception e) { Status("tlv failed: " + e.Message); }
        }

        /// <summary>Cache the control connection for TLVs (from the pump).</summary>
        public void BindControl(TcpClient ctl) { lock (_ctlLock) _ctl = ctl; }
    }

    /// <summary>Runs delegates on the Unity main thread.</summary>
    public class UnityMainThread : MonoBehaviour
    {
        static readonly ConcurrentQueue<Action> Q = new();
        static UnityMainThread _i;
        void Awake() { _i = this; DontDestroyOnLoad(gameObject); }
        void Update() { while (Q.TryDequeue(out var a)) a(); }
        public static void Dispatch(Action a)
        {
            if (_i == null) { UnityEngine.Debug.LogWarning("[PVRLink] no main-thread dispatcher"); return; }
            Q.Enqueue(a);
        }
    }
}
