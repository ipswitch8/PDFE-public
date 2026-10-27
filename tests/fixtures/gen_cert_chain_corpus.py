# -*- coding: utf-8 -*-
"""Meta-audit follow-up F23 (gap PDF-39): certificate trust-path corpus.

Synthesises every X.509 chain tests/test_cert_chain_trust.cpp needs:

    rootA.der            self-signed CA, the "trusted anchor" of the happy path
    interA.der           intermediate issued by rootA
    leafA.der            end-entity issued by interA          -> trusted chain
    rootB.der            unrelated self-signed CA             -> untrusted anchor
    interA_expired.der   intermediate issued by rootA, ALREADY EXPIRED
    leafA_expired.der    end-entity issued by interA_expired  -> expired-intermediate
    rootC.der            self-signed CA that cross-certifies interA
    interA_cross.der     interA's subject+public key re-issued by rootC
    leafA_revoked.der    end-entity issued by interA, listed in crlA_revoked
    crlA_revoked.der     CRL issued by interA revoking leafA_revoked
    crlA_empty.der       CRL issued by interA revoking nothing (the control)

WHY THIS SCRIPT EXISTS
----------------------
Certificates expire. A corpus of opaque committed blobs whose validity was
"whatever the clock said the day someone ran openssl" turns the suite red on a
calendar boundary years later, and the failure looks like a product defect.
Everything here is pinned:

  * Every validity window is a HARD-CODED absolute instant (see the constants
    below). Nothing is derived from datetime.now(). Re-running this script in
    2031 produces byte-different keys but IDENTICAL validity windows.
  * The "valid" material runs to NOT_AFTER_FAR (2199-01-01), so even the tests
    that verify at wall-clock time cannot age out inside any plausible life of
    this repo.
  * The "expired" material is expired BY CONSTRUCTION (notAfter 2021-01-01),
    not by waiting.
  * The C++ tests additionally pin the VERIFICATION time to
    kVerificationTimeUnix (2030-06-15T00:00:00Z), which sits inside every
    valid window and after every expired one. That is the load-bearing
    guarantee: the corpus's relationship to the verification instant is fixed
    by two constants, not by the calendar.

KEY MATERIAL
------------
No private key is ever written under the repository. Keys are generated into a
fresh tempfile.mkdtemp() directory and shredded on exit. Only the certificates
and CRLs -- which are public by definition -- are written to the output dir.
Pass --keep-keys to retain the temp directory (it prints the path); that is a
debugging aid and still writes nothing into the repo.

DEPENDENCY
----------
Python `cryptography` (>= 3.4 for the CRL builder API; developed against
44.0.3). This is a build/dev-time dependency of the fixture generator only --
the C++ suite consumes the committed .der files and does not invoke Python.

USAGE
-----
    python tests/fixtures/gen_cert_chain_corpus.py
    python tests/fixtures/gen_cert_chain_corpus.py --out <dir> --keep-keys
"""

from __future__ import annotations

import argparse
import datetime as _dt
import os
import shutil
import sys
import tempfile

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
except ImportError:  # pragma: no cover - explicit, actionable failure
    sys.stderr.write(
        "gen_cert_chain_corpus.py requires the 'cryptography' package.\n"
        "Install with:  python -m pip install cryptography\n")
    raise

# --------------------------------------------------------------------------
# Pinned instants. NOTHING here may be derived from the current date.
# --------------------------------------------------------------------------
UTC = _dt.timezone.utc

# Start of every validity window in the corpus.
NOT_BEFORE = _dt.datetime(2020, 1, 1, 0, 0, 0, tzinfo=UTC)
# End of every window that must remain valid. Deliberately absurd so the
# wall-clock-time tests cannot age out.
NOT_AFTER_FAR = _dt.datetime(2199, 1, 1, 0, 0, 0, tzinfo=UTC)
# End of the deliberately-expired intermediate's window. Already in the past
# at the time of writing AND, more importantly, before the verification
# instant the C++ tests pin.
NOT_AFTER_EXPIRED = _dt.datetime(2021, 1, 1, 0, 0, 0, tzinfo=UTC)

# Must match kVerificationTimeUnix in tests/test_cert_chain_trust.cpp.
# 2030-06-15T00:00:00Z -- inside every "far" window, after the expired one.
VERIFICATION_INSTANT = _dt.datetime(2030, 6, 15, 0, 0, 0, tzinfo=UTC)

# CRL freshness window: also pinned, so a CRL never goes stale on a boundary.
CRL_LAST_UPDATE = NOT_BEFORE
CRL_NEXT_UPDATE = NOT_AFTER_FAR

KEY_BITS = 2048


