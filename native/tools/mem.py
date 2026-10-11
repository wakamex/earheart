#!/usr/bin/env python3
"""Memory of a process and its descendants on Linux, as proportional set size (PSS): private
pages in full and shared pages divided among their users, so the figures add up across processes.

    mem.py PID            one table: each process and the total, in MiB
    mem.py PID --watch N  the total once a second for N seconds, then its peak
"""
import os, re, sys, time

def children(pid):
    out = []
    for t in os.listdir(f"/proc/{pid}/task"):
        try:
            out += [int(c) for c in open(f"/proc/{pid}/task/{t}/children").read().split()]
        except OSError:
            pass
    return out

def tree(pid):
    pids, queue = [], [pid]
    while queue:
        p = queue.pop()
        try:
            queue += children(p)
            pids.append(p)
        except OSError:
            pass
    return pids

def pss(pid):
    try:
        for line in open(f"/proc/{pid}/smaps_rollup"):
            if line.startswith("Pss:"):
                return int(line.split()[1]) / 1024
    except OSError:
        pass
    return 0.0

def label(pid):
    try:
        text = open(f"/proc/{pid}/cmdline").read().replace("\0", " ")
        comm = open(f"/proc/{pid}/comm").read().strip()
    except OSError:
        return "?"
    # Chromium's child processes say what they are in their arguments.
    found = re.search(r"--type=(\S+)", text)
    role = re.search(r"--earheart-role=(\S+)", text)
    sub = re.search(r"--utility-sub-type=(\S+)", text)
    kind = found.group(1) if found else "main"
    return " ".join(x for x in (comm, kind, (role or sub).group(1) if (role or sub) else None) if x)

def main():
    root = int(sys.argv[1])
    if "--watch" in sys.argv:
        peak = 0.0
        for _ in range(int(sys.argv[sys.argv.index("--watch") + 1])):
            total = sum(pss(p) for p in tree(root))
            peak = max(peak, total)
            print(f"{total:.0f}", flush=True)
            time.sleep(1)
        print(f"peak {peak:.0f} MiB")
        return
    rows = [(pss(p), p, label(p)) for p in tree(root)]
    for mib, pid, name in sorted(rows, reverse=True):
        if mib >= 1:
            print(f"{mib:8.0f}  {pid:>8}  {name}")
    print(f"{sum(r[0] for r in rows):8.0f}  total MiB over {len(rows)} processes")

main()
