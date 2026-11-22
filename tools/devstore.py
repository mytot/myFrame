#!/usr/bin/env python3
"""
devstore.py — the local app store (v1 default hosting).

One file, standard library only. Serves the catalog, the packages and a small
browser UI for drag-and-drop installs, and proxies the device's HTTP API so the
browser can drive a device on the bench without CORS games.

  python3 tools/devstore.py --pkgs dist --port 8080
  python3 tools/devstore.py --pkgs dist --port 8080 --device 192.168.1.23

Then on the device: Settings -> App store -> Server = http://<this laptop>:8080
and open App Store -> refresh. Or push straight from the browser page:
drag a .fpack onto the page and it is uploaded to the laptop store, the catalog
is rebuilt and (if a device is known) the device is told to refresh, or the
package is posted straight to /api/apps/install if you tick "install now".

Routes
  GET  /                     browser UI
  GET  /catalog.json         signed catalog (rebuilt on demand)
  GET  /catalog.sig          root signature over catalog.json
  GET  /pkgs/<file>          packages
  POST /upload               multipart .fpack  -> store, rebuild catalog
  POST /sign                 body bytes -> {"signature": <b64url>}   (laptop holds the key)
  GET  /device/api/...       proxied to the device's /api/...
  POST /device/api/...       proxied to the device's /api/...
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import threading
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import frame_crypto  # noqa: E402
import frame_pkg  # noqa: E402

REPO_ROOT = os.path.dirname(HERE)
DEFAULT_PUBLISHER = os.path.join(REPO_ROOT, "keys", "dev", "publisher")
DEFAULT_ROOT = os.path.join(REPO_ROOT, "keys", "dev", "store_root")

INDEX_HTML = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<title>Frame dev store</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:#0b0f14;--card:#1a212b;--hi:#232d3a;--accent:#2dd4bf;--dim:#8b98a5;--text:#e6edf3;--danger:#f87171}
*{box-sizing:border-box}body{margin:0;font:15px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--text)}
header{display:flex;gap:16px;align-items:center;padding:14px 22px;background:#11161d;border-bottom:1px solid #1f2937}
h1{font-size:18px;margin:0}main{max-width:1100px;margin:0 auto;padding:22px}
.row{display:flex;gap:12px;flex-wrap:wrap;align-items:center}
input,button,select{font:inherit;background:var(--hi);color:var(--text);border:1px solid #2b3646;border-radius:8px;padding:8px 12px}
button{cursor:pointer}button.primary{background:var(--accent);color:#0b0f14;border-color:transparent;font-weight:600}
.card{background:var(--card);border-radius:12px;padding:16px;margin-bottom:14px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(330px,1fr));gap:14px}
.app{display:flex;gap:12px;align-items:flex-start}.app .mono{width:44px;height:44px;border-radius:12px;background:#1e3a8a33;color:#60a5fa;display:flex;align-items:center;justify-content:center;font-weight:700}
.meta{color:var(--dim);font-size:13px;word-break:break-all}.ok{color:#34d399}.err{color:var(--danger)}
#drop{border:2px dashed #2b3646;border-radius:12px;padding:22px;text-align:center;color:var(--dim);cursor:pointer}
#drop.over{border-color:var(--accent);color:var(--text)}
pre{background:#0b0f14;border:1px solid #1f2937;border-radius:8px;padding:10px;max-height:340px;overflow:auto;font-size:12px}
label{display:inline-flex;gap:6px;align-items:center;color:var(--dim)}h2{font-size:15px;margin:0 0 10px}
</style></head><body>
<header><h1>Frame dev store</h1>
<input id="device" placeholder="device IP, e.g. 192.168.1.23" size="22">
<button onclick="saveDevice()">connect</button>
<span id="devstat" class="meta">no device</span></header>
<main>
<div class="card"><h2>Publish</h2>
<div id="drop">Drop a .fpack here, or click to choose one. It is signed by this server's
publisher key, added to the catalog, and (optionally) installed on the device.</div>
<div class="row" style="margin-top:10px"><label><input type="checkbox" id="installNow" checked> install on device after upload</label>
<span id="upstat" class="meta"></span></div></div>
<div class="card"><div class="row"><h2 style="margin:0">Catalog</h2>
<button onclick="refreshCatalog()">refresh</button><button onclick="loadDevice()">reload device</button>
<span id="catstat" class="meta"></span></div><div id="apps" class="grid"></div></div>
<div class="card"><h2>Device</h2><div id="dev" class="meta">not connected</div></div>
<div class="card"><h2>Device log (last 200 lines)</h2><pre id="log">—</pre></div>
</main>
<script>
let device=localStorage.getItem('frame_device')||'';
if(device){document.getElementById('device').value=device}
function saveDevice(){device=document.getElementById('device').value.trim();localStorage.setItem('frame_device',device);loadDevice()}
async function api(path,opts){const r=await fetch('/device/api/'+path,opts);const t=await r.text();try{return JSON.parse(t)}catch(e){return {raw:t,status:r.status}}}
async function refreshCatalog(){await fetch('/refresh',{method:'POST'});loadCatalog()}
async function loadCatalog(){
 const c=await (await fetch('/catalog.json')).json();
 document.getElementById('catstat').textContent=c.apps.length+' app(s), '+c.generated_at;
 const box=document.getElementById('apps');box.innerHTML='';
 for(const e of c.apps){const m=e.manifest;const d=document.createElement('div');d.className='app';
  d.innerHTML='<div class="mono">'+(m.name[0]||'?').toUpperCase()+'</div><div><div><b>'+esc(m.name)+'</b> <span class="meta">'+esc(m.version)+' · '+esc(m.kind)+'</span></div><div class="meta">'+esc(m.id)+'</div><div class="meta">'+esc(m.description||'')+'</div><div class="row" style="margin-top:6px"><button onclick="install(\\''+esc(m.id)+'\\')">install / update</button><button onclick="openApp(\\''+esc(m.id)+'\\')">open</button><button onclick="removeApp(\\''+esc(m.id)+'\\')">uninstall</button></div></div>';
  box.appendChild(d)}
}
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
async function install(id){const r=await api('apps/'+encodeURIComponent(id)+'/update',{method:'POST'});document.getElementById('upstat').textContent=JSON.stringify(r);loadDevice()}
async function removeApp(id){const r=await api('apps/'+encodeURIComponent(id)+'/uninstall',{method:'POST'});document.getElementById('upstat').textContent=JSON.stringify(r);loadDevice()}
async function openApp(id){const r=await api('launcher/open/'+encodeURIComponent(id),{method:'POST'});document.getElementById('upstat').textContent=JSON.stringify(r)}
async function loadDevice(){
 const el=document.getElementById('dev');if(!device){el.textContent='set a device IP above';return}
 const r=await api('apps');document.getElementById('devstat').textContent='connected';
 if(r.apps){el.innerHTML='<table>'+r.apps.map(a=>'<tr><td>'+esc(a.id)+'</td><td>'+esc(a.version)+'</td><td>'+esc(a.kind)+'</td><td>'+(a.update?'<span class="err">update</span>':'<span class="ok">ok</span>')+'</td></tr>').join('')+'</table>'}
 else{el.textContent=JSON.stringify(r)}
 const l=await api('logs?limit=200');document.getElementById('log').textContent=(l.lines||[]).map(x=>x.tag+': '+x.text).join('\\n')}
const drop=document.getElementById('drop');const file=document.createElement('input');file.type='file';file.accept='.fpack';file.hidden=true;document.body.appendChild(file);
drop.onclick=()=>file.click();
drop.ondragover=e=>{e.preventDefault();drop.classList.add('over')};drop.ondragleave=()=>drop.classList.remove('over');
drop.ondrop=async e=>{e.preventDefault();drop.classList.remove('over');if(e.dataTransfer.files.length)await upload(e.dataTransfer.files[0])};
file.onchange=async()=>{if(file.files.length)await upload(file.files[0]);file.value=''};
async function upload(f){const st=document.getElementById('upstat');st.textContent='uploading '+f.name+'…';
 const fd=new FormData();fd.append('file',f);
 const r=await fetch('/upload',{method:'POST',body:fd});const j=await r.json();st.textContent=JSON.stringify(j);
 await loadCatalog();if(document.getElementById('installNow').checked&&device&&j.ok){st.textContent+=' installing…';const ir=await api('apps/install',{method:'POST',body:f});st.textContent+=' '+JSON.stringify(await ir.json());loadDevice()}}
loadCatalog();loadDevice();setInterval(loadDevice,5000);
</script></body></html>
"""