def _assert_pins_consistent() -> None:
    """The corpus is only meaningful if the pinned instants stay ordered."""
    assert NOT_BEFORE < NOT_AFTER_EXPIRED < VERIFICATION_INSTANT < NOT_AFTER_FAR, (
        "Pinned instants are out of order -- the expired fixture would no "
        "longer be expired (or the valid fixtures no longer valid) at the "
        "verification instant the C++ tests use.")
    assert CRL_LAST_UPDATE < VERIFICATION_INSTANT < CRL_NEXT_UPDATE, (
        "CRL freshness window does not straddle the verification instant.")


def _name(cn: str) -> x509.Name:
    return x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, "US"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "PDFE Test Corpus"),
        x509.NameAttribute(NameOID.ORGANIZATIONAL_UNIT_NAME, "F23 PDF-39"),
        x509.NameAttribute(NameOID.COMMON_NAME, cn),
    ])


def _new_key() -> rsa.RSAPrivateKey:
    return rsa.generate_private_key(public_exponent=65537, key_size=KEY_BITS)


def _sign(builder, issuer_key):
    return builder.sign(private_key=issuer_key, algorithm=hashes.SHA256())


def _base(subject: x509.Name, issuer: x509.Name, pubkey, serial: int,
          not_after: _dt.datetime):
    return (x509.CertificateBuilder()
            .subject_name(subject)
            .issuer_name(issuer)
            .public_key(pubkey)
            .serial_number(serial)
            .not_valid_before(NOT_BEFORE)
            .not_valid_after(not_after))


def make_ca(cn: str, serial: int, issuer_cert=None, issuer_key=None,
            not_after: _dt.datetime = NOT_AFTER_FAR,
            path_length: int | None = None,
            key: rsa.RSAPrivateKey | None = None,
            subject_override: x509.Name | None = None):
    """Build a CA certificate. Self-signed when issuer_cert is None.

    `key` lets a caller re-use an existing key pair -- that is what makes a
    cross-certificate a cross-certificate: same subject DN, same public key,
    different issuer.
    """
    key = key or _new_key()
    subject = subject_override or _name(cn)
    issuer = issuer_cert.subject if issuer_cert else subject
    signing_key = issuer_key if issuer_cert else key

    b = _base(subject, issuer, key.public_key(), serial, not_after)
    b = b.add_extension(
        x509.BasicConstraints(ca=True, path_length=path_length), critical=True)
    b = b.add_extension(
        x509.KeyUsage(digital_signature=True, content_commitment=False,
                      key_encipherment=False, data_encipherment=False,
                      key_agreement=False, key_cert_sign=True, crl_sign=True,
                      encipher_only=False, decipher_only=False),
        critical=True)
    b = b.add_extension(
        x509.SubjectKeyIdentifier.from_public_key(key.public_key()),
        critical=False)
    return _sign(b, signing_key), key


def make_leaf(cn: str, serial: int, issuer_cert, issuer_key,
              not_after: _dt.datetime = NOT_AFTER_FAR):
    key = _new_key()
    b = _base(_name(cn), issuer_cert.subject, key.public_key(), serial,
              not_after)
    b = b.add_extension(x509.BasicConstraints(ca=False, path_length=None),
                        critical=True)
    b = b.add_extension(
        x509.KeyUsage(digital_signature=True, content_commitment=True,
                      key_encipherment=False, data_encipherment=False,
                      key_agreement=False, key_cert_sign=False, crl_sign=False,
                      encipher_only=False, decipher_only=False),
        critical=True)
    b = b.add_extension(
        x509.SubjectKeyIdentifier.from_public_key(key.public_key()),
        critical=False)
    return _sign(b, issuer_key), key


def make_crl(issuer_cert, issuer_key, revoked_serials):
    b = (x509.CertificateRevocationListBuilder()
         .issuer_name(issuer_cert.subject)
         .last_update(CRL_LAST_UPDATE)
         .next_update(CRL_NEXT_UPDATE))
    for serial in revoked_serials:
        b = b.add_revoked_certificate(
            x509.RevokedCertificateBuilder()
            .serial_number(serial)
            .revocation_date(NOT_BEFORE)
            .build())
    return b.sign(private_key=issuer_key, algorithm=hashes.SHA256())


def _write_der(out_dir: str, name: str, obj) -> str:
    path = os.path.join(out_dir, name)
    with open(path, "wb") as f:
        f.write(obj.public_bytes(serialization.Encoding.DER))
    return path


def _stash_key(key_dir: str, name: str, key) -> None:
    """Private keys go to the TEMP dir only -- never under the repo."""
    with open(os.path.join(key_dir, name), "wb") as f:
        f.write(key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption()))


