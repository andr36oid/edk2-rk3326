#!/usr/bin/env python3
"""Run QEMU headless with the serial console on a pipe, type keys when given
text shows up, stop at a timeout.
  qemu-run.py LOG TIMEOUT [--when TEXT --send KEYS]... -- qemu-system-aarch64 ...
Exit code: QEMU's, or 124 on timeout."""
import os, subprocess, sys, threading, time

def main():
    log, timeout = sys.argv[1], float(sys.argv[2])
    args = sys.argv[3:]
    steps = []
    while args and args[0] != "--":
        if args[0] == "--when" and len(args) >= 4 and args[2] == "--send":
            steps.append((args[1], args[3].encode().decode("unicode_escape")))
            args = args[4:]
        else:
            sys.exit("bad arguments near %r" % args[0])
    cmd = args[1:]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = bytearray()
    lock = threading.Lock()

    def reader():
        while True:
            b = p.stdout.read1(4096) if hasattr(p.stdout, "read1") else p.stdout.read(1)
            if not b:
                break
            with lock:
                out.extend(b)

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    start, seen = time.time(), 0
    code = None
    while time.time() - start < timeout:
        code = p.poll()
        if code is not None:
            break
        if steps:
            with lock:
                text = out[seen:].decode("latin-1")
            i = text.find(steps[0][0])
            if i >= 0:
                with lock:
                    seen += i + len(steps[0][0])
                time.sleep(0.3)
                p.stdin.write(steps[0][1].encode())
                p.stdin.flush()
                steps.pop(0)
        time.sleep(0.05)
    if code is None:
        p.kill()
        p.wait()
        code = 124
    t.join(2)
    with open(log, "wb") as f:
        f.write(out)
    if steps:
        print("qemu-run: never saw %r" % steps[0][0])
    sys.exit(code)

main()
