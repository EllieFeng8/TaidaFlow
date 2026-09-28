#!/usr/bin/env python3
"""w2-060 D3: exercise every RESTManager route through nginx (http://<host>/api/...).

  python -B docs/evidence/w2-060/tools/rest_api_check.py --hosts 127.0.0.1,192.168.0.125
         --settings-db build/runtime-cwd/settings.sqlite [--rest-port 18080] [--nginx-port 80]
         [--out docs/evidence/w2-060/30-rest-api.txt]

Run it while the DEVELOPMENT desktop app runs with the simulator profile (scripts\\run-desktop.ps1
-DeviceProfile simulator, working directory build\\runtime-cwd) and nginx (scripts\\nginx-start.ps1)
is up. Only test data is touched:

* --settings-db must be a settings.sqlite below <taidaflow>\\build\\ (the development working
  directory). Every PUT is checked in that file (read-only sqlite3 connection): the new value must
  be there after the PUT and the original value after the restore - so the write went to this test
  file and nowhere else.
* PUT /api/settings/sensors, /api/settings/frequency, /api/modbus/mode: original value read first
  (GET), a test value written, read back, then the original written back and read back again.

Checks: every GET route on every host (status, Content-Type, CORS header, body summary, time),
OPTIONS preflight on every OPTIONS route, invalid PUT bodies -> 400 without change, the internal
REST port answers on 127.0.0.1 and is refused on every non-loopback host. Exit 0 = all checks
passed, 1 = at least one failed, 2 = bad arguments / refused (settings db not below build\\).
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import socket
import sqlite3
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]          # taidaflow/
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))   # never a proxy

lines: list[str] = []
fails = 0


def note(text: str) -> None:
    lines.append(text)
    print(text, flush=True)


def check(what: str, ok: bool, detail: str = "") -> bool:
    global fails
    if not ok:
        fails += 1
    note(f"[{'PASS' if ok else 'FAIL'}] {what}{' - ' + detail if detail else ''}")
    return ok


# Header names are compared in lower case: the app (QHttpServer) sends them in lower case
# (e.g. "access-control-allow-origin"), nginx its own in mixed case; HTTP header names are
# case-insensitive.
def request(method: str, url: str, body: bytes | None = None, headers: dict | None = None,
            timeout: float = 60.0):
    req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    t0 = time.perf_counter()
    try:
        with OPENER.open(req, timeout=timeout) as r:
            data = r.read()
            return r.status, {k.lower(): v for k, v in r.headers.items()}, data, (time.perf_counter() - t0) * 1000
    except urllib.error.HTTPError as e:
        data = e.read()
        return e.code, {k.lower(): v for k, v in e.headers.items()}, data, (time.perf_counter() - t0) * 1000


def summary(data: bytes) -> str:
    try:
        v = json.loads(data.decode("utf-8"))
    except Exception:
        return f"{len(data)} bytes non-JSON: {data[:120]!r}"
    if isinstance(v, list):
        first = json.dumps(v[0], ensure_ascii=False)[:160] if v else ""
        return f"JSON array, {len(v)} item(s), {len(data)} bytes{'; first ' + first if first else ''}"
    if isinstance(v, dict) and isinstance(v.get("items"), list):
        meta = {k: v[k] for k in v if k != "items"}
        return f"JSON object {json.dumps(meta)}, items {len(v['items'])}, {len(data)} bytes"
    return f"JSON {json.dumps(v, ensure_ascii=False)[:200]}"


def db_value(db: Path, sql: str, args=()):
    con = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True, timeout=5)
    try:
        return con.execute(sql, args).fetchall()
    finally:
        con.close()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hosts", required=True)
    ap.add_argument("--settings-db", required=True)
    ap.add_argument("--rest-port", type=int, default=18080)
    ap.add_argument("--nginx-port", type=int, default=80)
    ap.add_argument("--out", default="")
    a = ap.parse_args()

    db = Path(a.settings_db)
    if not db.is_absolute():
        db = ROOT / db
    db = db.resolve()
    build = (ROOT / "build").resolve()
    if build not in db.parents or not db.is_file():
        print(f"REFUSED: {db} is not an existing settings.sqlite below {build} (test data only)")
        return 2
    hosts = [h.strip() for h in a.hosts.split(",") if h.strip()]
    note(f"=== w2-060 REST API check {dt.datetime.now().isoformat(timespec='seconds')}")
    note(f"hosts {hosts}, nginx port {a.nginx_port}, REST port {a.rest_port}, settings db (test data) {db}")

    now = int(time.time())
    frm, to = now - 3600, now
    iso_from = dt.datetime.fromtimestamp(frm).strftime("%Y-%m-%dT%H:%M:%S")
    iso_to = dt.datetime.fromtimestamp(to).strftime("%Y-%m-%dT%H:%M:%S")
    gets = [
        ("/api/", "status route '/' (nginx maps /api/ -> /)"),
        ("/api/settings/sensors", ""),
        ("/api/settings/frequency", ""),
        ("/api/modbus/mode", ""),
        (f"/api/sensor/range?from={frm}&to={to}", "last hour, epoch seconds"),
        (f"/api/holding/range?from={frm}&to={to}", "last hour, epoch seconds"),
        ("/api/device/sn", ""),
        ("/api/sensor/last", ""),
        ("/api/holding/last", ""),
        (f"/api/sensor/rangeDateTime?from={iso_from}&to={iso_to}", "last hour, ISO local time"),
        (f"/api/sensor/rangeDateTimePage?from={iso_from}&to={iso_to}&page=1&pageSize=5", ""),
        (f"/api/holding/rangeDateTime?from={iso_from}&to={iso_to}", ""),
        (f"/api/holding/rangeDateTimePage?from={iso_from}&to={iso_to}&page=1&pageSize=5", ""),
    ]
    # holding_register is never written by the current Manager (saveSensorData without holdings),
    # so the holding "last" route answers RESTManager's 404 {"ok":false,"error":"no data"}.
    expect_404 = {"/api/holding/last"}

    for h in hosts:
        base = f"http://{h}" + ("" if a.nginx_port == 80 else f":{a.nginx_port}")
        note(f"--- GET routes via nginx {base}")
        for path, why in gets:
            st, hd, data, ms = request("GET", base + path)
            key = path.split("?")[0]
            want = 404 if key in expect_404 and st == 404 else 200
            ok = st == want and hd.get("content-type", "").startswith("application/json") \
                and hd.get("access-control-allow-origin") == "*" and hd.get("cache-control") == "no-store"
            check(f"GET {base}{path}", ok,
                  f"{st} {hd.get('content-type')} ACAO={hd.get('access-control-allow-origin')} "
                  f"Cache-Control={hd.get('cache-control')} {ms:.0f} ms; {summary(data)}"
                  + (f" ({why})" if why else ""))
        # 400 path (still answered by the app, through nginx)
        st, hd, data, ms = request("GET", base + "/api/sensor/range?from=abc&to=1")
        check(f"GET {base}/api/sensor/range?from=abc -> 400 JSON error", st == 400, f"{st} {summary(data)}")
        st, hd, data, ms = request("GET", base + "/api/does-not-exist")
        check(f"GET {base}/api/does-not-exist -> 404 (no such route)", st == 404, f"{st}")

    note("--- OPTIONS preflight (CORS) via nginx")
    opt_paths = ["/api/settings/sensors", "/api/settings/frequency", "/api/modbus/mode", "/api/sensor/range",
                 "/api/holding/range", "/api/device/sn", "/api/sensor/last", "/api/holding/last",
                 "/api/sensor/rangeDateTime", "/api/sensor/rangeDateTimePage", "/api/holding/rangeDateTime",
                 "/api/holding/rangeDateTimePage"]
    for h in hosts:
        base = f"http://{h}" + ("" if a.nginx_port == 80 else f":{a.nginx_port}")
        for path in opt_paths:
            st, hd, data, ms = request("OPTIONS", base + path, headers={
                "Origin": "http://example.invalid", "Access-Control-Request-Method": "PUT",
                "Access-Control-Request-Headers": "Content-Type"})
            ok = st == 200 and hd.get("access-control-allow-origin") == "*" \
                and "PUT" in hd.get("access-control-allow-methods", "") \
                and "Content-Type" in hd.get("access-control-allow-headers", "")
            check(f"OPTIONS {base}{path}", ok,
                  f"{st} ACAO={hd.get('access-control-allow-origin')} ACAM={hd.get('access-control-allow-methods')} "
                  f"ACAH={hd.get('access-control-allow-headers')}")

    # ---- PUT: write test value, read back, restore, read back (test data only) -----------------
    put_host = hosts[-1]   # the LAN address when given: the write goes through nginx from the LAN side
    read_host = hosts[0]
    pb = f"http://{put_host}" + ("" if a.nginx_port == 80 else f":{a.nginx_port}")
    rb = f"http://{read_host}" + ("" if a.nginx_port == 80 else f":{a.nginx_port}")
    js = {"Content-Type": "application/json"}

    def get_json(path):
        st, hd, data, ms = request("GET", rb + path)
        return st, json.loads(data.decode("utf-8"))

    note(f"--- PUT /api/settings/frequency (write via {pb}, read via {rb}; db {db})")
    st, orig = get_json("/api/settings/frequency")
    orig_f = orig.get("read_frequency")
    db_before = db_value(db, "SELECT value FROM app_settings WHERE key='read_frequency'")
    note(f"  before: GET {orig} ; sqlite app_settings.read_frequency = {db_before}")
    test_f = 2345 if orig_f != 2345 else 2346
    st, hd, data, ms = request("PUT", pb + "/api/settings/frequency", json.dumps({"read_frequency": test_f}).encode(), js)
    check(f"PUT read_frequency={test_f}", st == 200, f"{st} {summary(data)}")
    st, now_v = get_json("/api/settings/frequency")
    dbv = db_value(db, "SELECT value FROM app_settings WHERE key='read_frequency'")
    check("read back test value (GET + test settings.sqlite)", now_v.get("read_frequency") == test_f and str(dbv[0][0]) == str(test_f),
          f"GET {now_v}, sqlite {dbv}")
    st, hd, data, ms = request("PUT", pb + "/api/settings/frequency", b'{"read_frequency": "x"}', js)
    check("PUT invalid read_frequency -> 400", st == 400, f"{st} {summary(data)}")
    st, hd, data, ms = request("PUT", pb + "/api/settings/frequency", json.dumps({"read_frequency": orig_f}).encode(), js)
    check(f"PUT restore read_frequency={orig_f}", st == 200, f"{st} {summary(data)}")
    st, back = get_json("/api/settings/frequency")
    dbv = db_value(db, "SELECT value FROM app_settings WHERE key='read_frequency'")
    check("restored (GET + sqlite equal to before)", back == orig and dbv == db_before, f"GET {back}, sqlite {dbv}")

    note("--- PUT /api/settings/sensors")
    st, orig_s = get_json("/api/settings/sensors")
    db_before = db_value(db, "SELECT sensor_key, sensor_name FROM sensor_config ORDER BY sensor_key")
    note(f"  before: {len(orig_s)} entries, s1={[o for o in orig_s if o.get('key') == 's1']}; sqlite rows {len(db_before)}")
    st, hd, data, ms = request("PUT", pb + "/api/settings/sensors",
                               json.dumps([{"key": "s1", "name": "w2060-test-s1"}]).encode(), js)
    check("PUT sensors [{s1: w2060-test-s1}]", st == 200, f"{st} {summary(data)}")
    st, now_s = get_json("/api/settings/sensors")
    others_same = {o["key"]: o["name"] for o in now_s if o["key"] != "s1"} == {o["key"]: o["name"] for o in orig_s if o["key"] != "s1"}
    dbv = db_value(db, "SELECT sensor_name FROM sensor_config WHERE sensor_key='s1'")
    check("read back: s1 renamed, other keys unchanged (merge), test settings.sqlite has it",
          any(o["key"] == "s1" and o["name"] == "w2060-test-s1" for o in now_s) and others_same and dbv == [("w2060-test-s1",)],
          f"{len(now_s)} entries, sqlite s1 {dbv}")
    st, hd, data, ms = request("PUT", pb + "/api/settings/sensors", b'{"not": "an array"}', js)
    check("PUT sensors invalid body -> 400", st == 400, f"{st} {summary(data)}")
    st, hd, data, ms = request("PUT", pb + "/api/settings/sensors", json.dumps(orig_s).encode(), js)
    check("PUT restore original sensor list", st == 200, f"{st} {summary(data)}")
    st, back_s = get_json("/api/settings/sensors")
    dbv = db_value(db, "SELECT sensor_key, sensor_name FROM sensor_config ORDER BY sensor_key")
    check("restored (GET + sqlite equal to before)", back_s == orig_s and dbv == db_before, f"{len(back_s)} entries, sqlite rows {len(dbv)}")

    note("--- PUT /api/modbus/mode (RESTManager in-memory value; no Core consumer)")
    st, orig_m = get_json("/api/modbus/mode")
    note(f"  before: {orig_m}")
    test_m = "standalone" if orig_m.get("mode") != "standalone" else "network"
    st, hd, data, ms = request("PUT", pb + "/api/modbus/mode", json.dumps({"mode": test_m}).encode(), js)
    check(f"PUT mode={test_m}", st == 200, f"{st} {summary(data)}")
    st, now_m = get_json("/api/modbus/mode")
    check("read back test mode", now_m.get("mode") == test_m, f"{now_m}")
    st, hd, data, ms = request("PUT", pb + "/api/modbus/mode", b'{"mode": "auto"}', js)
    check("PUT mode=auto -> 400", st == 400, f"{st} {summary(data)}")
    st, hd, data, ms = request("PUT", pb + "/api/modbus/mode", json.dumps({"mode": orig_m.get("mode")}).encode(), js)
    check(f"PUT restore mode={orig_m.get('mode')}", st == 200, f"{st} {summary(data)}")
    st, back_m = get_json("/api/modbus/mode")
    check("restored", back_m == orig_m, f"{back_m}")

    note("--- internal REST port")
    st, hd, data, ms = request("GET", f"http://127.0.0.1:{a.rest_port}/")
    check(f"http://127.0.0.1:{a.rest_port}/ (loopback, direct) -> 200", st == 200, f"{st} {summary(data)}")
    for h in hosts:
        if h.startswith("127."):
            continue
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3)
        try:
            s.connect((h, a.rest_port))
            res = "CONNECTED"
        except OSError as e:
            res = f"{type(e).__name__}: {e}"
        finally:
            s.close()
        check(f"{h}:{a.rest_port} (LAN address, direct) refused - only reachable through nginx",
              res != "CONNECTED", res)

    note(f"=== {fails} check(s) failed")
    if a.out:
        out = Path(a.out)
        if not out.is_absolute():
            out = ROOT / out
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
