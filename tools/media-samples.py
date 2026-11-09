#!/usr/bin/env python3
"""Fetch the streams the media tests play (once, into a cache): YouTube's first video, "Me at the
zoo" (19 s), as one progressive MP4 (itag 18) and as the separate DASH streams a Media Source
Extensions player appends: H.264 (133) and AAC (140) in fragmented MP4, VP9 (278) and Opus (249) in
WebM. YouTube is asked as user/desktop/media/ytresolve.c asks it (its Android app's API)."""
import json
import pathlib
import sys
import urllib.request

VIDEO = "jNQXAC9IVRw"
ITAGS = {18: "yt-18.mp4", 133: "yt-133.mp4", 140: "yt-140.mp4", 278: "yt-278.webm", 249: "yt-249.webm"}
UA = "com.google.android.youtube/20.10.38 (Linux; U; Android 14) gzip"

cache = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build/tests/media-samples")
cache.mkdir(parents=True, exist_ok=True)
marker = cache / "complete"
if marker.exists():
    sys.exit(0)

body = json.dumps({"context": {"client": {"clientName": "ANDROID", "clientVersion": "20.10.38",
                                          "androidSdkVersion": 34, "osName": "Android", "osVersion": "14",
                                          "hl": "en"}},
                   "videoId": VIDEO, "contentCheckOk": True, "racyCheckOk": True}).encode()
request = urllib.request.Request("https://www.youtube.com/youtubei/v1/player?prettyPrint=false", body,
                                 {"Content-Type": "application/json", "User-Agent": UA})
answer = json.load(urllib.request.urlopen(request, timeout=30))
if answer.get("playabilityStatus", {}).get("status") != "OK":
    raise SystemExit("host: YouTube will not give the test video: %s" % answer.get("playabilityStatus"))
formats = answer["streamingData"].get("formats", []) + answer["streamingData"].get("adaptiveFormats", [])
for itag, name in ITAGS.items():
    chosen = next((f for f in formats if f.get("itag") == itag and "url" in f), None)
    if not chosen:
        raise SystemExit(f"host: YouTube has no stream {itag} for the test video")
    print(f"host: fetching {name} ({chosen['mimeType']})", flush=True)
    data = urllib.request.urlopen(urllib.request.Request(chosen["url"], headers={"User-Agent": UA}), timeout=60).read()
    (cache / name).write_bytes(data)
marker.write_text("ok\n")
