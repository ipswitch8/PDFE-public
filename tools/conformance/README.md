# Independent conformance checker (veraPDF)

meta-audit follow-up **F14**, gaps **PDF-10** / **PDF-17**.

## Why this directory exists

Before F14 the only independent PDF/A checker in the tree was reached through
`std::getenv("VERAPDF_JAR")` in `tests/test_fidelity_r3_19_pdfa_independent.cpp`,
and the test `GTEST_SKIP`ped when the variable was unset. `VERAPDF_JAR` was set
nowhere in the repository or in CI, so **the checker had never run** — PDF/A
conformance was graded solely by PDFE's own validator against PDFE's own
converter, a closed loop that cannot detect a false conformance claim.

F14 replaced that opt-in with a **deterministic, variable-free** resolution.

## The documented default location

`tests/support/ConformanceChecker.cpp` resolves the checker in this fixed order,
consulting **no checker-specific environment variable**:

1. `<repo>/tools/conformance/verapdf/` — this directory, populated by the script
   below. **Searched first.**
2. Conventional system install roots, in order:
   `C:/verapdf`, `C:/Program Files/verapdf`, `C:/Program Files (x86)/verapdf`,
   `%USERPROFILE%/verapdf` (Windows); `/opt/verapdf`, `/usr/local/verapdf`,
   `/usr/share/verapdf`, `$HOME/verapdf` (POSIX).

Within a root it takes `bin/cli-*.jar` (current layout) or
`greenfield/verapdf.jar` (legacy layout), highest version first, chosen from a
sorted listing so the result never varies between runs.

Java is resolved separately and equally deterministically: `java` on `PATH`,
then `JAVA_HOME/bin/java`, then the conventional JDK roots.

The resolution is **total**: it returns either an available checker with a path
that exists, or an unavailable result carrying a concrete reason and the named
prerequisite. There is no indeterminate third state, which is what lets the
tests assert the outcome instead of skipping on it.

## Provisioning

```
python tools/conformance/fetch-verapdf.py
```

Downloads the official greenfield installer from `software.verapdf.org`, runs it
headless through an IzPack auto-install descriptor, and leaves the CLI jar at
`tools/conformance/verapdf/bin/cli-<version>.jar`.

Requirements: Python 3.9+, a JRE/JDK 11+, outbound HTTPS.

The installed tree is **gitignored** (`.gitignore` here) — a ~33 MB third-party
Java distribution does not belong in the history. The script is committed, so the
installation is reproducible on any machine.

## Result record

Every run of the conformance tests rewrites a result-or-reason record to

* `.claude/pipelines/meta-audit-followup/gate-artifacts/phase-14/conformance-checker-record.json`
  (gate artifact), and
* `docs/audit/meta-audit-2026-07/followup/phase-14/conformance-checker-record.json`
  (durable copy — `.claude/` is gitignored).

It records the resolution (including every location searched), and either the
real verdicts or the concrete reason the checker could not run plus the named
prerequisite.

## Exit-code contract

veraPDF CLI: `0` = conforms, `1` = does not conform, `>1` = tool error. The
integration treats only `0`/`1` as a verdict; anything else is reported as "did
not run" with the captured output, so a broken tool can never read as a pass.