class StoreState:
    def __init__(self, args):
        self.pkgs = os.path.abspath(args.pkgs)
        self.port = args.port
        self.base_url = args.base_url or f"http://{args.host if args.host != '0.0.0.0' else '192.168.1.10'}:{args.port}"
        self.publisher = args.publisher
        self.root = args.root
        self.allow_unsigned = args.allow_unsigned
        self.device = args.device
        self.lock = threading.Lock()
        self.catalog_bytes: bytes | None = None
        self.catalog_sig: str | None = None
        self.last_error = ""
        os.makedirs(self.pkgs, exist_ok=True)
        self.rebuild()

    # -- catalog ---------------------------------------------------------
    def publisher_pub(self) -> bytes | None:
        if self.publisher.endswith(".pub"):
            return frame_pkg.load_pub(self.publisher)
        if os.path.exists(self.publisher + ".pub"):
            return frame_pkg.load_pub(self.publisher + ".pub")
        return frame_crypto.public_from_seed(frame_crypto.load_seed(self.publisher + ".key"))

    def rebuild(self) -> dict:
        with self.lock:
            try:
                root_seed = frame_crypto.load_seed(self.root + ".key")
                cat, sig = frame_pkg.build_catalog(
                    self.pkgs, base_url=self.base_url, publisher_pub=self.publisher_pub(),
                    root_seed=root_seed)
                self.catalog_bytes, self.catalog_sig = cat, frame_pkg.b64url(sig)
                self.last_error = ""
                n = len(json.loads(cat.decode("utf-8"))["apps"])
                print(f"[store] catalog rebuilt: {n} app(s) from {self.pkgs}")
                return {"ok": True, "apps": n}
            except (frame_pkg.PkgError, FileNotFoundError) as e:
                self.last_error = str(e)
                print(f"[store] catalog: {e}")
                return {"ok": False, "error": str(e)}

    def save_upload(self, filename: str, data: bytes) -> dict:
        safe = re.sub(r"[^A-Za-z0-9._-]", "_", os.path.basename(filename))
        if not safe.endswith(".fpack"):
            safe += ".fpack"
        path = os.path.join(self.pkgs, safe)
        with open(path, "wb") as f:
            f.write(data)
        # Validate before it enters the catalog: a broken package must not
        # poison the list for the device.
        try:
            m, payload, sig, _ = frame_pkg.read_fpack(path)
            errs = frame_pkg.validate_manifest(m, payload_size=len(payload))
            if errs and not self.allow_unsigned:
                os.remove(path)
                return {"ok": False, "error": "invalid manifest: " + "; ".join(errs)}
            if m.get("kind") == "managed":
                frame_pkg.inspect_wasm(payload)
            if not sig and not self.allow_unsigned:
                os.remove(path)
                return {"ok": False, "error": "package is unsigned and --allow-unsigned is off"}
        except (frame_pkg.PkgError, json.JSONDecodeError, KeyError) as e:
            os.remove(path)
            return {"ok": False, "error": str(e)}
        return {"ok": True, "file": safe, "manifest": m, "catalog": self.rebuild()}

    def sign(self, body: bytes) -> str:
        seed = frame_crypto.load_seed(self.publisher + ".key")
        return frame_pkg.b64url(frame_crypto.sign(body, seed))

    # -- device proxy -----------------------------------------------------
    def device_request(self, method: str, path: str, body: bytes | None = None,
                       content_type: str | None = None) -> tuple[int, object]:
        if not self.device:
            return 502, {"error": "no device configured (--device IP)"}
        url = f"http://{self.device}/{path.lstrip('/')}"
        req = urllib.request.Request(url, data=body, method=method)
        if content_type:
            req.add_header("Content-Type", content_type)
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                raw = r.read()
                ct = r.headers.get("Content-Type", "")
                status = r.status
        except urllib.error.HTTPError as e:
            raw, status = e.read(), e.code
        except (urllib.error.URLError, OSError) as e:
            return 502, {"error": f"device unreachable: {e}"}
        try:
            return status, json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return status, {"raw": raw[:2000].decode("utf-8", "replace")}


