"""Compile the real HTTP/OAuth/MCP code against a bounded host transport fixture."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument("--cjson", type=Path,
                    default=root / "managed_components" / "espressif__cjson" / "cJSON")
args = parser.parse_args()
if not (args.cjson / "cJSON.c").is_file():
    parser.error("--cjson must contain cJSON.c and cJSON.h (idf.py build supplies it)")

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / ("test_dial_auth.exe" if os.name == "nt" else "test_dial_auth")
    subprocess.run([
        *shlex.split(os.environ.get("CC", "cc")), "-std=gnu11",
        "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-I", str(root / "test" / "stubs"), "-I", str(args.cjson),
        "-I", str(root / "components" / "dial_oauth"),
        "-I", str(root / "components" / "dial_mcp"),
        str(root / "test" / "test_dial_auth.c"),
        str(root / "components" / "dial_oauth" / "dial_http.c"),
        str(root / "components" / "dial_oauth" / "dial_oauth.c"),
        str(root / "components" / "dial_mcp" / "dial_mcp.c"),
        str(args.cjson / "cJSON.c"), "-lm", "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
