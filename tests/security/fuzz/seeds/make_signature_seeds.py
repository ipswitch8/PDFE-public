#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate DER seed corpora for the phase-73 signature fuzz harnesses.

Produces REAL, structurally-valid DER for the two harnesses whose parsers are
OpenSSL ``d2i_*`` calls:

  * ``seeds/fuzz_ocsp_response/`` -- OCSP responses produced by a real
    ``openssl ocsp`` responder run against a throwaway CA.
  * ``seeds/fuzz_crl_parse/``     -- X.509 CRLs produced by ``openssl ca
    -gencrl`` against the same CA.

WHY REAL DER AND NOT HAND-ROLLED BYTES.  A ``d2i_*`` parser rejects a malformed
outer layer in a few instructions and returns.  A corpus of only-garbage seeds
therefore never gets PAST the outer SEQUENCE, and every campaign built on it
explores the same shallow refusal path while reporting healthy exec/s.  The
phase-71 lesson (DCA 0035) was exactly this shape: a harness that NAMED a defect
class its corpus could not reach.  A valid response is the scaffold the mutator
needs in order to reach the inner BasicOCSPResponse / TBSCertList structures at
all; the malformed seeds below are useful only BECAUSE the valid ones are there
to splice against.

Deliberately NOT signed with a trusted anchor: the parsers under test
(``OcspResponseParser::parseBytes``, ``CrlParser::parseBytes``) are documented as
strictly structural -- they do not verify signatures -- so a self-signed
throwaway CA exercises exactly the code that runs in production.

Requires ``openssl`` on PATH (the MSYS/mingw64 one is fine).  Re-running is
idempotent: it regenerates every seed from scratch in a temp dir.

    python tests/security/fuzz/seeds/make_signature_seeds.py
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
OCSP_DIR = HERE / "fuzz_ocsp_response"
CRL_DIR = HERE / "fuzz_crl_parse"
TSA_DIR = HERE / "fuzz_tsa_response"

# Minimal CA config: `openssl ca -gencrl` refuses to run without one.
CA_CNF = """\
[ ca ]
default_ca = fuzz_ca

[ fuzz_ca ]
dir               = {dir}
database          = {dir}/index.txt
serial            = {dir}/serial
crlnumber         = {dir}/crlnumber
certificate       = {dir}/ca.pem
private_key       = {dir}/ca.key
default_md        = sha256
default_crl_days  = 30
policy            = fuzz_policy
email_in_dn       = no
rand_serial       = no
unique_subject    = no

[ fuzz_policy ]
commonName              = supplied
organizationName        = optional
countryName             = optional

[ tsa ]
default_tsa = fuzz_tsa

[ fuzz_tsa ]
serial            = {dir}/tsaserial
crypto_device     = builtin
signer_cert       = {dir}/tsa.pem
certs             = {dir}/ca.pem
signer_key        = {dir}/tsa.key
signer_digest     = sha256
default_policy    = 1.2.3.4.1
other_policies    = 1.2.3.4.5,1.2.3.4.6
digests           = sha1,sha256,sha512
accuracy          = secs:1,millisecs:500,microsecs:100
clock_precision_digits = 0
ordering          = yes
tsa_name          = yes
ess_cert_id_chain = no

[ tsa_ext ]
extendedKeyUsage = critical,timeStamping
basicConstraints = CA:FALSE
keyUsage         = critical,digitalSignature
"""


