# -*- coding: utf-8 -*-
"""
Static source audit: verifies every dialog/panel in src/ui/ has
setObjectName calls for the widgets that the native-automation test
layer depends on. Does not launch PDFE. Safe to run any time.

Run:
    python tests/ui_automation/audit_source.py
"""
from __future__ import annotations
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "src" / "ui"

# file -> list of objectName strings we expect to see set
EXPECTED = {
    "AboutDialog.cpp": [
        # 2026-05-06: marketing content widgets (appNameLabel,
        # versionLabel, librariesLabel, licenseLabel, colorLogoLabel,
        # broughtToYouByLabel, itwerxLogoLabel, donationLabel) were
        # collapsed into a single QTextBrowser "aboutContent"
        # rendering :/branding/about.md.
        "aboutDialog", "aboutContent", "licenseTextEdit", "okButton",
    ],
    "PasswordDialog.cpp": [
        "passwordDialog", "passwordPromptLabel", "passwordEdit",
        "buttonBox", "okButton",
    ],
    "AnnotationPropertiesWidget.cpp": [
        "annotationPropertiesWidget", "annotTypeLabel", "annotAuthorEdit",
        "annotContentsLabel", "annotContentsEdit", "annotColorButton",
        "annotOpacitySlider", "annotOpacityLabel", "annotFontGroup",
        "annotFontCombo", "annotFontSizeSpinner", "annotTextColorButton",
    ],
    "ComplianceDialog.cpp": [
        "ComplianceDialog", "TargetLabel", "LevelCombo", "AnalyzeButton",
        "ValidateButton", "ConvertButton", "IssueLabel", "IssueTable",
        "StatusLabel",
    ],
    "AtlasComparisonDialog.cpp": [
        "AtlasComparisonDialog", "atlasOriginalPreview", "atlasFontPreview",
        "atlasFontCombo", "atlasAtlasPreview", "atlasFontCharsetScroll",
        "atlasFontCharsetLabel", "atlasUseFontButton", "atlasFillMissingCheck",
        "atlasAtlasCharsetScroll", "atlasAtlasCharsetLabel",
        "atlasUseScanButton", "atlasCancelButton",
    ],
    "RecentFilesMenu.cpp": [
        "recentFilesMenu", "recentFilesNoneAction", "recentFilesClearAction",
    ],
    "PrintPreviewDialog.cpp": [
        "printPreviewDialog", "previewToolBar", "printPreviewWidget",
        "zoomInAction", "zoomOutAction", "fitWidthAction", "fitPageAction",
    ],
}

OBJ_NAME_RE = re.compile(r'setObjectName\(\s*(?:QStringLiteral\()?\s*"([^"]+)"')


def audit_file(path: Path, expected: list[str]) -> tuple[int, int, list[str]]:
    if not path.exists():
        return 0, len(expected), [f"file missing: {path.name}"]
    source = path.read_text(encoding="utf-8", errors="replace")
    found = set(OBJ_NAME_RE.findall(source))
    missing = [name for name in expected if name not in found]
    return len(expected) - len(missing), len(missing), missing


def main() -> int:
    print(f"Auditing {UI}\n")
    total_ok = 0
    total_missing = 0
    failed_files: list[str] = []
    for filename, expected in sorted(EXPECTED.items()):
        ok, miss, missing = audit_file(UI / filename, expected)
        total_ok += ok
        total_missing += miss
        status = "OK  " if miss == 0 else "MISS"
        print(f"  [{status}] {filename:38s}  {ok}/{len(expected)}")
        if miss:
            failed_files.append(filename)
            for m in missing:
                print(f"          - missing: {m}")
    print()
    print(f"Result: {total_ok} found, {total_missing} missing "
          f"across {len(EXPECTED)} files")
    if total_missing:
        print(f"\nFAILED files: {', '.join(failed_files)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
