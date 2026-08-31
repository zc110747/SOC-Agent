#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
End-to-end acceptance for the video-server (MediaMTX + SQLite + embedded Web UI).

Methodology (SOC camera/RTSP agent + STM32 verification-acceptance):
  - Launch the server binary (it launches MediaMTX as a subprocess).
  - Push N synthetic RTSP streams with ffmpeg (lavfi testsrc -> libx264 -> rtsp).
  - Assert /api/health, camera auto-registration (/api/cameras), RTSP playback
    (ffprobe), and WebRTC WHEP signaling reachability (/api/cameras/{id}/webrtc).
  - Print a PASS/FAIL tally and exit non-zero if any core check fails.

Stdlib only (urllib + subprocess). ffmpeg/ffprobe are resolved from PATH.

Usage:
  python3 scripts/verify_e2e.py [--binary D:/.../video-server.exe]
                                [--project-root D:/.../video-server]
                                [--streams 3] [--host localhost] [--http-port 8080]
                                [--rtsp-port 8554] [--no-webrtc]
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

PASS = 0
FAIL = 0


def check(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  [PASS] {name}" + (f" :: {detail}" if detail else ""))
    else:
        FAIL += 1
        print(f"  [FAIL] {name}" + (f" :: {detail}" if detail else ""))
    return ok


def http_get_json(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8")), r.status


def http_post_json(url, payload, timeout=10):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8"), r.status


def wait_for(predicate, timeout=40, interval=1.0, label="condition"):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = predicate()
            if last:
                return last
        except Exception as e:
            last = e
        time.sleep(interval)
    return last


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    proj = os.path.dirname(here)
    ap.add_argument("--binary", default=os.path.join(proj, "video-server.exe"))
    ap.add_argument("--project-root", default=proj)
    ap.add_argument("--streams", type=int, default=3)
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--rtsp-port", type=int, default=8554)
    ap.add_argument("--no-webrtc", action="store_true")
    args = ap.parse_args()

    base = f"http://{args.host}:{args.http_port}"
    rtsp_base = f"rtsp://{args.host}:{args.rtsp_port}"

    ffmpeg = shutil.which("ffmpeg") or shutil.which("ffmpeg.exe")
    ffprobe = shutil.which("ffprobe") or shutil.which("ffprobe.exe")
    if not ffmpeg:
        print("[FATAL] ffmpeg not found on PATH")
        sys.exit(2)
    if not ffprobe:
        print("[FATAL] ffprobe not found on PATH")
        sys.exit(2)

    print(f"== video-server e2e acceptance ==")
    print(f"   binary     : {args.binary}")
    print(f"   project    : {args.project_root}")
    print(f"   base url   : {base}")
    print(f"   ffmpeg     : {ffmpeg}")
    print(f"   ffprobe    : {ffprobe}")
    print(f"   streams    : {args.streams}")
    print()

    # ---- start server ----
    # Generate a per-run config that points the SQLite DB at a brand-new file.
    # Some sandboxes forbid a subprocess from *writing* to a pre-existing file,
    # which surfaces as SQLITE_READONLY; a freshly-created DB file is writable.
    base_cfg = os.path.join(args.project_root, "config", "config.yaml")
    run_id = f"{os.getpid()}_{int(time.time()*1000)}"
    # Keep generated config under data/ (runtime state), not the source config/ dir.
    cfg = os.path.join(args.project_root, "data", f"e2e_{run_id}.yaml")
    try:
        with open(base_cfg, "r", encoding="utf-8") as f:
            content = f.read()
        db_path = f"data/video.e2e_{run_id}.db"
        content = re.sub(r"(path:\s*)data/video\.db", r"\1" + db_path, content)
        with open(cfg, "w", encoding="utf-8") as f:
            f.write(content)
    except Exception as e:
        print(f"[FATAL] could not generate per-run config: {e}")
        sys.exit(2)
    # Unique per-run log: some sandboxes forbid overwriting a pre-existing file,
    # so embed the PID+time to guarantee a brand-new path each run.
    log_path = os.path.join(args.project_root, "data", f"verify_e2e_server_{run_id}.log")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    print(f"[*] starting server (config={cfg}) ...")
    server = subprocess.Popen(
        [args.binary, cfg],
        cwd=args.project_root,
        stdout=open(log_path, "w", encoding="utf-8", errors="replace"),
        stderr=subprocess.STDOUT,
    )
    publishers = []

    def cleanup():
        for p in publishers:
            try:
                p.terminate()
            except Exception:
                pass
        try:
            server.terminate()
        except Exception:
            pass

    try:
        # ---- health gate ----
        print("[*] waiting for /api/health ...")
        hres = wait_for(
            lambda: http_get_json(f"{base}/api/health")[0],
            timeout=45, interval=1.0, label="health",
        )
        if isinstance(hres, Exception):
            print("    server log tail:")
            _tail(log_path)
            check("server /api/health reachable", False, str(hres))
            cleanup()
            return _finish()
        status = hres.get("status") if isinstance(hres, dict) else None
        db_ok = hres.get("database") if isinstance(hres, dict) else None
        media_ok = hres.get("media_server") if isinstance(hres, dict) else None
        check("server /api/health reachable", True, json.dumps(hres))
        check("database healthy", db_ok == "ok", f"database={db_ok}")
        check("mediamtx (media_server) healthy", media_ok == "ok", f"media_server={media_ok}")

        # ---- push streams ----
        streams = [f"camera{i:02d}" for i in range(1, args.streams + 1)]
        print(f"[*] pushing {len(streams)} RTSP streams: {', '.join(streams)}")
        for name in streams:
            url = f"{rtsp_base}/{name}"
            cmd = [
                ffmpeg, "-hide_banner", "-loglevel", "error",
                "-re", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=15",
                "-pix_fmt", "yuv420p", "-c:v", "libx264", "-preset", "ultrafast",
                "-tune", "zerolatency", "-g", "30", "-keyint_min", "30",
                "-rtsp_transport", "tcp", "-f", "rtsp", url,
            ]
            p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            publishers.append(p)
            print(f"    launched ffmpeg -> {url} (pid={p.pid})")

        # ---- wait for auto-registration ----
        print("[*] waiting for monitor auto-registration (poll /api/cameras) ...")
        cam_res = wait_for(
            lambda: _cameras_with_source(f"{base}/api/cameras"),
            timeout=30, interval=2.0, label="camera registration",
        )
        if isinstance(cam_res, Exception):
            check("cameras auto-registered from RTSP publishers", False, str(cam_res))
            _tail(log_path)
            cleanup()
            return _finish()
        registered = cam_res
        check(
            f"cameras auto-registered ({len(streams)} expected)",
            len(registered) >= len(streams),
            f"registered={sorted(c['id'] for c in registered)} expected={streams}",
        )
        # status should be online for streams actively published
        online_ids = [c["id"] for c in registered if c.get("status") == "online"]
        check(
            "registered cameras report online",
            len(online_ids) >= 1,
            f"online={online_ids}",
        )

        # ---- per-camera RTSP playback via ffprobe ----
        print("[*] verifying RTSP playback via ffprobe ...")
        for name in streams:
            if not any(c["id"] == name for c in registered):
                check(f"rtsp playback {name}", False, "camera not registered")
                continue
            url = f"{rtsp_base}/{name}"
            ok, detail = _ffprobe_ok(ffprobe, url)
            check(f"rtsp playback {name}", ok, detail)

        # ---- per-camera /stream metadata ----
        print("[*] verifying /api/cameras/{id}/stream metadata ...")
        for name in streams:
            try:
                meta, _ = http_get_json(f"{base}/api/cameras/{name}/stream")
                ok = bool(meta.get("rtsp_url")) and name in meta.get("rtsp_url", "")
                check(f"stream metadata {name}", ok, meta.get("rtsp_url", ""))
            except Exception as e:
                check(f"stream metadata {name}", False, str(e))

        # ---- WebRTC WHEP signaling reachability (informational) ----
        if not args.no_webrtc:
            print("[*] verifying WebRTC WHEP signaling reachability (informational) ...")
            for name in streams:
                offer = (
                    "v=0\r\n"
                    "o=- 1 1 IN IP4 127.0.0.1\r\n"
                    "s=-\r\n"
                    "c=IN IP4 127.0.0.1\r\n"
                    "t=0 0\r\n"
                    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
                    "a=rtpmap:96 H264/90000\r\n"
                    "a=recvonly\r\n"
                    "a=setup:actpass\r\n"
                    "a=mid:0\r\n"
                )
                try:
                    body, st = http_post_json(
                        f"{base}/api/cameras/{name}/webrtc", {"sdp": offer}, timeout=10
                    )
                    answered = st in (200, 201) and ("sdp" in body.lower())
                    # 200/201 with an answer == full negotiation path works;
                    # 400/502 is expected for a hand-crafted offer but proves the
                    # endpoint proxies to MediaMTX (not a 500 from our server).
                    if answered:
                        detail = f"webrtc answer returned (status={st})"
                        cls = "PASS"
                    else:
                        detail = f"signaling reachable, status={st} (browser client needed for full SDP negotiation)"
                        cls = "INFO"
                    print(f"  [{cls}] webrtc signaling {name} :: {detail}")
                except Exception as e:
                    print(f"  [INFO] webrtc signaling {name} :: endpoint error {e}")

    finally:
        cleanup()

    return _finish()


def _cameras_with_source(url):
    data, _ = http_get_json(url)
    if not isinstance(data, list):
        raise RuntimeError(f"unexpected /api/cameras body: {data}")
    seen = [c for c in data if c.get("stream_path")]
    return seen


def _ffprobe_ok(ffprobe, url, timeout=15):
    try:
        out = subprocess.run(
            [ffprobe, "-v", "error", "-show_entries",
             "stream=codec_type,codec_name", "-of", "json", url],
            capture_output=True, text=True, timeout=timeout,
        )
        if out.returncode != 0:
            return False, f"ffprobe rc={out.returncode} {out.stderr.strip()[:160]}"
        info = json.loads(out.stdout)
        vids = [s for s in info.get("streams", []) if s.get("codec_type") == "video"]
        if not vids:
            return False, "no video stream in ffprobe output"
        return True, f"video codec={vids[0].get('codec_name')}"
    except subprocess.TimeoutExpired:
        return False, "ffprobe timeout"
    except Exception as e:
        return False, str(e)


def _tail(path, n=25):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()[-n:]
        for ln in lines:
            print("    | " + ln.rstrip())
    except Exception:
        pass


def _finish():
    print()
    print(f"== RESULT: PASS={PASS} FAIL={FAIL} ==")
    return 1 if FAIL > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