def run(args: list[str], cwd: Path) -> None:
    """Run a command, raising with full output on failure."""
    proc = subprocess.run(
        args,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if proc.returncode != 0:
        out = proc.stdout.decode("utf-8", errors="replace")
        raise RuntimeError(
            "command failed ({rc}): {cmd}\n{out}".format(
                rc=proc.returncode, cmd=" ".join(args), out=out
            )
        )


def write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    print("  wrote {} ({} bytes)".format(path.name, len(data)))


def build_ca(work: Path) -> None:
    """Self-signed CA + one leaf cert + the little CA database `ca` needs."""
    run(
        [
            "openssl", "req", "-x509", "-newkey", "rsa:2048",
            "-keyout", "ca.key", "-out", "ca.pem",
            "-days", "3650", "-nodes",
            "-subj", "/CN=PDFE Fuzz Seed CA/O=PDFE/C=US",
        ],
        work,
    )
    run(
        [
            "openssl", "req", "-newkey", "rsa:2048",
            "-keyout", "leaf.key", "-out", "leaf.csr", "-nodes",
            "-subj", "/CN=PDFE Fuzz Seed Leaf/O=PDFE/C=US",
        ],
        work,
    )
    run(
        [
            "openssl", "x509", "-req", "-in", "leaf.csr",
            "-CA", "ca.pem", "-CAkey", "ca.key",
            "-set_serial", "0x1001",
            "-days", "3650", "-out", "leaf.pem",
        ],
        work,
    )

    (work / "openssl.cnf").write_text(
        CA_CNF.format(dir=work.as_posix()), encoding="utf-8"
    )
    (work / "serial").write_text("1002\n", encoding="utf-8")
    (work / "crlnumber").write_text("1000\n", encoding="utf-8")
    # index.txt: one VALID entry for serial 1001, expiry far in the future.
    # Field order: status, expiry, revocation date, serial, filename, DN.
    (work / "index.txt").write_text(
        "V\t350101000000Z\t\t1001\tunknown\t/CN=PDFE Fuzz Seed Leaf/O=PDFE/C=US\n",
        encoding="utf-8",
    )
    (work / "index.txt.attr").write_text("unique_subject = no\n", encoding="utf-8")


def gen_ocsp(work: Path) -> None:
    print("OCSP seeds ->", OCSP_DIR)
    run(
        [
            "openssl", "ocsp", "-issuer", "ca.pem", "-cert", "leaf.pem",
            "-reqout", "req.der", "-no_nonce",
        ],
        work,
    )

    def responder(out_name: str) -> None:
        run(
            [
                "openssl", "ocsp",
                "-index", "index.txt",
                "-CA", "ca.pem",
                "-rsigner", "ca.pem",
                "-rkey", "ca.key",
                "-reqin", "req.der",
                "-respout", out_name,
                "-nmin", "60",
            ],
            work,
        )

    responder("resp_good.der")
    good = (work / "resp_good.der").read_bytes()
    write(OCSP_DIR / "ocsp_good.der", good)

    # Same responder, leaf now marked revoked -- a DIFFERENT inner
    # CertStatus CHOICE branch (revoked carries a RevokedInfo SEQUENCE that
    # `good`'s NULL-shaped branch does not), so this is not a duplicate seed.
    (work / "index.txt").write_text(
        "R\t350101000000Z\t250101000000Z,keyCompromise\t1001\tunknown"
        "\t/CN=PDFE Fuzz Seed Leaf/O=PDFE/C=US\n",
        encoding="utf-8",
    )
    responder("resp_revoked.der")
    write(OCSP_DIR / "ocsp_revoked.der", (work / "resp_revoked.der").read_bytes())

    # responseStatus != successful. Hand-built because a responder will not
    # emit one on demand, and it is trivially small: SEQUENCE { ENUMERATED 6 }
    # (6 = unauthorized). This is the shape that has NO responseBytes at all,
    # so it exercises the "outer layer parsed, nothing inside" path that the
    # OcspResponse struct's defaults exist for.
    write(OCSP_DIR / "ocsp_unauthorized.der", bytes([0x30, 0x03, 0x0A, 0x01, 0x06]))

    # Truncation at the midpoint: valid prefix, absent suffix. The single most
    # productive malformed shape for a length-prefixed format, because every
    # inner length now overruns.
    write(OCSP_DIR / "ocsp_truncated.der", good[: len(good) // 2])

    # Empty outer SEQUENCE: well-formed DER, semantically nothing.
    write(OCSP_DIR / "ocsp_empty_sequence.der", bytes([0x30, 0x00]))


def gen_crl(work: Path) -> None:
    print("CRL seeds ->", CRL_DIR)

    def gencrl(out_der: str) -> bytes:
        run(
            [
                "openssl", "ca", "-config", "openssl.cnf", "-batch",
                "-gencrl", "-out", "crl.pem",
            ],
            work,
        )
        run(
            [
                "openssl", "crl", "-in", "crl.pem",
                "-outform", "DER", "-out", out_der,
            ],
            work,
        )
        return (work / out_der).read_bytes()

    # index.txt currently has the leaf REVOKED (gen_ocsp left it that way), so
    # this CRL carries a populated revokedCertificates SEQUENCE.
    revoked = gencrl("crl_revoked.der")
    write(CRL_DIR / "crl_revoked.der", revoked)

    # ...and an EMPTY one: revokedCertificates is OPTIONAL in TBSCertList, so
    # its absence is a structurally different encoding, not a smaller version
    # of the same one.
    (work / "index.txt").write_text(
        "V\t350101000000Z\t\t1001\tunknown\t/CN=PDFE Fuzz Seed Leaf/O=PDFE/C=US\n",
        encoding="utf-8",
    )
    empty = gencrl("crl_empty.der")
    write(CRL_DIR / "crl_empty.der", empty)

    write(CRL_DIR / "crl_truncated.der", revoked[: len(revoked) // 2])

    # Correct length framing, wrong outer tag: SET (0x31) where SEQUENCE
    # (0x30) is required. Exercises tag dispatch rather than length handling.
    if revoked:
        write(CRL_DIR / "crl_wrong_outer_tag.der", bytes([0x31]) + revoked[1:])


def gen_tsa(work: Path) -> None:
    """Real RFC 3161 TimeStampTokens, which is what verifyTimestampToken takes.

    Not the .tsr response envelope: the harness's entry point consumes the
    TOKEN, so `-token_out` is the encoding that matters. Seeding with the
    envelope would put the mutator one layer away from the parser under test.
    """
    print("TSA seeds ->", TSA_DIR)

    # A TSA signer needs the timeStamping EKU; `openssl ts -reply` refuses to
    # sign without it, which is worth knowing: the seed generator cannot
    # accidentally produce a token no real TSA would issue.
    run(
        [
            "openssl", "req", "-newkey", "rsa:2048",
            "-keyout", "tsa.key", "-out", "tsa.csr", "-nodes",
            "-subj", "/CN=PDFE Fuzz Seed TSA/O=PDFE/C=US",
        ],
        work,
    )
    run(
        [
            "openssl", "x509", "-req", "-in", "tsa.csr",
            "-CA", "ca.pem", "-CAkey", "ca.key",
            "-set_serial", "0x2001", "-days", "3650",
            "-extfile", "openssl.cnf", "-extensions", "tsa_ext",
            "-out", "tsa.pem",
        ],
        work,
    )
    (work / "tsaserial").write_text("01\n", encoding="utf-8")
    (work / "payload.bin").write_bytes(b"PDFE fuzz seed payload\n")

    run(
        [
            "openssl", "ts", "-query", "-data", "payload.bin",
            "-sha256", "-cert", "-out", "req.tsq",
        ],
        work,
    )
    run(
        [
            "openssl", "ts", "-reply", "-config", "openssl.cnf",
            "-queryfile", "req.tsq",
            "-signer", "tsa.pem", "-inkey", "tsa.key",
            "-out", "resp.tsr",
        ],
        work,
    )
    run(
        [
            "openssl", "ts", "-reply", "-in", "resp.tsr",
            "-token_out", "-out", "token.der",
        ],
        work,
    )

    token = (work / "token.der").read_bytes()
    write(TSA_DIR / "tst_token.der", token)
    # The response envelope too: it is a DIFFERENT structure (PKIStatusInfo +
    # the token), and a mutator that splices between the two explores framing
    # confusion between them -- a realistic confusion, since the transport
    # hands over the envelope and the caller extracts the token.
    write(TSA_DIR / "tsa_response_envelope.der", (work / "resp.tsr").read_bytes())
    write(TSA_DIR / "tst_truncated.der", token[: len(token) // 2])
    write(TSA_DIR / "tst_empty_sequence.der", bytes([0x30, 0x00]))


def main() -> int:
    if shutil.which("openssl") is None:
        print("ERROR: openssl not found on PATH", file=sys.stderr)
        return 1

    for d in (OCSP_DIR, CRL_DIR, TSA_DIR):
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)

    work = Path(tempfile.mkdtemp(prefix="pdfe_sig_seeds_"))
    try:
        build_ca(work)
        gen_ocsp(work)
        gen_crl(work)
        gen_tsa(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("\nOK. Seeds regenerated.")
    return 0


if __name__ == "__main__":
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    sys.exit(main())
