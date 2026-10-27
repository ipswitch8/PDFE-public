# Windows ASan leak detection — accept-and-document decision (SEC-11 / SEC-20)

**Status:** ACCEPTED, not fixed. `windows-sanitizer.yml` runs with
`detect_leaks=0` deliberately and stays that way.
**Finding:** GitHub #48 (SEC-20) and #45 (SEC-11), meta-audit gap-register.
**Owner:** PDFE maintainers.
**Last reviewed:** 2026-09-14 (p85-remaining-open, phase 2) — the #48 half is
CONFIRMED and now rests on direct measurement of both Windows ASan runtimes
rather than on the MSVC one alone. **The #45 half (§2) is REFUTED**: its claim
that clang-cl's Windows ASan implements LeakSanitizer is false, so #45's closure
rationale does not hold and #45 needs re-dispositioning. Previously reviewed
2026-09-07 (backlog-workable-p82, phase-13).

---

## 1. What the two acceptance criteria asked for

- **#45 (SEC-11):** "Flip `detect_leaks=1` on a dispatch run of
  `windows-fuzz.yml` and record whether the LeakSanitizer output is clean."
- **#48 (SEC-20):** "Dispatch `windows-sanitizer.yml` with `detect_leaks=1`
  and a suppression file for Qt/MuPDF statics; record the residual finding
  count."

Read literally as one ask ("both files should show `detect_leaks=1`"), these
two rows point at two lanes with **different, incompatible toolchains**, and
only one of the two asks is satisfiable at all.

## 2. #45 — REFUTED BY MEASUREMENT 2026-09-14; the closure rationale below is WRONG

> **STOP. THE CENTRAL FACTUAL CLAIM OF THIS SECTION IS FALSE.** It asserts that
> clang-cl's Windows ASan runtime "**does** implement LeakSanitizer". It does
> not. Measured on this host on 2026-09-14 (pipeline `p85-remaining-open`
> phase 2), compiling the *exact* configuration this section relies on —
> `clang-cl -fsanitize=fuzzer,address` — and running it with
> `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=0`:
>
> ```
> ==8256==AddressSanitizer: detect_leaks is not supported on this platform.
> RUN_EXIT=1
> ```
>
> The process aborts **before running a single input**. The control run at
> `detect_leaks=0` completed a full 200-run libFuzzer campaign on the same
> binary (`#200 DONE cov: 3 ft: 3 corp: 2/2b`), so the harness is sound and the
> only variable is the flag.
>
> **Consequence: `gh workflow run windows-fuzz.yml -f detect_leaks=1` does not
> produce "a leak-detecting campaign". It produces a lane where every fuzz
> process dies immediately** — the same failure shape §3 correctly warns about
> for the MSVC lane, which this section did not realise also applies here.
> #45 was closed against that rationale and the rationale does not hold; #45
> should be reopened or re-dispositioned on this evidence. That re-disposition
> is **not** in p85 phase 2's scope (its criteria name #48, not #45), so it is
> recorded here and raised rather than silently actioned.
>
> LSan is absent from **both** Windows ASan runtimes — MSVC and clang-cl alike.
> Full experiment, verbatim output and the positive control are in
> `tests/docs/known_issues.md` →
> `CI-LIMITATION-WINDOWS-ASAN-NO-LEAK-DETECTION`.
>
> The original text is preserved below rather than rewritten, because what went
> wrong here is instructive: every *line number* it cites is accurate, and it
> says "verified by reading the file at HEAD, not assumed". It verified that the
> dispatch input exists. It never verified that the runtime honours it — the
> claim the whole closure rests on. Reading a workflow cannot establish a
> property of a compiler runtime.

`windows-fuzz.yml` uses **clang-cl** (`-DPDFE_FUZZ_BACKEND=libfuzzer`,
`-fsanitize=fuzzer,address,undefined`), whose ASan runtime ~~**does** implement
LeakSanitizer~~ **does NOT implement LeakSanitizer (refuted above)**. The
workflow already exposes `detect_leaks` as a
`workflow_dispatch` input:

```
windows-fuzz.yml:115      detect_leaks: (workflow_dispatch input, default '0')
windows-fuzz.yml:120      # gh workflow run windows-fuzz.yml -f detect_leaks=1
windows-fuzz.yml:504      ASAN_OPTIONS: "detect_leaks=${{ github.event.inputs.detect_leaks || '0' }}:halt_on_error=1:abort_on_error=0"
```