def make_handler(state: StoreState):
    class Handler(BaseHTTPRequestHandler):
        server_version = "FrameDevStore/1.0"

        def log_message(self, fmt, *a):  # quieter, prefixed
            sys.stdout.write("[store] %s - %s\n" % (self.address_string(), fmt % a))

        def _send(self, status: int, body: bytes, ctype: str = "application/json") -> None:
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, obj, status: int = 200) -> None:
            self._send(status, json.dumps(obj, indent=1).encode("utf-8"))

        # -- GET --------------------------------------------------------
        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path in ("/", "/index.html"):
                return self._send(200, INDEX_HTML.encode("utf-8"), "text/html; charset=utf-8")
            if path == "/catalog.json":
                with state.lock:
                    if state.catalog_bytes is None:
                        state.rebuild()
                    body = state.catalog_bytes or b'{"schema":1,"apps":[]}'
                return self._send(200, body)
            if path == "/catalog.sig":
                with state.lock:
                    sig = state.catalog_sig or ""
                return self._send(200, (sig + "\n").encode("ascii"), "text/plain")
            if path.startswith("/pkgs/"):
                name = os.path.basename(path[len("/pkgs/"):])
                fp = os.path.join(state.pkgs, name)
                if not os.path.isfile(fp):
                    return self._json({"error": "not found"}, 404)
                with open(fp, "rb") as f:
                    data = f.read()
                return self._send(200, data, "application/octet-stream")
            if path == "/status":
                return self._json({"pkgs": sorted(n for n in os.listdir(state.pkgs) if n.endswith(".fpack")),
                                   "device": state.device, "base_url": state.base_url,
                                   "last_error": state.last_error})
            if path.startswith("/device/api/"):
                status, obj = state.device_request("GET", path[len("/device/"):])
                return self._json(obj, status)
            return self._json({"error": "not found"}, 404)

        # -- POST -------------------------------------------------------
        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            path = self.path.split("?", 1)[0]
            if path == "/upload":
                ctype = self.headers.get("Content-Type", "")
                if not ctype.startswith("multipart/form-data"):
                    return self._json({"error": "expected multipart/form-data"}, 400)
                try:
                    m = re.search(r'boundary="?([^";]+)"?', ctype)
                    if not m:
                        raise ValueError("no boundary in Content-Type")
                    body = self.rfile.read(length)
                    filename, data = _first_part(body, b"--" + m.group(1).encode("ascii"))
                    if not data:
                        raise ValueError("no file part")
                except Exception as e:  # noqa: BLE001
                    return self._json({"error": f"bad multipart: {e}"}, 400)
                return self._json(state.save_upload(filename or "upload.fpack", data))
            if path == "/refresh":
                return self._json(state.rebuild())
            if path == "/sign":
                body = self.rfile.read(length)
                return self._json({"signature": state.sign(body)})
            if path == "/device/api/catalog/refresh":
                status, obj = state.device_request("POST", "api/catalog/refresh")
                return self._json(obj, status)
            if path.startswith("/device/api/"):
                body = self.rfile.read(length) if length else None
                status, obj = state.device_request("POST", path[len("/device/"):], body,
                                                    self.headers.get("Content-Type"))
                return self._json(obj, status)
            return self._json({"error": "not found"}, 404)

    return Handler


