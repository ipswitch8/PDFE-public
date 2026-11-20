#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Exit 0 if the probe text on stdin hits the needle inventory, 1 if not.

Used by set-hygiene-probe-secret.sh before it stores PDFE_HYGIENE_PROBE: a
probe the scanner cannot see would make check_tracker_hygiene.py's control
fail in CI. Prints only the verdict, never the probe or a needle label.

    printf '...' | python3 probe-hits-needle.py <repo-root>
"""
import importlib.util
import os
import sys

for _n in ("stdout", "stderr"):
    _s = getattr(sys, _n, None)
    if _s is not None and hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")


def main(argv):
    root = argv[1] if len(argv) > 1 else "."
    spec = importlib.util.spec_from_file_location(
        "csr", os.path.join(root, "tools", "gate", "check_scrub_regressions.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    needles, err = m.load_all_needles()
    if err:
        print("needle inventory unreadable: %s" % err)
        return 2
    probe = sys.stdin.read().strip()
    if len(probe) < 4:
        print("probe absent/short")
        return 1
    hit = bool(m.scan_text("x %s y" % probe, needles))
    print("probe hits the needle inventory: %s" % ("yes" if hit else "NO"))
    return 0 if hit else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