def generate(out_dir: str, key_dir: str) -> list[str]:
    os.makedirs(out_dir, exist_ok=True)
    written: list[str] = []

    # --- Trusted branch: rootA -> interA -> leafA -----------------------
    root_a, root_a_key = make_ca("PDFE F23 Root A", 0x1001)
    inter_a, inter_a_key = make_ca("PDFE F23 Intermediate A", 0x1002,
                                   root_a, root_a_key, path_length=0)
    leaf_a, leaf_a_key = make_leaf("PDFE F23 Leaf A", 0x1003,
                                   inter_a, inter_a_key)

    # --- Untrusted anchor: an entirely unrelated root -------------------
    root_b, root_b_key = make_ca("PDFE F23 Root B", 0x2001)

    # --- Expired branch: rootA -> interA_expired -> leafA_expired -------
    # The INTERMEDIATE is the expired link. The leaf's own window is far, so
    # a rejection here can only be attributed to the intermediate.
    inter_a_exp, inter_a_exp_key = make_ca(
        "PDFE F23 Intermediate A Expired", 0x3001, root_a, root_a_key,
        not_after=NOT_AFTER_EXPIRED, path_length=0)
    leaf_a_exp, leaf_a_exp_key = make_leaf("PDFE F23 Leaf A Expired", 0x3002,
                                           inter_a_exp, inter_a_exp_key)

    # --- Cross-certification: rootC re-issues interA --------------------
    # Same subject DN, same public key, different issuer and serial. leafA
    # therefore has TWO paths: via rootA (direct) and via rootC (cross).
    root_c, root_c_key = make_ca("PDFE F23 Root C", 0x4001)
    inter_a_cross, _ = make_ca(
        "PDFE F23 Intermediate A", 0x4002, root_c, root_c_key,
        path_length=0, key=inter_a_key, subject_override=inter_a.subject)
    assert inter_a_cross.subject == inter_a.subject, "cross cert subject drift"
    assert (inter_a_cross.public_key().public_numbers()
            == inter_a.public_key().public_numbers()), "cross cert key drift"

    # --- Revocation: leafA_revoked + the CRL that names it --------------
    leaf_a_rev, _ = make_leaf("PDFE F23 Leaf A Revoked", 0x5001,
                              inter_a, inter_a_key)
    crl_revoked = make_crl(inter_a, inter_a_key, [leaf_a_rev.serial_number])
    # CONTROL: structurally identical CRL from the same issuer that revokes
    # nothing. Without it, "revoked" could be an artefact of CRL checking
    # being switched on at all rather than of the serial being listed.
    crl_empty = make_crl(inter_a, inter_a_key, [])

    for name, obj in [
        ("rootA.der", root_a),
        ("interA.der", inter_a),
        ("leafA.der", leaf_a),
        ("rootB.der", root_b),
        ("interA_expired.der", inter_a_exp),
        ("leafA_expired.der", leaf_a_exp),
        ("rootC.der", root_c),
        ("interA_cross.der", inter_a_cross),
        ("leafA_revoked.der", leaf_a_rev),
        ("crlA_revoked.der", crl_revoked),
        ("crlA_empty.der", crl_empty),
    ]:
        written.append(_write_der(out_dir, name, obj))

    for name, key in [
        ("rootA.key.pem", root_a_key),
        ("interA.key.pem", inter_a_key),
        ("leafA.key.pem", leaf_a_key),
        ("rootB.key.pem", root_b_key),
        ("interA_expired.key.pem", inter_a_exp_key),
        ("leafA_expired.key.pem", leaf_a_exp_key),
        ("rootC.key.pem", root_c_key),
    ]:
        _stash_key(key_dir, name, key)

    return written


def main(argv: list[str] | None = None) -> int:
    _assert_pins_consistent()

    default_out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "cert_chains")
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=default_out,
                    help="output directory for certificates and CRLs")
    ap.add_argument("--keep-keys", action="store_true",
                    help="retain the temporary private-key directory "
                         "(still outside the repository) and print its path")
    args = ap.parse_args(argv)

    key_dir = tempfile.mkdtemp(prefix="pdfe_f23_keys_")
    try:
        written = generate(args.out, key_dir)
    finally:
        if args.keep_keys:
            sys.stdout.write("private keys retained in: %s\n" % key_dir)
        else:
            shutil.rmtree(key_dir, ignore_errors=True)

    sys.stdout.write("wrote %d public artefacts to %s\n"
                     % (len(written), args.out))
    for p in written:
        sys.stdout.write("  %s\n" % os.path.basename(p))
    sys.stdout.write(
        "validity pins: notBefore=%s notAfterFar=%s notAfterExpired=%s "
        "verificationInstant=%s (unix %d)\n"
        % (NOT_BEFORE.isoformat(), NOT_AFTER_FAR.isoformat(),
           NOT_AFTER_EXPIRED.isoformat(), VERIFICATION_INSTANT.isoformat(),
           int(VERIFICATION_INSTANT.timestamp())))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
