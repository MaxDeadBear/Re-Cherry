"""Drive tools/ida/export_all.py through the IDA MCP server until every
function is exported. Usage: python export_all_loop.py [port]. Without a port
it uses the IDA instance whose database is assets/default.xex.i64 of this
repository, from the MCP server's instance files."""
import glob
import json
import os
import sys
import time
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def find_port():
    want = os.path.normcase(os.path.join(REPO, "assets", "default.xex.i64"))
    pattern = os.path.join(os.environ["APPDATA"], "Hex-Rays", "IDA Pro", "mcp", "instances", "instance_*.json")
    for path in glob.glob(pattern):
        with open(path, encoding="utf-8") as f:
            inst = json.load(f)
        if os.path.normcase(inst.get("idb_path", "")) == want:
            return inst["port"]
    sys.exit(f"no IDA instance has {want} open")


PORT = int(sys.argv[1]) if len(sys.argv) > 1 else find_port()
print(f"IDA MCP port {PORT}", flush=True)
SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "export_all.py").replace("\\", "/")


def call(tool, args):
    req = urllib.request.Request(
        f"http://127.0.0.1:{PORT}/mcp",
        data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                         "params": {"name": tool, "arguments": args}}).encode(),
        headers={"Content-Type": "application/json",
                 "Accept": "application/json, text/event-stream"})
    resp = json.loads(urllib.request.urlopen(req, timeout=600).read())
    if "error" in resp:
        raise RuntimeError(resp["error"])
    return "".join(c.get("text", "") for c in resp["result"].get("content", []))


while True:
    try:
        text = call("py_exec_file", {"file_path": SCRIPT})
    except Exception as e:  # noqa: BLE001 -- IDA busy or restarting: retry
        print(f"call failed: {e}; retrying in 10 s", flush=True)
        time.sleep(10)
        continue
    # The MCP result is JSON with the script's stdout, whose last line is the
    # export status.
    status = None
    try:
        stdout = json.loads(text).get("stdout", "")
        status = json.loads(stdout.strip().splitlines()[-1])
    except (ValueError, IndexError, AttributeError):
        pass
    print(time.strftime("%H:%M:%S"), status or text[:300], flush=True)
    if status and status["remaining"] == 0:
        break
    if not status:
        time.sleep(10)
