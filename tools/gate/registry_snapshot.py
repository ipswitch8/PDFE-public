# -*- coding: utf-8 -*-
"""Snapshot the HKCU registry hives the test suite is capable of touching.

Meta-audit follow-up F28, acceptance criterion 3: the disposition record must
carry a VERIFIED statement of registry state after a full sharded run, not an
asserted one. This produces the before/after evidence.

Usage:
    python tools/gate/registry_snapshot.py <out.json>
    python tools/gate/registry_snapshot.py --diff <before.json> <after.json>

Windows-only by nature; on other platforms it emits an empty snapshot with a
``platform`` marker so the caller can tell "clean" from "not applicable".
"""
from __future__ import annotations

import json
import sys

# Every organization name the C++ test suite is known to install via
# QCoreApplication::setOrganizationName, plus the product's own hive.
HIVES = ["PDFE", "PDFE-Test", "PDFE_ImageStampTest", "OcrLangTest"]


def _walk(key, path):
    import winreg

    out = {}
    try:
        with winreg.OpenKey(key, path) as h:
            n_sub, n_val, _ = winreg.QueryInfoKey(h)
            vals = {}
            for i in range(n_val):
                name, data, _ = winreg.EnumValue(h, i)
                vals[name] = repr(data)
            out["_values"] = vals
            for i in range(n_sub):
                sub = winreg.EnumKey(h, i)
                out[sub] = _walk(key, path + "\\" + sub)
    except FileNotFoundError:
        return None
    except OSError as exc:  # pragma: no cover - permission edge
        return {"_error": str(exc)}
    return out


def snapshot() -> dict:
    if not sys.platform.startswith("win"):
        return {"platform": sys.platform, "hives": {}}
    import winreg

    return {
        "platform": "win32",
        "hives": {
            h: _walk(winreg.HKEY_CURRENT_USER, "Software\\" + h) for h in HIVES
        },
    }


def main(argv: list[str]) -> int:
    if len(argv) == 4 and argv[1] == "--diff":
        with open(argv[2], encoding="utf-8") as f:
            before = json.load(f)
        with open(argv[3], encoding="utf-8") as f:
            after = json.load(f)
        rc = 0
        for hive in HIVES:
            b = before["hives"].get(hive)
            a = after["hives"].get(hive)
            if b == a:
                state = "ABSENT" if a is None else "present"
                print(f"UNCHANGED  {hive:24s} ({state})")
            else:
                rc = 1
                print(f"CHANGED    {hive:24s} before={_terse(b)} after={_terse(a)}")
        return rc
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1], "w", encoding="utf-8") as f:
        json.dump(snapshot(), f, indent=2, sort_keys=True)
    print(f"wrote {argv[1]}")
    return 0


def _terse(node) -> str:
    if node is None:
        return "ABSENT"
    subs = sorted(k for k in node if k != "_values")
    return "subkeys=" + (",".join(subs) if subs else "<none>")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
