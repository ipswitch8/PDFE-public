#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SHA-pin every external `uses:` step in .github/workflows and .github/actions.

GitHub #147. A `uses: owner/repo@v4` reference resolves the tag at run time, so
whoever controls that tag controls what executes in CI. Tags are mutable and are
routinely re-pointed; `lukka/run-vcpkg@v11` in this repo was not even a tag but a
BRANCH, which has no tag-protection semantics whatsoever.

Pinning to a 40-hex commit SHA makes the reference immutable. The trailing
`# vX.Y.Z` comment is not decoration -- without it nobody can tell what version a
SHA is, and the pin becomes unmaintainable.

Local (`./.github/actions/...`) references are deliberately NOT pinned: they
resolve inside this repository at the commit already being checked out, so they
carry no external trust and a SHA would be meaningless.

Usage:
    python tools/gate/pin_workflow_actions.py            # apply
    python tools/gate/pin_workflow_actions.py --check    # exit 1 if unpinned
"""

import argparse
import io
import os
import re
import sys

# repo -> (commit sha, version tag the sha corresponds to)
# Resolved from the GitHub API; the version is the tag POINTING AT that sha, not
# the floating major alias that was there before.
PINS = {
    "actions/checkout":          ("11d5960a326750d5838078e36cf38b85af677262", "v4.4.0"),
    "actions/upload-artifact":   ("ea165f8d65b6e75b540449e92b4886f43607fa02", "v4.6.2"),
    "actions/download-artifact": ("d3f86a106a0bac45b974a628896c90dbdf5c8093", "v4.3.0"),
    "actions/github-script":     ("f28e40c7f34bde8b3046d885e986cb6290c5673b", "v7.1.0"),
    "actions/setup-python":      ("a26af69be951a213d495a4c3e4e4022e16d87065", "v5.6.0"),
    "actions/cache":             ("0057852bfaa89a56745cba8c7296529d2fc39830", "v4.3.0"),
    # v11 was a BRANCH, not a tag. Its head is identical to the v11.6 tag, so the
    # pin is both current and tag-corroborated.
    "lukka/run-vcpkg":           ("b1a0dd252f06b9e25b3c022a9a03bd7a427fb6a2", "v11.6"),
    "ilammy/msvc-dev-cmd":       ("0b201ec74fa43914dc39ae48a89fd1d8cb592756", "v1.13.0"),
}

SHA_RE = re.compile(r"^[0-9a-f]{40}$")
# Captures indentation, the `uses:` keyword, the ref, and any trailing comment.
USES_RE = re.compile(r"^(?P<pre>\s*(?:-\s*)?uses:\s*)(?P<ref>\S+)(?P<rest>.*)$")

SEARCH_DIRS = (
    os.path.join(".github", "workflows"),
    os.path.join(".github", "actions"),
)


def yaml_files():
    for d in SEARCH_DIRS:
        if not os.path.isdir(d):
            continue
        for root, _dirs, files in os.walk(d):
            for f in files:
                if f.endswith((".yml", ".yaml")):
                    yield os.path.join(root, f)


def classify(ref):
    """Return ('local'|'pinned'|'unpinned'|'unknown', repo, current_ref)."""
    if ref.startswith("./") or ref.startswith("."):
        return "local", ref, ""
    if "@" not in ref:
        return "unknown", ref, ""
    repo, _, at = ref.partition("@")
    if SHA_RE.match(at):
        return "pinned", repo, at
    if repo in PINS:
        return "unpinned", repo, at
    return "unknown", repo, at


def process(path, apply_changes):
    # newline="" preserves the file's existing CRLF/LF endings exactly; this repo
    # has core.autocrlf=true, so rewriting them would make the diff meaningless.
    with io.open(path, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()

    out_lines = []
    changed = 0
    remaining = []
    for line in text.splitlines(keepends=True):
        stripped = line.rstrip("\r\n")
        eol = line[len(stripped):]
        m = USES_RE.match(stripped)
        if not m:
            out_lines.append(line)
            continue

        kind, repo, at = classify(m.group("ref"))
        if kind == "unpinned":
            sha, ver = PINS[repo]
            rest = m.group("rest")
            # Drop any pre-existing trailing comment; it described the old ref.
            rest = re.sub(r"\s*#.*$", "", rest)
            new = "%s%s@%s%s  # %s" % (m.group("pre"), repo, sha, rest, ver)
            out_lines.append(new + eol)
            changed += 1
        else:
            if kind == "unknown":
                remaining.append((path, stripped.strip()))
            out_lines.append(line)

    if changed and apply_changes:
        with io.open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write("".join(out_lines))
    return changed, remaining


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="report only; exit 1 if any external uses: is unpinned")
    args = ap.parse_args()

    total_changed = 0
    unknown = []
    files = sorted(yaml_files())
    for path in files:
        changed, rem = process(path, apply_changes=not args.check)
        if changed:
            print("%s: %d" % (path.replace(os.sep, "/"), changed))
        total_changed += changed
        unknown.extend(rem)

    if unknown:
        print("\nUNKNOWN external actions (no pin defined -- add one to PINS):")
        for path, line in unknown:
            print("  %s: %s" % (path.replace(os.sep, "/"), line))

    if args.check:
        if total_changed or unknown:
            print("\nFAIL: %d unpinned + %d unknown external `uses:` reference(s)."
                  % (total_changed, len(unknown)))
            return 1
        print("OK: every external `uses:` reference is SHA-pinned (%d files)."
              % len(files))
        return 0

    print("\nPinned %d `uses:` reference(s) across %d file(s)."
          % (total_changed, len(files)))
    return 1 if unknown else 0


if __name__ == "__main__":
    sys.exit(main())
