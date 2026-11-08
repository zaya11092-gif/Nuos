using System;
using UnityEngine;

namespace CardboardXR.PVR
{
    /// <summary>
    /// The board's streamed app, floating in 3D space above the dock: a curved
    /// world-space quad showing the decoded video texture, with a window-list
    /// strip for switching board apps. Created by PVRBootstrap at runtime.
    /// </summary>
    public class FloatingWindow : MonoBehaviour
    {
        public float Width = 1.6f;              // meters
        public Vector3 OffsetFromCamera = new(0f, 0.25f, 2.2f);

        GameObject _quad;
        Renderer _renderer;
        MaterialPropertyBlock _props;
        Camera _cam;
        PVRLink _link;
        PVRDecoder _decoder;
        string[] _windowTitles = Array.Empty<string>();
        uint[] _windowIds = Array.Empty<uint>();
        GameObject[] _buttons;

        public void Setup(Camera camera, PVRLink link, PVRDecoder decoder)
        {
            _cam = camera;
            _link = link;
            _decoder = decoder;

            transform.position = camera.transform.position +
                                 camera.transform.rotation * OffsetFromCamera;
            transform.rotation = Quaternion.LookRotation(
                camera.transform.forward, Vector3.up);

            _quad = GameObject.CreatePrimitive(PrimitiveType.Quad);
            _quad.name = "BoardWindow";
            _quad.transform.SetParent(transform, false);
            _quad.transform.localScale = new Vector3(Width, Width * 9f / 16f, 1f);
            _renderer = _quad.GetComponent<Renderer>();
            _props = new MaterialPropertyBlock();
            _renderer.material = new Material(Shader.Find("Unlit/Texture"));
            _renderer.material.color = new Color(0.05f, 0.06f, 0.08f);

            link.OnWindowsList += OnWindowsList;
            link.OnStatus += s => Debug.Log("[FloatingWindow] " + s);

            InvokeRepeating(nameof(PollWindows), 1f, 3f);
        }

        void PollWindows()
        {
            if (_link != null && _link.isActiveAndEnabled) _link.ListWindows();
        }

        void OnWindowsList(string json)
        {
            // Minimal JSON walk: {"id":..,"title":".."} entries.
            try
            {
                var list = ParseWindowJson(json);
                if (list == null) return;
                _windowTitles = new string[list.Count];
                _windowIds = new uint[list.Count];
                for (int i = 0; i < list.Count; i++)
                {
                    _windowIds[i] = list[i].id;
                    _windowTitles[i] = list[i].title ?? ("win " + list[i].id);
                }
                BuildButtons();
            }
            catch (Exception e)
            {
                Debug.LogWarning("[FloatingWindow] list parse: " + e.Message);
            }
        }

        [Serializable]
        class WinEntry { public uint id; public string title; public int w, h; }

        // Minimal parser for [{"id":N,"title":"...","w":N,"h":N}, ...] —
        // flat objects only, avoids the Newtonsoft package dependency.
        static System.Collections.Generic.List<WinEntry> ParseWindowJson(string json)
        {
            var res = new System.Collections.Generic.List<WinEntry>();
            int i = json.IndexOf('"', json.IndexOf('['));
            uint id = 0; string title = null; int w = 0, h = 0;
            while (i >= 0 && i < json.Length)
            {
                int q = json.IndexOf('"', i);
                int q2 = q >= 0 ? json.IndexOf('"', q + 1) : -1;
                if (q < 0 || q2 < 0) break;
                string key = json.Substring(q + 1, q2 - q - 1);
                int colon = json.IndexOf(':', q2);
                if (colon < 0) break;
                if (key == "title")
                {
                    int tq = json.IndexOf('"', colon + 1);
                    int tq2 = json.IndexOf('"', tq + 1);
                    title = json.Substring(tq + 1, tq2 - tq - 1);
                    i = tq2 + 1;
                }
                else
                {
                    int comma = json.IndexOf(',', colon);
                    int close = json.IndexOf('}', colon);
                    int end = (comma >= 0 && comma < close) ? comma : close;
                    if (end < 0) break;
                    int val = int.TryParse(json.Substring(colon + 1, end - colon - 1).Trim(), out var v) ? v : 0;
                    if (key == "id") id = (uint)val;
                    else if (key == "w") w = val;
                    else if (key == "h") h = val;
                    i = end;
                }
                if (json[i] == '}')
                {
                    res.Add(new WinEntry { id = id, title = title ?? ("win " + id), w = w, h = h });
                    id = 0; title = null; w = 0; h = 0;
                    int next = json.IndexOf('{', i);
                    if (next < 0) break;
                    i = next;
                }
            }
            return res;
        }

        void BuildButtons()
        {
            if (_buttons != null)
                foreach (var b in _buttons) if (b) Destroy(b);
            _buttons = new GameObject[Mathf.Min(6, _windowTitles.Length)];
            var parent = new GameObject("WinButtons").transform;
            parent.SetParent(transform, false);
            parent.localPosition = new Vector3(0f, -Width * 9f / 32f - 0.12f, -0.02f);
            float bw = Width / Mathf.Max(3, _buttons.Length) - 0.04f;
            for (int i = 0; i < _buttons.Length; i++)
            {
                var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
                go.name = "win" + _windowIds[i];
                go.transform.SetParent(parent, false);
                float x = (i - (_buttons.Length - 1) / 2f) * (bw + 0.04f);
                go.transform.localPosition = new Vector3(x, 0f, 0f);
                go.transform.localScale = new Vector3(bw, 0.05f, 0.03f);
                go.GetComponent<Renderer>().material.color =
                    new Color(0.15f, 0.35f, 0.6f);
                uint id = _windowIds[i];
                var col = go.AddComponent<BoxCollider>();
                // Tap: use camera ray in Update (gaze + pinch handled by
                // CardboardXR's pointer; simple proximity-select for v1).
                _buttons[i] = go;
            }
        }

        void Update()
        {
            // Video texture from the decoder.
            var tex = _decoder?.GetTexture();
            if (tex != null && _renderer != null)
            {
                _renderer.material.mainTexture = tex;
            }

            // Gaze-select buttons: raycast from camera; tap with the viewer
            // button (mouse left / touchscreen press via Input System).
            if (_cam != null && _buttons != null && Physics.Raycast(
                    _cam.transform.position, _cam.transform.forward,
                    out var hit, 5f))
            {
                bool tapped =
                    (UnityEngine.InputSystem.Mouse.current != null &&
                     UnityEngine.InputSystem.Mouse.current.leftButton.wasPressedThisFrame) ||
                    (UnityEngine.InputSystem.Touchscreen.current != null &&
                     UnityEngine.InputSystem.Touchscreen.current.primaryTouch.press.wasPressedThisFrame);
                if (!tapped) return;
                var go = hit.collider.gameObject;
                for (int i = 0; i < _buttons.Length; i++)
                {
                    if (_buttons[i] != go) continue;
                    Debug.Log("[FloatingWindow] stream window " + _windowIds[i]);
                    _link.StreamWindow(_windowIds[i]);
                    break;
                }
            }
        }
    }
}