def _first_part(body: bytes, boundary: bytes) -> tuple[str, bytes]:
    """Minimal multipart: returns (filename, data) of the first file part."""
    sep = b"\r\n" + boundary
    start = body.find(boundary)
    if start < 0:
        raise ValueError("boundary not found")
    pos = start + len(boundary)
    if body[pos:pos + 2] == b"--":
        raise ValueError("empty multipart body")
    pos += 2  # \r\n
    hdr_end = body.find(b"\r\n\r\n", pos)
    if hdr_end < 0:
        raise ValueError("part headers not terminated")
    headers = body[pos:hdr_end].decode("utf-8", "replace")
    hdr_end += 4
    end = body.find(sep, hdr_end)
    data = body[hdr_end:end if end >= 0 else len(body)]
    m = re.search(r'filename="([^"]*)"', headers)
    return (m.group(1) if m else ""), data


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pkgs", default="dist", help="directory of .fpack files served and catalogued")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--base-url", help="what goes into download_url (default http://HOST:PORT)")
    ap.add_argument("--publisher", default=DEFAULT_PUBLISHER)
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--allow-unsigned", action="store_true")
    ap.add_argument("--device", help="device IP for the browser proxy (can be set in the UI)")
    args = ap.parse_args(argv)

    state = StoreState(args)
    httpd = ThreadingHTTPServer((args.host, args.port), make_handler(state))
    print(f"[store] serving {state.pkgs} on http://{args.host}:{args.port}  (catalog base {state.base_url})")
    print(f"[store] set the device's store URL to {state.base_url} (Settings -> App store -> Server)")
    if state.device:
        print(f"[store] proxying device {state.device} at /device/api/")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
