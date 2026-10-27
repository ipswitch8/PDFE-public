# -*- coding: utf-8 -*-
"""
Generator for tests/fixtures/redaction_multi_revision.pdf

A GENUINE multi-revision (incremental-update) PDF used to exercise the
RedactionVerifier::checkSingleRevision() FAIL path (mutant M11).

Mutant-anchor note (F31, 2026-08-06): M11 used to be spelled
`return maxGeneration == 0;` -> `return true;`, which was the whole body of
the generation-only heuristic. Commit ed75a5a (F1 / PDF-68) replaced that
heuristic with a structural check, so M11 is now anchored on the generation
loop and replaces it with an unconditional-clean early return. The MUTANT'S
EFFECT is unchanged — checkSingleRevision always reports clean — and this
fixture is still what kills it. See tools/gate/mutants.json and
docs/audit/meta-audit-2026-07/followup/phase-31-dispositions.md.

The file has TWO cross-reference sections chained by /Prev:
  * Base revision: objects 1(Catalog) 2(Pages) 3(Page), all generation 0.
  * Incremental revision (appended): object 2 is overridden to point its
    /Kids at "3 1 R", and a NEW object "3 1 obj" (a Page at GENERATION 1)
    is appended. Reusing object number 3 at a higher generation is exactly
    how residual pre-edit data survives across an incremental update.

Because a live object survives at generation 1, QPDF's getAllObjects()
yields an object with getGeneration() == 1, so checkSingleRevision() must
return FALSE (previous data may be recoverable). The mutant that forces it
to always report clean is thereby KILLED. Note the fixture trips ALL THREE
of the post-PDF-68 signals — generation > 0, /Prev in the trailer, and two
startxref/%%EOF markers in an unlinearized file — which is why M11 is a
single "always clean" mutant rather than one mutant per signal (a
per-signal mutant would be masked by the other two and would SURVIVE).

Regenerate with:  python tests/fixtures/gen_redaction_multi_revision.py
"""
import os


def _obj(num, gen, body):
    return b"%d %d obj\n" % (num, gen) + body + b"\nendobj\n"


def build():
    out = bytearray()
    out += b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"
    off = {}

    # ---- base revision (all generation 0) ----
    off[(1, 0)] = len(out)
    out += _obj(1, 0, b"<< /Type /Catalog /Pages 2 0 R >>")
    off[(2, 0)] = len(out)
    out += _obj(2, 0, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    off[(3, 0)] = len(out)
    out += _obj(3, 0, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>")

    base_xref = len(out)
    out += b"xref\n0 4\n"
    out += b"0000000000 65535 f \n"
    out += b"%010d 00000 n \n" % off[(1, 0)]
    out += b"%010d 00000 n \n" % off[(2, 0)]
    out += b"%010d 00000 n \n" % off[(3, 0)]
    out += b"trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % base_xref

    # ---- incremental revision (appended) ----
    # Override obj 2 to reference the gen-1 page, and append "3 1 obj".
    off[(2, 0)] = len(out)
    out += _obj(2, 0, b"<< /Type /Pages /Kids [3 1 R] /Count 1 >>")
    off[(3, 1)] = len(out)
    out += _obj(3, 1, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>")

    incr_xref = len(out)
    out += b"xref\n2 2\n"
    out += b"%010d 00000 n \n" % off[(2, 0)]
    out += b"%010d 00001 n \n" % off[(3, 1)]
    out += (b"trailer\n<< /Size 4 /Root 1 0 R /Prev %d >>\n"
            b"startxref\n%d\n%%%%EOF\n" % (base_xref, incr_xref))
    return bytes(out)


def main():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "redaction_multi_revision.pdf")
    with open(path, "wb") as f:
        f.write(build())
    print("wrote", path)


if __name__ == "__main__":
    main()