Verified by reading the file at HEAD, not assumed: those three line numbers
are current. An operator can already run
`gh workflow run windows-fuzz.yml -f detect_leaks=1` to get a leak-detecting
campaign; #45 is **CLOSED against the current tree**, no code change needed.

> **(The preceding sentence is the false one — see the box at the top of this
> section. The line numbers are still correct; the conclusion drawn from them
> is not. That dispatch aborts the campaign rather than instrumenting it.)**

(A separate `detect_leaks=0` at `windows-fuzz.yml:833` is unrelated — it is
inside the post-campaign `-merge=1` corpus-minimisation step, which is not a
detection pass at all and is hardcoded off there so a benign minimisation run
never manufactures a spurious "leak" finding on inputs already triaged.)

## 3. #48 — declined as written; the grep the criterion asks for would break the lane

`windows-sanitizer.yml` is the **MSVC** ASan lane (`cl.exe /fsanitize=address`
via the `windows-vs2022-msvc-asan` preset). **LeakSanitizer does not exist in
the MSVC ASan runtime.** Setting `detect_leaks=1` there does not produce a
quieter or noisier run — it makes every process print

    AddressSanitizer: detect_leaks is not supported on this platform.

and exit before running a single test. This is already recorded, twice, at
the site itself:

```
windows-sanitizer.yml:309-312   # detect_leaks=0    — NOT a noise-tuning choice. LeakSanitizer does
                                 #                     not exist in the MSVC ASan runtime ...
windows-sanitizer.yml:370-371   # detect_leaks=0    — mandatory: LSan is absent from the MSVC ASan
                                 #                     runtime and detect_leaks=1 aborts the process
```

If #48's literal criterion were satisfied — make `detect_leaks=1` appear in
that file — the lane would go from a working ASan gate on ~445 security-focused
tests to a hard failure on every single dispatch, and a bare `grep -n
'detect_leaks=1'` check would report **success** while the thing it exists to
protect died. That is the exact failure shape #48's own criterion warns
against elsewhere in this project's gate discipline: a check that turns a
working control red while reporting green.

**Decision: leave `windows-sanitizer.yml` at `detect_leaks=0`.** The
suppression-file half of #48's ask (a suppressions list "for Qt/MuPDF
statics") is therefore also moot — there is nothing to suppress on a runtime
that cannot report a leak in the first place.

## 4. What covers the gap instead

Leak detection is not absent from the project; it runs on the two lanes that
have a working LSan:

- `sanitizer.yml:140` — Linux/clang, `ASAN_OPTIONS` includes `detect_leaks=1`.
- `hetzner-sanitizer.yml:114` — Linux/clang, same.
- ~~`windows-fuzz.yml` — clang-cl, opt-in per dispatch (see §2).~~
  **STRUCK 2026-09-14.** This lane has no working LSan; the dispatch input
  exists but the runtime rejects it (§2). Windows leak coverage from this
  project's CI is therefore **zero**, not "opt-in per dispatch" — the two
  Linux lanes above are the whole of it.

Windows-specific leak coverage that MSVC ASan cannot provide is covered
in-suite instead, by live-allocation oracles independent of any sanitizer
(`tests/test_leak_soak_r205_r206.cpp`, closing A1-95/R2-05/R2-06 per the
`windows-sanitizer.yml` comment block cited above).

## 5. If Windows LSan coverage is ever genuinely wanted

Two options exist, neither exercised by this decision:

1. ~~Build the Windows sanitizer lane with **clang-cl** instead of MSVC (what
   `windows-fuzz.yml` already does), gaining LSan at the cost of switching the
   lane's toolchain and re-validating everything currently tuned for MSVC
   ASan's behaviour.~~
   **WITHDRAWN 2026-09-14 — this option does not exist.** It gains nothing:
   clang-cl's Windows ASan runtime rejects `detect_leaks=1` exactly as MSVC's
   does (§2, measured). Switching the lane's toolchain would pay the full
   re-validation cost for zero leak coverage. Anyone reaching for this option
   should stop here; it was the obvious next move and it is a dead end.
2. Accept that Windows leak coverage comes from the Linux lanes and the
   in-suite oracles, and say so — which is what this document does.

Choosing between them is out of scope for #48; this document exists so that
choice is not silently deferred by omission the next time someone reads
`windows-sanitizer.yml` and wonders why leaks are off.
