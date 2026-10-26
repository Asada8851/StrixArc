#!/usr/bin/env python3
import sys
import base64
import subprocess

def run_ps(cmd: str, host: str = os.environ.get("TARGET_HOST", "user@localhost")):
    b64 = base64.b64encode(cmd.encode("utf-16le")).decode("ascii")
    proc = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", host, f"powershell -NoProfile -EncodedCommand {b64}"],
        capture_output=True
    )
    # decode with cp866 fallback
    try:
        out = proc.stdout.decode("utf-8")
    except UnicodeDecodeError:
        try:
            out = proc.stdout.decode("cp866", errors="replace")
        except Exception:
            out = proc.stdout.decode("latin1", errors="replace")
            
    try:
        err = proc.stderr.decode("utf-8")
    except UnicodeDecodeError:
        try:
            err = proc.stderr.decode("cp866", errors="replace")
        except Exception:
            err = proc.stderr.decode("latin1", errors="replace")
            
    return proc.returncode, out, err

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: win_exec.py <powershell-command>")
        sys.exit(1)
    code, out, err = run_ps(" ".join(sys.argv[1:]))
    if out:
        print(out)
    if err and "WARNING" not in err:
        print("STDERR:", err, file=sys.stderr)
    sys.exit(code)
