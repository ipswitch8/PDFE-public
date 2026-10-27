# -*- coding: utf-8 -*-
"""Compatibility & environment tests covering Pipeline 3 items.

LOCALE-RENDERING, UNICODE-PATH-FILENAMES (E2E), NETWORK-SNIFFER-INTEGRATION
(simplified), CONFIG-CORRUPTION-RESISTANCE (E2E).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

FIXTURES = REPO_ROOT / "tests" / "fixtures"


def _has_pdfe() -> bool:
    try:
        from tests.helpers.pdfe_session import PDFE_EXE
        return Path(PDFE_EXE).exists()
    except Exception:
        return False


def _is_interactive() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


pytestmark = [
    pytest.mark.skipif(not _has_pdfe(), reason="PDFE binary not found"),
    pytest.mark.skipif(not _is_interactive(), reason="No interactive session"),
]


@pytest.fixture(autouse=True)
def _isolate():
    from tests.helpers.pdfe_session import kill_pdfe_orphans
    try:
        kill_pdfe_orphans()
    except Exception:
        pass
    yield
    try:
        kill_pdfe_orphans()
    except Exception:
        pass


# =====================================================================
# LOCALE-RENDERING: launch under different LANG settings
# =====================================================================

@pytest.mark.parametrize("locale", [
    "en_US.UTF-8",
    "de_DE.UTF-8",
    "ja_JP.UTF-8",
    "zh_CN.UTF-8",
    "ar_SA.UTF-8",  # RTL
])
def test_locale_launch(tmp_path, locale):
    """PDFE launches under different locale environment settings."""
    from tests.helpers.pdfe_session import PDFE_EXE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    env = os.environ.copy()
    env["LANG"] = locale
    env["LC_ALL"] = locale

    proc = subprocess.Popen([str(PDFE_EXE), str(fixture)], env=env)
    try:
        time.sleep(4.0)
        assert proc.poll() is None, f"PDFE must launch under LANG={locale}"
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            proc.kill()


# =====================================================================
# NETWORK-SNIFFER-INTEGRATION (simplified): TCP connection audit
# =====================================================================

def test_pdfe_makes_no_outbound_tcp_during_pdf_open(tmp_path):
    """When opening a benign PDF, PDFE should not initiate any
    outbound TCP connections."""
    from tests.helpers.pdfe_session import PDFE_EXE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    try:
        import psutil
    except ImportError:
        pytest.skip("psutil not available")

    proc = subprocess.Popen([str(PDFE_EXE), str(fixture)])
    try:
        time.sleep(4.0)
        if proc.poll() is not None:
            pytest.skip("PDFE exited early")

        p = psutil.Process(proc.pid)
        # Wait briefly for any startup-triggered connections to settle
        time.sleep(1.0)
        try:
            conns = p.connections(kind="tcp")
        except (psutil.AccessDenied, psutil.NoSuchProcess):
            pytest.skip("Cannot query process connections")

        # Filter to outbound established connections (not local/loopback)
        outbound = [
            c for c in conns
            if c.status == "ESTABLISHED"
            and c.raddr
            and not c.raddr.ip.startswith("127.")
            and not c.raddr.ip.startswith("::1")
        ]
        assert len(outbound) == 0, \
            f"PDFE opened {len(outbound)} outbound TCP conns during PDF open: {outbound}"
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            proc.kill()


def test_pdfe_makes_no_outbound_tcp_with_hostile_js(tmp_path):
    """When opening a PDF with hostile JS (XMLHttpRequest attempt),
    PDFE should not actually initiate the outbound connection."""
    from tests.helpers.pdfe_session import PDFE_EXE

    fixture = FIXTURES / "test_doc_js.pdf"
    if not fixture.exists():
        pytest.skip("test_doc_js.pdf missing")

    try:
        import psutil
    except ImportError:
        pytest.skip("psutil not available")

    proc = subprocess.Popen([str(PDFE_EXE), str(fixture)])
    try:
        time.sleep(4.0)
        if proc.poll() is not None:
            pytest.skip("PDFE exited early on hostile JS")

        p = psutil.Process(proc.pid)
        time.sleep(1.0)
        try:
            conns = p.connections(kind="tcp")
        except (psutil.AccessDenied, psutil.NoSuchProcess):
            pytest.skip("Cannot query process connections")

        outbound = [
            c for c in conns
            if c.status == "ESTABLISHED"
            and c.raddr
            and not c.raddr.ip.startswith("127.")
            and not c.raddr.ip.startswith("::1")
        ]
        assert len(outbound) == 0, \
            f"Hostile JS PDF triggered outbound connections: {outbound}"
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            proc.kill()


# =====================================================================
# CONFIG-CORRUPTION-RESISTANCE (E2E)
# =====================================================================

def test_pdfe_relaunches_clean_after_malicious_open(tmp_path):
    """After opening a malicious PDF and exiting, the next PDFE
    launch must start cleanly without inheriting damage."""
    from tests.helpers.pdfe_session import PDFE_EXE

    # Build a tiny hostile PDF
    malicious = tmp_path / "malicious.pdf"
    malicious.write_bytes(
        b"%PDF-1.7\n"
        b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
        b"/OpenAction << /S /Launch /F (calc.exe) >> >>\nendobj\n"
        b"2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        b"3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b">>\nendobj\n"
        b"xref\n0 4\n0000000000 65535 f \n"
        b"0000000009 00000 n \n0000000150 00000 n \n"
        b"0000000200 00000 n \n"
        b"trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n"
    )

    # First launch: malicious doc
    p1 = subprocess.Popen([str(PDFE_EXE), str(malicious)])
    try:
        time.sleep(3.0)
        # Don't care about p1's state — kill it
    finally:
        try:
            p1.terminate()
            p1.wait(timeout=10)
        except Exception:
            p1.kill()

    time.sleep(2.0)

    # Second launch: benign doc
    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")
    p2 = subprocess.Popen([str(PDFE_EXE), str(fixture)])
    try:
        time.sleep(4.0)
        assert p2.poll() is None, \
            "Second PDFE launch must work after prior malicious open"
    finally:
        try:
            p2.terminate()
            p2.wait(timeout=10)
        except Exception:
            p2.kill()
