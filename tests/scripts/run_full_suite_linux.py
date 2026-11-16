# -*- coding: utf-8 -*-
"""Run every pdfe_tests suite in its own process (so one crash or hang cannot
take the rest of the run down), with a per-suite timeout, then merge the JUnit
XML. A suite that crashes or times out gets a synthetic <testcase> with an
<error>, so it can never disappear from the totals.

usage: run_full_suite_linux.py BIN OUTDIR [JOBS]
  e.g. python3 tests/scripts/run_full_suite_linux.py build/bin/Release/pdfe_tests out 3

Environment: SUITES=a,b,c runs only those suites; SUITE_TIMEOUT=<seconds>
(default 1200) caps each suite. Writes OUTDIR/<suite>.{xml,log} and the merged
OUTDIR/merged.xml, and prints a TOTALS line (GitHub #390).
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor

BIN, OUT = sys.argv[1], sys.argv[2]
JOBS = int(sys.argv[3]) if len(sys.argv) > 3 else 3
TIMEOUT = int(os.environ.get("SUITE_TIMEOUT", "1200"))
# Suites documented as long-running get their own budget rather than a false
# timeout (XfaSaveBenchmark: "~35+ min on Release", opt-in via
# PDFE_RUN_SLOW_SAVE_TESTS).
LONG = {"XfaSaveBenchmark": 3600}
ONLY = [x for x in os.environ.get("SUITES", "").split(",") if x]
os.makedirs(OUT, exist_ok=True)
SYSTEM_TMP = tempfile.gettempdir()

env = dict(os.environ, QT_QPA_PLATFORM="offscreen", PDFE_RUN_SLOW_SAVE_TESTS="1")
listing = subprocess.run([BIN, "--gtest_list_tests"], env=env, capture_output=True,
                         text=True, encoding="utf-8", errors="replace").stdout
suites, cases = [], {}
cur = None
for line in listing.splitlines():
    if not line.strip() or line.startswith(" "):
        if cur and line.startswith("  "):
            cases[cur].append(line.strip().split()[0])
        continue
    cur = line.split()[0].rstrip(".")
    suites.append(cur)
    cases[cur] = []


if ONLY:
    suites = [x for x in suites if x in ONLY]


def run(suite):
    safe = suite.replace("/", "_")
    xml = os.path.join(OUT, f"{safe}.xml")
    log = os.path.join(OUT, f"{safe}.log")
    status = "ok"
    # Per-suite settings/data roots: parallel suites otherwise share the real
    # ~/.config/PDFE store (QSettings) and race on it. The font cache stays
    # shared (XDG_CACHE_HOME) so fontconfig is not rebuilt per suite.
    home = os.path.join(OUT, "xdg", safe)
    senv = dict(env, XDG_CONFIG_HOME=os.path.join(home, "config"),
                XDG_DATA_HOME=os.path.join(home, "data"))
    os.makedirs(senv["XDG_CONFIG_HOME"], exist_ok=True)
    os.makedirs(senv["XDG_DATA_HOME"], exist_ok=True)
    # Per-suite temp root, same reason: the search index lives under
    # temp_directory_path()/pdfe_search_indexes, and the sweep/purge suites
    # remove directories there while another suite is building the index for
    # the same fixture (DocumentSearchIndexTest.LazyBuildOnFirstSearch read 0
    # documents). Kept on the system temp filesystem so temp-path semantics
    # do not change.
    tmp = os.path.join(SYSTEM_TMP, "pdfe-suite-tmp-%d" % os.getpid(), safe)
    os.makedirs(tmp, mode=0o700, exist_ok=True)
    senv["TMPDIR"] = tmp
    with io.open(log, "w", encoding="utf-8", errors="replace") as lf:
        try:
            p = subprocess.run([BIN, f"--gtest_filter={suite}.*", f"--gtest_output=xml:{xml}"],
                               env=senv, stdout=lf, stderr=subprocess.STDOUT,
                               timeout=LONG.get(suite, TIMEOUT))
            rc = p.returncode
            if rc < 0 or not os.path.exists(xml):
                status = f"crash rc={rc}"
        except subprocess.TimeoutExpired:
            rc, status = "timeout", f"timeout after {LONG.get(suite, TIMEOUT)}s"
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"{suite}: rc={rc} {status}", flush=True)
    return suite, rc, status, xml


# Wall-clock budgets are measured on an otherwise idle machine; running them
# beside two other suites measures the neighbours. They run alone, last.
SERIAL = {"PerfStressTest", "XfaSaveBenchmark", "AutoMissingGlyphResolverFixtureTest",
          "PerformanceTest", "UndoStackStressTest"}
parallel = [x for x in suites if x not in SERIAL]
serial = [x for x in suites if x in SERIAL]
with ThreadPoolExecutor(max_workers=JOBS) as ex:
    results = list(ex.map(run, parallel))
results += [run(x) for x in serial]
shutil.rmtree(os.path.join(SYSTEM_TMP, "pdfe-suite-tmp-%d" % os.getpid()), ignore_errors=True)

root = ET.Element("testsuites", name="pdfe_tests-full")
tot = dict(tests=0, failures=0, errors=0, skipped=0)
for suite, rc, status, xml in results:
    if status == "ok":
        r = ET.parse(xml).getroot()
        for s in r.iter("testsuite"):
            root.append(s)
            for c in s.findall("testcase"):
                tot["tests"] += 1
                if c.find("failure") is not None:
                    tot["failures"] += 1
                if c.find("skipped") is not None:
                    tot["skipped"] += 1
    else:
        s = ET.SubElement(root, "testsuite", name=suite, tests="1", failures="0", errors="1")
        c = ET.SubElement(s, "testcase", name="__suite_process__", classname=suite)
        ET.SubElement(c, "error", message=status).text = (
            f"suite process did not complete ({status}); {len(cases.get(suite, []))} "
            f"registered cases not individually reported; see {suite}.log")
        tot["tests"] += 1
        tot["errors"] += 1
for k, v in tot.items():
    root.set(k, str(v))
ET.ElementTree(root).write(os.path.join(OUT, "merged.xml"), encoding="utf-8",
                           xml_declaration=True)
print("TOTALS", tot, "suites", len(suites))
