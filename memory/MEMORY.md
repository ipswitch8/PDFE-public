# PDFE Project Memory Index

Topic files in this directory hold detailed in-tree notes that survive
across sessions. The auto-memory system at `~/.claude/projects/<...>/memory/`
holds the live cross-session notes; this in-tree index points at the
fixes and analyses that are valuable to surface in code review and
onboarding.

## Text-edit save path

- [textedit-tm-sign-bug](textedit-tm-sign-bug.md) — Phase-2 root-cause
  analysis: ContentStreamEditor's `Tm = trm × ctm⁻¹ / fontSize` recovery
  produced wrong-sign Tm.d under Y-flipping CTM. Fix: canonical Tm via
  `canonicalTextMatrixForReplacement`. Locked by 18 tests in
  `tests/test_content_stream_tm_sign.cpp` and
  `tests/test_textedit_tm_regression.cpp`.
