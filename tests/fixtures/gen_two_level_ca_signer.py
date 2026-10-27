# -*- coding: utf-8 -*-
"""Meta-audit follow-up F47 / F1 (phase-48): two-level CA signing corpus whose
CRL issuers differ ONLY in the keyUsage `cRLSign` bit.

WHY THIS EXISTS
---------------
RFC 5280 §6.3.3(f) says a CRL may only be honoured if the certificate that
signed it is entitled to sign CRLs -- i.e. if its keyUsage extension is
present, it must assert `cRLSign`. `src/signatures/LtvValidator.cpp`
(`crlIsAuthorized`) could not enforce that rule while the entire signing test
corpus rested on `tests/fixtures/test_signer.pfx`, which is a **self-signed
end-entity** certificate (`CA:FALSE`; keyUsage critical
`digitalSignature, contentCommitment`; **no** `cRLSign`). Every CRL in the LTV
suite is issued by that certificate to itself, so honouring the rule would have
rejected all of them. Phase-42 filed the rule rather than shipping it
half-applied; this script supplies the fixture that unblocks it.

WHAT IT PRODUCES  (all under tests/fixtures/crl_chain/)
------------------------------------------------------
    root.der                  self-signed root CA (keyCertSign + cRLSign)

  the POSITIVE chain -- the CRL issuer IS entitled to sign CRLs:
    inter_crlsign.der         intermediate CA issued by root,
                              keyUsage = keyCertSign + cRLSign
    inter_crlsign_key.pem     its private key, so tests can mint CRLs with
                              wall-clock-relative validity at run time
    leaf_crlsign.der          end-entity issued by inter_crlsign
    leaf_crlsign.pfx          the same leaf as PKCS#12 (password `test123`),
                              bundled with inter_crlsign + root, for
                              SigningOptions::pfxPath

  the NEGATIVE control -- structurally identical, one bit different:
    inter_nocrlsign.der       intermediate CA issued by root,
                              keyUsage = keyCertSign ONLY (no cRLSign)
    inter_nocrlsign_key.pem   its private key
    leaf_nocrlsign.der        end-entity issued by inter_nocrlsign
    leaf_nocrlsign.pfx        PKCS#12 (password `test123`)

  the DISTRIBUTION-POINT control (phase-49 review, finding M3):
    leaf_crldp.der            end-entity issued by inter_crlsign, identical to
                              leaf_crlsign except that it carries a
                              cRLDistributionPoints extension naming
                              LEAF_CRLDP_URI. Without a certificate that names
                              a distribution point, the fullName-matching block
                              of `idpCoversCert()` is unreachable from any test.
    leaf_crldp.pfx            PKCS#12 (password `test123`)

  the keyUsage-ABSENT control (F55, phase-51):
    inter_noku.der            intermediate CA issued by root with NO keyUsage
                              extension at all -- not present-and-empty,
                              genuinely absent. RFC 5280 §4.2.1.3 constrains a
                              certificate only when the extension is present,
                              and OpenSSL signals that by returning UINT32_MAX
                              (every bit set) from X509_get_key_usage(). So this
                              CA's CRLs ARE honoured, and it is the only member
                              of the corpus that exercises that branch of
                              crlIsAuthorized(). Over-tightening the rule into
                              "keyUsage must be present" would break real-world
                              CAs; inter_noku is what makes that mutation die.
    inter_noku_key.pem        its private key, for run-time CRL minting
    leaf_noku.der             end-entity issued by inter_noku
    leaf_noku.pfx             PKCS#12 (password `test123`)

  the MALFORMED-CA control (F60(b), phase-51):
    inter_malformed_ca.der    intermediate CA issued by root whose
                              basicConstraints is the RFC-violating encoding
                              SEQUENCE { INTEGER 0 } -- cA absent (FALSE) WITH a
                              pathLenConstraint, which RFC 5280 §4.2.1.9
                              forbids -- while its keyUsage asserts keyCertSign
                              and cRLSign. X509_check_ca() cannot classify it
                              and returns 0, the same value it returns for
                              "definitely not a CA". idpCoversCert() reads that
                              0 at face value, so an onlyCA-scoped CRL is judged
                              NOT to cover this certificate and its revocation
                              entry is discarded. That is a DOCUMENTED,
                              accepted fail-open; it is pinned, not changed --
                              see docs/audit/.../phase-51 disposition for F60(b)
                              and tests/test_f55_f60b_crl_fixture_coverage.cpp.
                              No private key is emitted for it: every CRL it
                              appears in is signed by the ROOT.

    MANIFEST.sha256           SHA-256 of every byte-deterministic artifact

The two chains are generated by the SAME code path with the same subject
pattern, the same validity window, the same key sizes and the same signature
algorithm. The ONLY difference between `inter_crlsign` and `inter_nocrlsign` is
the `crl_sign` boolean in the keyUsage extension. That is what makes the
negative test a *control* rather than a differently-shaped test: if the
enforcement in `crlIsAuthorized` is removed, the negative case stops failing
closed and the control goes red.

WHICH keyUsage BITS AN INTERMEDIATE CRL ISSUER ACTUALLY NEEDS
-------------------------------------------------------------
Derived from RFC 5280 and from what the verification code really checks, not
guessed:

  * `keyCertSign` (RFC 5280 §6.1.4(n) / §4.2.1.3) -- required because
    `certIssuedSigner()` in LtvValidator.cpp calls `X509_verify(signerCert,
    issuerPub)`, i.e. the intermediate must genuinely have issued the leaf, and
    a CA asserting keyUsage must assert keyCertSign to do that.
  * `cRLSign` (RFC 5280 §6.3.3(f)) -- the bit under test.
  * `basicConstraints CA:TRUE` -- required by `X509_verify_cert()` inside
    `CertChainValidator::validate()` for the intermediate to be usable as a
    path element at all.

Deliberately NOT set on the intermediates: `digitalSignature`. It is not needed
to sign either a certificate or a CRL (both are covered by keyCertSign /
cRLSign respectively), and leaving it off keeps the fixture minimal, so no test
can accidentally pass because of an unrelated bit. Note this differs from
`tests/fixtures/cert_chains/` (the F23 corpus), whose intermediate also carries
`digitalSignature`; that corpus is for path building, not CRL authorisation,
and is left untouched.

The leaves carry `digitalSignature + contentCommitment` and `CA:FALSE`,
matching what a document-signing certificate needs (and matching
`test_signer.pfx`, so the leaves are drop-in for signing purposes).

VALIDITY WINDOW  --  2020-01-01T00:00:00Z .. 2199-01-01T00:00:00Z
------------------------------------------------------------------
Hard-coded absolute instants. Nothing is derived from `datetime.now()`, so
re-running this script in any year produces the SAME window. The window is
chosen to satisfy two constraints simultaneously:

  * the LTV tests verify at WALL-CLOCK time (they mint CRLs relative to
    `time(nullptr)`), so the certificates must be valid *now*, on any machine,
    for the life of this repository -- hence notAfter 2199;
  * notBefore is in the past for any plausible run.

This mirrors the pinning discipline already established by
`tests/fixtures/gen_cert_chain_corpus.py`. A fixture whose validity was
"whatever the clock said the day someone ran openssl" turns the suite red on a
calendar boundary and the failure looks like a product defect.

DETERMINISM  --  what is byte-reproducible and what is not
-----------------------------------------------------------
BYTE-DETERMINISTIC (regenerating reproduces identical bytes, and
`--check` proves it):

  * every `.der`
  * every `_key.pem` (marker included -- the marker is a fixed constant, so it
    does not cost determinism)

This is achieved by (a) embedding the private keys verbatim below rather than
generating them, (b) fixing every serial number, (c) fixing the validity
window, and (d) signing with **RSA PKCS#1 v1.5**, which -- unlike ECDSA -- is a
deterministic signature scheme. So the committed certificate bytes can be shown
to have come from the committed script.

NOT BYTE-DETERMINISTIC, and this cannot be fixed:

  * every `.pfx`

PKCS#12 (RFC 7292) encrypts its contents with a password-based scheme that
requires a fresh random salt and IV per serialisation. Two runs therefore
produce different bytes for the same inputs. What IS stable is the *content*:
the certificate and key inside each .pfx are the byte-identical DER/PEM
artifacts listed above. `--check` verifies exactly that -- it unwraps each
committed .pfx and compares its certificate DER and private key DER against the
regenerated ones -- so a reviewer can still establish that the committed .pfx
came from the committed script, just not by hashing it.

KEY MATERIAL
------------
The private keys are embedded in this file **on purpose**. They are test-only
keys with no trust relationship to any real CA, they are already public by
virtue of being in a public AGPL repository, and embedding them is what makes
the certificate bytes reproducible. Do not reuse them for anything.

Since F56 (phase-51) every EMITTED `*_key.pem` additionally carries a leading
`# PDFE TEST-ONLY KEY ...` marker (see `KEY_MARKER`) so the file says what it is
without the repository around it. The marker sits before the `-----BEGIN` line,
where RFC 7468 §5.2 says readers must skip it, so parsing is unaffected --
`--check` proves that by reloading each committed key rather than asserting it.
`tools/gate/check_crl_chain_fixture.py` independently requires the marker, so it
cannot silently vanish on a regeneration.

DEPENDENCY
----------
Python `cryptography` (developed against 44.x). Build/dev-time only -- the C++
suite consumes the committed artifacts and never invokes Python.

USAGE
-----
    python tests/fixtures/gen_two_level_ca_signer.py            # (re)generate
    python tests/fixtures/gen_two_level_ca_signer.py --check    # verify only
    python tests/fixtures/gen_two_level_ca_signer.py --out DIR
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import io
import pathlib
import sys
import tempfile

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.serialization import pkcs12
from cryptography.x509.oid import ExtensionOID, NameOID

# ---------------------------------------------------------------------------
# Pinned parameters. Change NOTHING here casually: every one of these values is
# load-bearing for byte-determinism.
# ---------------------------------------------------------------------------

NOT_BEFORE = datetime.datetime(2020, 1, 1, 0, 0, 0, tzinfo=datetime.timezone.utc)
NOT_AFTER = datetime.datetime(2199, 1, 1, 0, 0, 0, tzinfo=datetime.timezone.utc)

SERIAL_ROOT = 0x2001
SERIAL_INTER_CRLSIGN = 0x2002
SERIAL_LEAF_CRLSIGN = 0x2003
SERIAL_INTER_NOCRLSIGN = 0x2004
SERIAL_LEAF_NOCRLSIGN = 0x2005
SERIAL_LEAF_CRLDP = 0x2006
# F55 (phase-51): the keyUsage-ABSENT intermediate and its leaf.
SERIAL_INTER_NOKU = 0x2007
SERIAL_LEAF_NOKU = 0x2008
# F60(b) (phase-51): the malformed/contradictory CA.
SERIAL_INTER_MALFORMED_CA = 0x2009

# The distribution point `leaf_crldp` names, and the ONLY name an
# issuingDistributionPoint may carry and still cover it. Any other URI, and the
# fullName-matching block in idpCoversCert() must refuse. Pinned here so the
# fixture and the tests cannot drift into agreeing by accident.
LEAF_CRLDP_URI = "http://crl.pdfe.test/f47/inter_crlsign.crl"

PFX_PASSWORD = b"test123"

ORG = "PDFE Test Corpus"
OU = "F47 CRL keyUsage"

# ---------------------------------------------------------------------------
# Meta-audit follow-up F56 (phase-51): every emitted private key carries an
# in-file, human-readable statement of what it is.
#
# The keys below are throwaway test material with no trust relationship to
# anything, but a `.pem` holding `-----BEGIN PRIVATE KEY-----` and NOTHING ELSE
# is indistinguishable from a real one to anybody who finds it out of context —
# a secret scanner, a support bundle, a developer grepping the tree. The file
# should say what it is without needing the repository around it.
#
# The marker is PRE-ENCAPSULATION BOUNDARY text: RFC 7468 §5.2 and every PEM
# reader in practice (OpenSSL's PEM_read_bio_*, `cryptography`'s
# load_pem_private_key) skip everything before the first `-----BEGIN` line. So
# this is inert to parsers and visible to humans, which is exactly the
# combination wanted. `--check` and tools/gate/check_crl_chain_fixture.py both
# assert it is still there, because a marker nothing enforces is a marker that
# quietly disappears on the next regeneration.
#
# LF ONLY. `core.autocrlf=true` in this repository, and
# `.gitattributes` marks `tests/fixtures/crl_chain/*.pem` as `-text` so these
# bytes survive a checkout unnormalised. Every consumer of this constant
# nevertheless folds CRLF -> LF before comparing, so a lost `.gitattributes`
# rule degrades to "still passes" rather than to a false corruption report.
# ---------------------------------------------------------------------------
KEY_MARKER = (
    "# PDFE TEST-ONLY KEY -- generated by "
    "tests/fixtures/gen_two_level_ca_signer.py\n"
    "# Not a secret. Never used outside the test corpus. Safe to publish.\n"
)

# ---------------------------------------------------------------------------
# Embedded fixed key material -- see "KEY MATERIAL" in the module docstring.
# ---------------------------------------------------------------------------

ROOT_KEY_PEM = """-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC9DYcqKR0fRupf
++7HWCAl3oiTtZJhhpPaN5IG7BD77N+3mgaWBqcdY7sFjeju05W1UVUcrXM9uPC/
4sjK7bEwqkb3SbOAD9VMrGylpG+YIQuJs4piGb9ILzD2XRA1PQ3XD5bMuaRnEMxS
76KbUglb5F7J6VSiBGQN2Ihuoa04O62hHb4h6yNAgilhWp2lEWZ28QJmhWKSNvd3
G+iR6VDOpIdV/viXa7yOMBQ6mSZa7IIMoKOygUMu/7itkdrcyPDQnRYOmFs80HxT
Ll1asuCfy/p/s9UyjjGddK5LRmmbi2GHOlWjU4GZjegQzqOl0EbuOJfS+LNCXK+c
1nyiAk17AgMBAAECggEAHhWxh/A2qLFd1HwgfEuWA2toMO1TnT56d972V4X95paZ
kWZVnE6fDcppoCbRmx6Y89uSCk1rNIwiOmmqChUM6KbS7UD8FinZh+k7zu5Ig0fl
m0+y0A1d3GxaGSfSek5mze22pzShqMnKYiZX/5SXVa7AC4Hx9xMbeFKYKiwuBxpo
s8EFFAg6rmX0W1o3dLEVavI8UEhRTlmDwZ06xAqwkzeFN0/jBk4UlU8LkBhQ1Jvl
CZO8Vq8ApVT63KnbCQkIJDmP76fUHOI3mtlbDzSNdzsICd5CUhQJjXPahFI04kmX
/WDqM9dkFjtaPVwGGdcL0TXogJm2SfAwTIGlP5mKUQKBgQDmHd/GIDGGw6x3Jbfj
uMdWHE2Ik+uV1fajU74ScfcMzfnjCH2123f1/CAlV1mxgmnKsRTIpD6vyGuDmezC
4GrabWCcqWrxzciD1tSDyUwRdcuBPQJWRyU7QBlh7DSO/O/pC4YK5/YgumJLxE6z
tnYA7NdVKxyJMPUXbcm8HnvQ0QKBgQDSUTw8/2PBJyfiUf3dNf5RmRj7YxYyA66Z
9jOFaLpamqUK8bPRe+dVU+pY7OGbz6bnBgo3FjaD7ZxIUEkvwfpTcej5pUQZOg01
IFjvizebKuPCh8OcT1N8Kf7LoO5KCEn2DDMoo32u9JSc+QXTzL8fh9LxRXwGwySE
8VQyV6QsiwKBgH9C/kKAA2PYS0UJYw1ZX0RMGsTxqVQQJHpnEbL5u/sk3pGkXIF6
gnlTmiOMj1oQGZmzHR9pVhG48Vyyf7G2kuGBo/B/8VkQyVfAM38+kAcH8ifjKau1
ZO3DoUX6b4a0c6maaeMWLZQj/8CPLEdPHSudz+iWEw38OU9ftDWJVkhBAoGAWfcJ
74XHpB4dv4AXOgMYsMrulspB+k785zsbutubq5ovM0lFBD0eLa/htPxPXZkr6fL5
d6r20uRXT8+YHue5LuyxHekGAkyEzc7yfDVvKd7k4pe0oy6Dpb9JEM3cYKq/CJMb
ZyzYXHifAa68QWd58gDlyPNSJIphGsUooQhAYBUCgYEAqlCgftdJdQBvbxJZTrq2
ELChVdm6Y0aoomer8jm4s9L90ytg89W7M/+unduGYng4tdty53D6FHlt+CHENCKL
/FKj3SHmdcZM4HpFUBVtXEGLWjAYG5cOsytGYlrYNLJ6ZiGeQaO4tbgMDttD6enI
+z6rTGaLWmPp33RtlsyZj8E=
-----END PRIVATE KEY-----
"""

INTER_CRLSIGN_KEY_PEM = """-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDkjpyUJXNdEALl
jt+uwudD64NR0FhBX5jttssFw8q8wUO0VIi92wpm7DVowN/PX/lqBd/Liumfi1oq
IRd1APhKdXPF1Mf6H4nQ5agJvSyJEt18meeG8xep+IKghPlXfAyQnX5T/mz2anFl
PwNhSH3I7IaPCdYZx0fVYHcfEMH6JTSlnhQDan3CbHKaGAu00E9dGNqJNFMCn8xJ
K0ZX0W4G1fuM/diuKi3isebb1cJ7x4XGF9FjjEKN30MgK945nLZEMDxOmpBaYLfn
Elh7uxzjbs8mFF/AXve+VH5uZED9XP4zwSNTEoecvmhZcPbfUgPYUEKbiysr3FqM
ZSYf2KnzAgMBAAECggEAIe9Es0BOPSKL845/iqCYmr6EjGitOeOaPq01vqDf4J+n
UEZ+2QWFKVvn4CpG8LDUxNejb0KsRkp39Cpeo7olo1SGAHkDyhDVswTuKqgCTUXW
SdJGDITaXhuuNWwY41LhQLWJlKoBfqK2PGybMV4EgwkPP3yb487xTO6aMSwZSOzm
YoIU6jiOScXvxQNz0cW/RrJZ4tW7OMTd0Zd2Y7BSFAudSgy9UF6io8oJdKyUJn01
nri743O9MpHLVlu5kFF0ghYfAGw1LKpe12wJvMEX9m86/cXlkzuIK/5lCQnTupEo
llaCUrJAX9tl9acsIPmjV9QSYSw3GeGMa67dRZQU5QKBgQD58BxDDb6tqL9QZynv
FEoRtdkKcoQoB7Trf6qdVoQLkItn6uafP+2y6wQWR4f5MnCAwv8GLA9mP/zaPG+Z
kRpoQ7lI4fs1n3+3yDUMQmqHHdFk6SWn8LOiAZKEmaaWL+2MylXZWUk2/IG0YQ17
HACFCMu+dASENSxH/Ecrv+ZXnwKBgQDqGb7Q1VMw0RjQuKBztDpkAA1S2k5L/By7
O2nMUiuzuJXCU861A6+fHjruEPEkxjuJdBxyp9mpZrvHAZF3KEPgwKKzUjqgs8LZ
ostTEdeweZBGjLsQcWNXv9IrGLAE2REe/7T2khm9YnTTMp7DcCKkzT6An1/QGGK+
CgMyVendLQKBgBWilD+1q+DKb60dEQMyBWtdC2mdGhWA/Uc5aOJvv8RkgbzAF7CH
TyMittra3ElU9Wof9wVowPh/53pYquxnJBsMabiGX0F4Cg01lmn2QdRqX1rV172e
113Ztv4lcscjZPO//AhxcyYAW5XqpMkYVm8toL1eu5laJPgiPWLrHJxPAoGAf3Fw
ngPkfCj5F3fLcfHl79bPahGKmncvtQZNPHxnajnM3bP+TjEphLhSYJQ8k0aP1u6t
JgYWytAw3dAOvw3elhaI/JKBdEcoho7HnfuSrnIfhyjKFdOEv5qf8fJIsPrzBSsz
+x/if2WAMBHYuJDta0aJBkC9Sm5cIxUUVGzNGikCgYEA+SdRlx4eyAVUrLZvI3Mk
q9bRzZmNUgKOH421Giv2/Bp/XWLhBRZmJAeMK3cylxM1Au0bWybW8QjSeBTELApy
eV4zkdraBx9xagXIYwsJkxyaKpJy1+4GOthxJiF8tTMGUiy4vpwnVgkp+2gir8sI
tYH7LoCMJDReD1OSTVh6BRk=
-----END PRIVATE KEY-----
"""

INTER_NOCRLSIGN_KEY_PEM = """-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC7YdJaqQOTStqC
TTwcMpD8nI3in/MRnuZbM9rxbXBNQu3wx25zCJ6Vx4dE0XUgXA10gifncmX3yDw0
rfhVNR5cXrwwPjBREOrr+rGeFHwkmIFQviN9S/JoeSXsrZMROQrVVRaxmFioGzqB
xMLN8b7G6V2jAlOcNQE2zgvIyO/30aohNZc3qKKlSA1GyD1I0PRdH3IPB6z/y/9Y
P6kn+GgmJUesy28a1YE2GmrGqpW1B+2DEh+aZnrxUl/jG0R1f+z5shQRq7+1wSOw
7gsi72I+grZBO+9eJmjxfSoVY7GP9dT2LBGSLc4AP9ksFIGDdUbEdylkoF3zHQ72
h4plhfCzAgMBAAECggEAJYoj89C/bB9HZmplu/pvbIRKspe7wRB18tiRKOP1Zy0c
8StrF7Wm27flBCb4UZrPgUNgDjCvd/QQB7fAtAVXx0adagbTdDIw9kS7l8Ff248H
/S6SepQgBMkcjW16tK0aXAHXcY3lWQxXbM8H/e1L/JqdDT62lGlUDHA8ESYcWSe6
n70zn1NQSPcmvSeGvAKQcbhAj4A3D+H6w31JIzzSA3y3XZaRjoXXhEMyyQqIM0Gh
+d5BaAT393sSMmP4xcArvimezmYawf9AeIEbrJbDBxANOSzyHD+gCgh8s50BNHHV
CH8dKh0D05+pjIZiier4DQp6Z8etlKZjGAT0TyuOLQKBgQDiy9UgnlRP33DgQHie
xw910DkwLso5uw+tLpiegzWdNgFulkXFBJA+llTnx/CeA75PpbkeAjF4ocCr/2YJ
+fizkJrHzuJxNwbjOW53XVlbKymw6Uy1iisqwK8r/Rj76V7bMPy0NnEhsNA18Glv
1Cx0zGXYPxIlrEc+nbwxFvK77wKBgQDTgruUmx/cke2au/+o441n91DtA++GZOqY
xo11JsFiS63UOh39hHnEjqhj8I4NnMxgQC3NigFR5KkT0h4MA6e3ymEbxM+e9iwu
AcH29uXet81/LrtTgNPIcEy0uz40gZdnsaohlSLGo8Z1jiB8Od3sLxmzw1AXiHTk
11X+RxmjfQKBgF8sRn3vs1XpYEbbB0MqKdS40qbjm71qQIPeX/F1AlS8yxELQrAQ
4C9DitHRV6oXzA9EblCnfvcuedozzTA2lCVWc32R1fjIrL1B5pd0twDJNi+lV6l/
F+pS/a+qAIgGQOYewo0lo4gN2PIptzVUhmtlMv/yR71DdIpUSxsWlDpFAoGBAJMu
EaGjIFS4JwLZd1nqnmeOLX4AkxiN+ME+yQiQY1knGFrp5hyXprQkIQEKT48hYQZJ
IpY4RhraZt0oBUiO8pJt/Yz5MxCGdM++4V4dCHAmrpSlzCybJMQBgR7yfKLE2rqJ
Yr3jrUNUi7EZqXlIOykyzUvNyVnW1qC3R1ud1jVxAoGBAL0JHZO7sq6KNq2FufqM
YNb+xBcz1eqgjxT93TB+mmZ781e4pyQpNIOgsGhBrDlLDawqU11OAeOeCYMG43zN
A8TOlb9icsC1A7ymoNAjoqhhoGEE+OMwr1VdDjbRIabbeyL5zzZRRPreBxDvFWBo
XfDcB/WcHqgwRNt+sqBNkVe6
-----END PRIVATE KEY-----
"""

LEAF_CRLSIGN_KEY_PEM = """-----BEGIN PRIVATE KEY-----
MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQCnv4IZA7G/u2oU
ceu9Cu+tWBYaVYWY1HK8MyhbavqPhRTxjTswOrrnk3Q1MWha5YamwVK1IwGcanm3
zrm2n+kxBtkYf9QfPBDnbYdvJWLwaIW10RwOwK2z9nIf4B+YfQ5echf2vJcGwXSb
4VZj7gg+q1ITm+bYFnnggSB0UeJkirGaIa7wzgutc0baG9etp1vUwdTOjmlUDBmx
cyysUj/oxach+TPCOx0OyFx212daDXfEZm8k7w03BXDfArxHBDoLfS1j/jIDtwVS
xXmUCtLHfmwgAJ8Hikzkc0FV2nhhtjMjK9WBa0KPwvtmygYQ77hr1+wIC9U2Ehgo
ce9BzJApAgMBAAECggEAPn0XPDVFmkx5BFu5uwbfaP4W4cF5DokZoqUWBBQSebNU
LXV0lo9SP+EMmaIpoy68ndMHeeWr/HYnmILAaNkiF7ra1YcM4IPSMjv5hzxwomj1
Ylke8Bnzu0Xzlo85hecUGre62JXBVulrBISJV5l85ceRQC/D6zxYM3s2uU4lnVj4
dUl53Kesl03dclvl0fyW3cl7jIrGrwcov4tR8ab7FGfXrFIiX1HXvypp3dpkOBF9
srGnRLfllgWZUGt9dTbf5hKzUcJgbPZFeNfkM7T77RGNxxNv1i/8tCennKDj/iJf
GsjVM+MFB734UvJHT2ic/jSeUzffFXTdzTJINCk/7wKBgQDd8lnY7iEudByVA3yp
FOdVib/ZRHhwnioJKGaqS+mZlSo/8m0xBSvBUbjPd/YDph+nSBIFCufsMT8EEQ/I
9DghmtfawSFtxqvVpF+4NRtIouZiIEyK+b7RJiUAH2reBIXI0D0/fsGqtpgawDwA
GSPhc8ReMPvrWontmoZdYOMsxwKBgQDBfFZ5uh3ypwBwf2v+IweqntRFRCa5zUC1
ViIkXAFlmQJsBfd5diVqOubNREuIg65ALjyfQwVpTaz40uk0fI3kLaepjMF9kzHS
ly7S7xGvloDols3xZs6gBKm6AM99rz5WlnEG17xWdyDs9ki1SWTU3lZa1SgqGhIo
yBXNojYLjwKBgQC3W0X6vw/OjsImIRFVph1PP1OtLLHLZ/8WPbhDLB+6b87NsXuf
qcPyG49wZoxpi05PJm0smUvshuqmc2n0/D17Fa8J28/AHwdKolcpmUYHBf0KR+C0
V4rawoaCyXHCJT5ZHh/bWt6oJ90VDIcFFb/cu4UYw6L7VnroPmaOskI7dQKBgQCl
ovPEsXgBysWtiOfCX+qoPeAqi/U7CmNi2aHQzpLDjwFryI/bAzF0h4geIWEoNNJv
TyXGkXGIfF6fhK7vp9EwjYUCuOTLw77bMWF8Yf0Z/BXPB4q2JfStOFC0Uw1N4z/S
kGeiG/23Z7RgJ1aHG5WeRdhXWjNQF5GEKAXCTCNIMQKBgQCRHoRn4tLz0ZfUXyW9
NM/vH+Wau6bLykVevlIajSPx2u+uJESHn0ydcZgRJ5z01c6tx76EKIFmT5l4zEZ7
4yVK9SSJmgxNXtx6zFG495ubF/+ay2dzp4GpRBOjDYfisILpNqlYHHEEQOf2pZ9h
V8kWV3sx4hvJoy3H5OaUJc0bxw==
-----END PRIVATE KEY-----
"""

LEAF_CRLDP_KEY_PEM = """-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCiMXx6U+qqL4H+
FefZN/sxYLE/g34CcYgncapm6Iub33x7hjRpfdx+jkTLj6p03a4NZMUj+mwFGXyH
lmecc8YoRC2NsvYGMj3M1diCchboHkYZdNoxMoPV1c1L9bslvHATjZwvDx9cgJ4j
qpvjIfPbDlSb9eok1pKyKJQFGWrA37j7RRGs3pgiqLuK8+Zz0DY1axW3zvqcXUk+
GWJ1CQ1hYnxtijT9uza0NieCnyGf1goyXLNglSQIXJJwPAeeNM0QEXOEWVwiISS+
RqoiELdgxT8nWjeGg02O1qCp8eUBSs2QZQUX99XBGBIaDpK9uNww7/KPHTKJ4mao
GwNJPV0/AgMBAAECggEAAUIwKGoNBR1Ld1Z8Ty35KtEs0QmDGlcqhwIXYVK5W7zW
DU3HW0yAd4OmZqbCw/0G+iELTY1FSsLWjkhB15Up1We4cZnDryBhHF3C4sq7Vaic
l8CZNifbe8jyG0uzb/WD2WLXfo4ZvXi4oNCeWauWolgD4zW8VzTH5+QiPqitY1HV
UbEyzKKcRcZF74XXBYB12FKBPqBObReSFg3ld/XWvQp+5p1Tq+FlOGkRwk26n6qP
Re3p9b0s+0D6+1xgTpY5zXcwtops9YACUVD80w1x5MFOhyBCNXrE+8kBvbbutNQS
kjJV96wrBWnthLuH1v/3OwXH4LW9ryhLOwAvL/9eLQKBgQDgRaGsJPJm/+TeqN+e
SuD2NudwJMcX15cSJJsmi4nbs8u6lnCEKwhT8F9s7ePbnrMzO0RIvh2evtvB8tRr
yzWF/z13oT6YXm1rFp3BGrMwlDUtIOeyHaXEyKkYMQSrtbH0P+p5gpsn6zLx8UOa
R4OZXLbrNdJNID9TGA8rBtFnjQKBgQC5I5NkRBWfOvK9ZksFeg7rB0COTK43LY9Y
PgvTiFsnIQy9YKhDB2FOrT08/ku0z5KMyP3xnzixcRKmU5p3vrTwTZsXDyO8Y3fd
Si+5JHcW7uSTa24tlpJyJoI141yMlitgNvOhjaXTrFGSc3scfy1e68g2PI7JF7xT
qjbEb3eu+wKBgBt9TXjNOo0XdpU4O9iqMvLa/flH+MNaYKEPrZjG58nvOtyPDhGV
OLsZLokqi5Bi3nI8JH2KGBVaLZOTUSg5S0E2bPFZ6VNB18WVixadUDN3JyF19BBo
A+af9eIKOBlX9w0KPJJSSPRWpvLy8Kdbdqz4qU8hVLqxZjGoNEvA1TANAoGAfOQk
37SWKW4opd/FqBIo0h94SwYrWAmh81MXw0DuzLyczXvQxjbKS7rN3sFk7LpCEDre
JArWwcyXWiB3yWOax6hygOGzp7gp+du7yddxhsTs6Q3Fq3cxn1Oe7x9LJYOAC03G
mCrnvJ04AeVpHVYlkyzXSM8dO0aJwp7Gh0jyodUCgYA6dlB4uitrUNgDDc5y0Kc1
QWE+ohu4etPbWj5BDXKoJwbG9Om85OE3JcxpkvDGEkzAH2LqnttJyyOpRzGqwD51
mgLB/GGPridXxcE6seyRFgOddCN+SDplMhg5ydXy06i9mvX7+ze2l3O6QEZEkeNr
/aENjY5BmHIxTKeW6XsIWA==
-----END PRIVATE KEY-----
"""

LEAF_NOCRLSIGN_KEY_PEM = """-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDOiianUcE52QHB
y9MdqeMt4x1WEJIU6voRtAuis7d5kr0L6V9vaTmmejPmqDbtnW5N/PQ+bi/hlF67
wNcv9nXTTTMzoaPa2X7uBv2I6Y/QrRqS5bD/8PwezpqYso967scR/RpLsHOeIAWF
fBU522Ttj35B3JJcgeJaq9/hV52wHUWWc/j+/2T7aFf7B10/NJ7kimTrCdl7nhhw
v6hsQVhpcFDFqIAqjNPybSNpU4PmTA6ZV1AaVB3I+BDTWGbRo6SPqN1a3/9CnTZQ
FjosslkUG1LNieoHWhEUCAvTDd13BfqGo+2PcMVsO65+dji3skZiJb4XZqObjrMU
5RjOcGaZAgMBAAECggEADtxi3pwjzpE+Lp8HUDWWmokiZbxofmfP2XE+exT6SR2V
S6Odkcsc8k0NqfWFikBPHP5r5ogQGueNj23XLeTKYotF+VuSih8nImJPYMgJaIbk
CBjNzQSeZ2sPNZh3eW1v1mwDNdn637BHJW0hq9mM4Aj4mS2RPGW+Dy8Zu0GOH4tC
cTSXtjZVro9CwPQcqkT0DQhKiO1mjh5VFvlzcMPNf2u31mQXzpXC3uzOIM9ztchR
u21MsfUQd+VaDMGEqgdfqXnmR6SgHFteIziDV2fm2FH3lLd6tbnYi0Gmis1X3xkM
6Br24/IL4EJ/YPYGs45rLdjfv7W1H0UvXwtvVDk3WQKBgQD5Uknb2WO6W89L74S/
ENMn579t0GzoTYdVvNhTnMVEmUSPYAHgP6e5mlqdknd2q5o6iN0rKEeFvz/hFGm8
92e+uuD8ju54W0lw+52VkfjIkCxEsPpA301+sMEMDmygUkSyXS3FPhe8OhMMpKFq
LYS8gsN2tt4iihfOCrKEPNNTfQKBgQDUEnz2o7EdzapLQL94L27UZH5VJdq5V7wW
J878ve+k/0FEXQzCPudN07u2NF48VuVt5/nMYtkF+bbHJvJHLfkHzBBk9gXKRvxW
ryAz55yyPnQ7RSOTRE20O0F4+JBDm29i+OgxHhNBzYOBK9AaGwqsFmA16ymkp+jr
0EEp77+STQKBgA3BnJGkzPvbVPS7ondoVj49azKD7X5jyLPSV51FaRz/1+iRQ4k7
+eUjvxwymKTTkw7nvkxDTK70fnHMzoi+ndrINOG6gS3i24qAV5GJHFD0o3ymxygN
KhlzZzNjGFtALsoomf3oDyczjRmFe+J/4HMGRAKo1FjThby4mmDxejeBAoGBAJbP
Uvhd7d1A3UJImCCSsqDtmf1ZPxKgtouW4ZZQR7QxJSvFdAZKj1iEIq/uy7jjHqKq
3bQf8y9Rpk43fwFpSvflgR8a6YEwO9A1F5Rxt7f1u9hOUX8vioaJwQSS1pyNU4ex
dAVB2vXnfQYFryZxZoR8IQ0W33VNRtEzNrPo33XdAoGBAIpjFisSE+ZFUiiALzjO
8ln01Yi1xxDxQKTSlMkjmODYvbVqTZj6wODqV7Re/XMQUkMyRO6v1Jp2ub2gR1Wo
o/nzQcvn4Sl669E8biAR+y2mUAxaSSnX0YAkqbekcj1odYoQZkTRuYZo2sY8SxNn
kV0nlnCpw0DRG9hGfAgrDwUO
-----END PRIVATE KEY-----
"""


# --- F55 (phase-51): the keyUsage-ABSENT intermediate and its leaf. Same
# provenance and the same disclaimer as every key above.
# --- F60(b) (phase-51): the malformed/contradictory CA.

INTER_NOKU_KEY_PEM = """-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC6T5qiPq067KjK
Tfxf75f8wG+XQw62xhxP6y6CraxWTJsWKK8QGI+PzULbvfDdcS6KR6i7M0/GbxSV
aVlHVNXA5TfwZbz4Nu/m6S6hmSaVn0Ya5O1N2Aipcjl+pr9Y5fRwcXyOTY9+a7Z8
zQFfbdezjgwHW61bLiAefwHqQLL5LKD/1q1YmKY2svOEypdtwvh2rqQ58Xs+cf/L
8aJX3pCYj9MeVp9Z2OSrB7l+Z+e7FNkirsvuVLjZHARcTADHgU/eC/1bPcW/djtG
VaC1j8GiCKkaZLNo7/9e0Kem7BJPqWjOyCMljmLS0lORpsBg5mTsmK4UmUbwN68/
DLPjFkWFAgMBAAECggEAQAoFplfx1kurVuJIabdioBuLH8yKXbOp3qSBd6R3lLA/
LHYuelNe4ThWD07x4EO7kgdAjZv9A0GCKzsrV1PnJycCu8xUkGLdwhqLjzsjopE9
GRhwIu4rSAb9kTWClu1vwEiZ3hX1ySf/8VTL8H/CgTLFmq/d3fX0T1crktfLTM79
yAhZ/Y9wWjheNhgz7GclccJ9qvoI9psKym+SW2hTBlEavTM11k5RWVIHMUR5TK4k
ouW76hwoz2ZVMEBu0lT96QuNd2XF6h3VDdI+JZWRSmte8s//eWxvAB+K8BZTVfIC
+qrMAu2NpcnAjg3CrCxYev8hZn7OencnDUFNAVhCXwKBgQDzAQsptfvo+hoM/xFA
mlqiY8G1fIPTGJ2rgTplWrcq5xpSl91N9bJB+CegvVYOL/HIiZngqrMk/cU/6km4
1YtIRsqkUmkj6/uP6St2wd6WDJEnXtf7G8HhxUICspRDhaoPyp03N+nL9gmYSlCu
OXzi7BXI1Sc+pcp8ZtQGfOFwEwKBgQDERmC2HHTW/gcxVDLtNanO6CrwAehqai+p
DyJCUx+bk2vjG6HTbZfM545xgbkDeH6M33Ihw48t9RD40TOM73UTO7W2nu84pOqc
xqa8Kw0gbJZXAY+GD3YPDA2Pqt/4m2BznkhhuqITsNPpBFEaI2CorUQQHOao5o4G
Q13XdQuXBwKBgQDtBA2RR3FHUfnqxzZanrLcRRSBIcuqMA5JY6hDh3G0pOilun4e
NhECCzsLquCuwdDAWoX9/kDxSZwEtU3mvFI30uR/1em7IcaiHTlJgdrK28MQdY+P
2wkGXtH9s5StL0gqUuN5Ik4lo/rD0WhUJrpK0x6EXnMCxc5z/A+9rDxuhQKBgGnV
9VJd19cSVbiQyYFFnZI3eOfAb53qMlb8FSIkhneyr8tf3AwRHXjdJ98m7yfi8oJt
/rIcLHnAmu1AiYamnL/JQ7v8ycwGiYgOSlxO7p4cT5kM+2Jztdp5Slq87la1GfZe
VHN2Wu05PZt5u23Xw8hQ3dG+c9aBs/XGn7SNpblDAoGAU++oC0f3zmxR0lENJJ7L
sCJFgI1SO7Nf7Fh0/vQ2CJHuaJMeVGT2Hw+x0g/cIMOZOI+C5bT9+i2Nr1//aMsM
uRRmAlpZ+Pl5V6CtdJBElX8aHpDFFFWhALtIdAY0WiTXZ2KDfgvSBCEs+IqSZQG+
4Rv3RJyB3c2xzS1/SOsc4Ms=
-----END PRIVATE KEY-----
"""

LEAF_NOKU_KEY_PEM = """-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQDGzQdTxND581+i
SCBYL89AwmuExcvyJa/hAdQT9MKPoLSTbezFgAzszD2c87BT0NG+1af+KuCnCMKA
HWnW3yS7L7ZQTMX/bzyxOOGFFpUo9pgEnwpcdSPK9JYnlXYW/HxAh5/hIP2mKL+8
Nh7p1cNuek7BZAKD4PVs7cTKwDlcNukl3pj0/ET/oFVpHLVl3E+Zv/zL5wNkgYam
yU4stg1EOc/UnJYfUPBuH97XVLAjQhxP7hJJcJe8dndFLwX4fjRt42a/mNz929iA
nvvExcbvozNztyn7JR9z291RzENoM9l5h3iL+f391edCONJ2pHI5bul17u9A6P7/
g7eXIUj9AgMBAAECggEABrA8tsndzF8EC2zDoSaV9uQ4+P9XndFnR1nH+xY7xS3K
DYDABngJAfQwKXuTEIrWF7EkBQYyceQ2o61O/jKaj4Kq2VeakhNka8OnEL13rhMN
KRtPCQPMPRE5YQ0OAKF6Ll3xwtcDjvzQJe1TNm5MSYdWEXsY0OY3aPS0miwD8gZ8
DzfbjurgOUg9s5nuZ0kfATEin7fN+NbABYvRoZG0iWISjyrawo3zKLKpIcy9drqD
x7hcTCOAOimbnxjHaBtaGAc6GNcchbUM4GWt1+uxqJ/vGPyECVYGqLyVlFePvrCf
49hPqi2pzFKIoVH8Fq9frtlUPrAXq+L78Xruw2gbIQKBgQDq254D4BUHHWcVtO6m
P30f4Q1ig/O9kr88UemHWM6AQSljSDNENKjoNRkQwaS7Ez3I2wnkXKWsgAsz3He0
TiVyGnF/uWPU6ld0zGKodyuo962xG3p/P3ijj3FZyoIjpLv+S2YGEQhyhQctsRMS
3flMaor4QVPFdatG+hUXvzqnnQKBgQDYsncjeIc+8u/h8GJETOfncHbyThe+/wEN
Gb9LmJ+QSpTRh//xzXhz+DkCwwuyhOhBvif0AijuMnGJ5jKSpgzD1HKiBYPuPC8S
fFC7VxlPRCRBBCyQgsGRLTHd2zxf/31miapD4BSe5FljRSnNpQTGrHRV+2A/Px++
uH+dc+5Y4QKBgDiS/MZ8NTSIjat58T8U7iQ+x0Wi+ZUCA5UZOkMDToMwrzKeUtMs
SD/fgpE5xF/YK7r1oLVk5/fcxFYS1rcVWAnbetnEo9rHNJgLH/zq8BZ8y1dtgh2j
AYGM7UYu9Rh2914zdXkio0FK8Ea+cBTMaeBRjfZtbiuTw9WOKoSU6SE1AoGAMVN+
dx6KerPGEwIq/C0dZLte+Az6rslRLfA8cTLOr4OqmH+CrUH+Nze7kntfgEVssPpe
EdckBgSJf8BcprERmsoTRfpHa8VbWbPelKZg5oA2WG5gzoxVD3zgKwYqmheILf0M
OuMZDoogJs6pk1wK2EaNPL05zWvLCBjrQ6YODUECgYAwPpfSFew6TTZUayAcZJCV
PFoGZcHPmxSiceMJ1YQjdJTv7XL2kTawvjo6OdQHRFDwLpSDxlcpyi1PqTXCM4gw
Eh1RlpBiN2aoK4MZZUjyYDQ3oPWNWBArzjGy7lobrpca216Mu3WXIffU7pOWaUp+
+uQjXW2yoW8+7kSQ7SkWNw==
-----END PRIVATE KEY-----
"""

INTER_MALFORMED_CA_KEY_PEM = """-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC3aOBkEFs9HyNR
ECaXt+P4PesL0hsayca1INORqNG4eLUqFDr4zFzYRvaVSYSj2BofjeFozVhHPG8M
gOfThVG1+IQVTrIAPsDu1LAHTUoCYbuWMIEOl7R9+beWT22MFV8KsQRsssaZI8T8
wxP3bH73jpxS8vw9BUTYmNV6l4PpX64VdiWRED3GoqDY+DOzeg3d3moTB3bBPew+
0JalOjSJXfIk54mwnjKpBJLm/fZ/5F17mh/JIIZH+KF1jTgmOBVsJIh8Ns7oIFNL
IvK3/DIziZXFZAAPf28Pp3ko/EWHraqEw3MgP73MzpTKmsJhk4dn8568d0StYwrR
DrtRqWoDAgMBAAECggEAWeqjM7tnF5iH5mp+GRxwuZeVf5aSbjgk4FDHGF6E7qTS
4F3JqYb7L5betcK0seS0reqPayNc15Ozt5QM04DFVeqPyqhpkMAzMwN3hyTnlzJV
1mRSKRbLyITf8eNfx/EERsgQ7L9aqOJF5Us4IzGAL5qtXec9LSdDoNKQukjExWir
9e47p+ZljD/TN8woGmuk8TnQzbwW7cDsh2HtVRjiqFgRzW/1R24L177Z7feAQ0f0
K50a3WKc5U6rkNitYR4YlkY7Z9o7x4DWRc9OPadvBLLjwcBXww2Azx6tnFBIM99Y
8WmvKP+8tfb94kSc7oBD3mRi9gFbFy5YySBAS8B/7QKBgQDeSEQEkjWydep/wlsf
yAYxhDniWMycKjWqkWRVKyvBTeqKDTZ1+m3+lSzVeFdO36sfTsHDX4VTw1r6mTrj
GkHcNRW+QCu8JkFoWxSHLHwwdwfcr/9wIxMer0zVIo3IAY08FWDevnQVLGlgrOnB
Wiq0OEo5qWpRdvthTckVt9bWHQKBgQDTOxj2SIbxZJ6nEbPUnYSbJ7DXEu6JkziJ
9V46EQsuarjAt8QM7I2+IeqN7xRuxQt8BDEEtBvh337ae8xcTpaf7rM2DbSmY9SV
2Eu9UoJfMGkRLpH0U3fM84XIxsYp1FNpGkO4TJgbC79M2IgFMPGdzQv3EIGgB2NA
lOaCLiDGnwKBgQDJKKMNc/fOWjHujsgCefVI6H2t3mjExtosU0l2XgxJ8qZXat+n
YwwaAYxCX5a7lWT6fz6Q/afeBwQVl4gYMBH8I7paox4xNnmo5AwuzE5TWdUuncPQ
Cp3p/gDNPWkL4IOdJUe2LKtG4hs3VyRwS5Yr5Gkfx7akpMWX6FobInT07QKBgEBo
Iale3f6WzqVkiW/IA9oHVS2PMTqzsCaosW2FKuKhWeoxXk8Ohf22e72Ba7qGzISp
BoVHocTSj+xwmRSARf9IGk7eCPo/MF/sTtPQZ748lZzOlWRpWvscOevJaDzy5+gB
QSrd9/cvMspsr7Tdgp66Ge7gkpXICuRzQoHpLMdDAoGAYqUYBydVC/IFaQso7pEh
9uyMmhk9ViiPMI2JatAd7lKTum9BOB0IfBztNLBxhoNS5Ww52M3dVO24F+Xw2lt1
PNJYnwfu44EDaSIOdnqf2quylCaWllYfm5a/JwYIpOchyGmm776UyN5AWZR4UEt0
50C4cJ5gr1c1SEje75PjsYY=
-----END PRIVATE KEY-----
"""


def _load(pem: str):
    return serialization.load_pem_private_key(pem.encode("ascii"), password=None)


def _name(cn: str) -> x509.Name:
    return x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, "US"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, ORG),
        x509.NameAttribute(NameOID.ORGANIZATIONAL_UNIT_NAME, OU),
        x509.NameAttribute(NameOID.COMMON_NAME, cn),
    ])


def _ca_key_usage(crl_sign: bool) -> x509.KeyUsage:
    """keyUsage for a CA. `crl_sign` is the ONE bit that varies."""
    return x509.KeyUsage(
        digital_signature=False,
        content_commitment=False,
        key_encipherment=False,
        data_encipherment=False,
        key_agreement=False,
        key_cert_sign=True,
        crl_sign=crl_sign,
        encipher_only=False,
        decipher_only=False,
    )


def _leaf_key_usage() -> x509.KeyUsage:
    """keyUsage for a document-signing end-entity (mirrors test_signer.pfx)."""
    return x509.KeyUsage(
        digital_signature=True,
        content_commitment=True,
        key_encipherment=False,
        data_encipherment=False,
        key_agreement=False,
        key_cert_sign=False,
        crl_sign=False,
        encipher_only=False,
        decipher_only=False,
    )


# ---------------------------------------------------------------------------
# F60(b) (phase-51): the malformed / self-contradictory basicConstraints.
#
#   SEQUENCE { INTEGER 0 }   ==   30 03 02 01 00
#
# BasicConstraints ::= SEQUENCE { cA BOOLEAN DEFAULT FALSE,
#                                 pathLenConstraint INTEGER (0..MAX) OPTIONAL }
#
# so this decodes as cA = FALSE (the omitted default) WITH a pathLenConstraint
# of 0 — which RFC 5280 §4.2.1.9 forbids outright: "CAs MUST NOT include the
# pathLenConstraint field unless the cA boolean is asserted". The certificate is
# additionally contradictory in that its keyUsage asserts keyCertSign and
# cRLSign, i.e. it claims to be a CA in one extension and denies it in another,
# and it does in fact issue a certificate in this corpus.
#
# `cryptography` will not build this through x509.BasicConstraints (it raises
# on path_length with ca=False), which is why _build() takes raw octets.
#
# WHY IT EXISTS. `idpCoversCert()` in src/signatures/LtvValidator.cpp reads
#
#     if (covers && idp->onlyCA && X509_check_ca(targetCert) == 0) covers = false;
#
# and X509_check_ca() returns 0 for "definitely not a CA" AND for a certificate
# it cannot classify. The two are indistinguishable at that call site. This
# fixture is what makes the resulting behaviour observable and therefore
# pinnable; see tests/test_f55_f60b_crl_fixture_coverage.cpp.
MALFORMED_BASIC_CONSTRAINTS_DER = bytes([0x30, 0x03, 0x02, 0x01, 0x00])


def _crl_distribution_points(uri: str) -> x509.CRLDistributionPoints:
    """A single cRLDistributionPoints entry whose distributionPoint is a
    fullName holding one URI. This is the shape idpCoversCert() matches an
    issuingDistributionPoint's fullName against (GENERAL_NAME_cmp over the two
    GENERAL_NAMES sequences), so it is the shape the fixture must have."""
    return x509.CRLDistributionPoints([
        x509.DistributionPoint(
            full_name=[x509.UniformResourceIdentifier(uri)],
            relative_name=None,
            reasons=None,
            crl_issuer=None,
        )
    ])


def _build(subject_cn, subject_key, issuer_cn, issuer_key, serial,
           is_ca, key_usage, self_signed=False, crldp_uri=None,
           raw_basic_constraints=None):
    """Build one certificate. All non-pinned inputs are arguments, so both
    chains go through this identical code path.

    `key_usage=None` OMITS the keyUsage extension entirely (F55). This is not
    the same as an empty keyUsage: RFC 5280 §4.2.1.3 constrains a certificate
    only when the extension is PRESENT, and OpenSSL signals the absent case by
    returning UINT32_MAX from X509_get_key_usage() — every bit set, i.e.
    unconstrained. `inter_noku` exists to exercise that branch, which is
    reachable by no other member of this corpus.

    `raw_basic_constraints` replaces the normal basicConstraints with verbatim
    extension-value octets (F60(b)). Its only use is producing a certificate
    whose basicConstraints is malformed or self-contradictory, which
    `cryptography`'s typed BasicConstraints deliberately refuses to build.
    """
    subject = _name(subject_cn)
    issuer = subject if self_signed else _name(issuer_cn)
    if raw_basic_constraints is None:
        bc = x509.BasicConstraints(
            ca=is_ca, path_length=0 if is_ca and not self_signed else None)
    else:
        bc = x509.UnrecognizedExtension(
            ExtensionOID.BASIC_CONSTRAINTS, raw_basic_constraints)
    builder = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(subject_key.public_key())
        .serial_number(serial)
        .not_valid_before(NOT_BEFORE)
        .not_valid_after(NOT_AFTER)
        .add_extension(bc, critical=True)
    )
    if key_usage is not None:
        builder = builder.add_extension(key_usage, critical=True)
    builder = (
        builder
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(subject_key.public_key()),
            critical=False)
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_public_key(issuer_key.public_key()),
            critical=False)
    )
    # cRLDistributionPoints, when the certificate is meant to NAME a
    # distribution point. Appended last and only when asked for, so every
    # pre-existing certificate's DER is byte-unchanged.
    if crldp_uri is not None:
        builder = builder.add_extension(
            _crl_distribution_points(crldp_uri), critical=False)
    # RSA PKCS#1 v1.5 with SHA-256: deterministic, so the DER is reproducible.
    return builder.sign(issuer_key, hashes.SHA256())


def generate(out_dir: pathlib.Path) -> dict:
    """Generate every artifact. Returns {relative_name: bytes}."""
    root_key = _load(ROOT_KEY_PEM)
    inter_crl_key = _load(INTER_CRLSIGN_KEY_PEM)
    inter_nocrl_key = _load(INTER_NOCRLSIGN_KEY_PEM)
    leaf_crl_key = _load(LEAF_CRLSIGN_KEY_PEM)
    leaf_nocrl_key = _load(LEAF_NOCRLSIGN_KEY_PEM)
    leaf_crldp_key = _load(LEAF_CRLDP_KEY_PEM)
    inter_noku_key = _load(INTER_NOKU_KEY_PEM)
    leaf_noku_key = _load(LEAF_NOKU_KEY_PEM)
    inter_malformed_key = _load(INTER_MALFORMED_CA_KEY_PEM)

    root_cn = "PDFE F47 CRL Root"
    inter_crl_cn = "PDFE F47 Intermediate cRLSign"
    inter_nocrl_cn = "PDFE F47 Intermediate NO cRLSign"
    leaf_crl_cn = "PDFE F47 Signer via cRLSign CA"
    leaf_nocrl_cn = "PDFE F47 Signer via NO-cRLSign CA"
    leaf_crldp_cn = "PDFE F47 Signer with CRLDP"
    inter_noku_cn = "PDFE F55 Intermediate NO keyUsage"
    leaf_noku_cn = "PDFE F55 Signer via NO-keyUsage CA"
    inter_malformed_cn = "PDFE F60b Intermediate malformed basicConstraints"

    root = _build(root_cn, root_key, root_cn, root_key, SERIAL_ROOT,
                  is_ca=True, key_usage=_ca_key_usage(True), self_signed=True)

    # The positive and the negative intermediate differ in ONE argument.
    inter_crl = _build(inter_crl_cn, inter_crl_key, root_cn, root_key,
                       SERIAL_INTER_CRLSIGN, is_ca=True,
                       key_usage=_ca_key_usage(crl_sign=True))
    inter_nocrl = _build(inter_nocrl_cn, inter_nocrl_key, root_cn, root_key,
                         SERIAL_INTER_NOCRLSIGN, is_ca=True,
                         key_usage=_ca_key_usage(crl_sign=False))

    leaf_crl = _build(leaf_crl_cn, leaf_crl_key, inter_crl_cn, inter_crl_key,
                      SERIAL_LEAF_CRLSIGN, is_ca=False,
                      key_usage=_leaf_key_usage())
    leaf_nocrl = _build(leaf_nocrl_cn, leaf_nocrl_key, inter_nocrl_cn,
                        inter_nocrl_key, SERIAL_LEAF_NOCRLSIGN, is_ca=False,
                        key_usage=_leaf_key_usage())

    # leaf_crldp: identical to leaf_crlsign in every respect that any test
    # turns on -- same issuer, same key usage, same validity window, same
    # CA:FALSE -- except that it NAMES a distribution point. Phase-49 shipped
    # the fullName-matching block of idpCoversCert() with no certificate in the
    # corpus that carries a cRLDistributionPoints extension at all, so that
    # ~36-line block could not be entered by any test and a partial
    # implementation of it passed the whole suite. This is the certificate that
    # makes the DP branch reachable, and its pairing with leaf_crlsign (which
    # asserts it has NO CRLDP) is what makes each DP verdict a control.
    leaf_crldp = _build(leaf_crldp_cn, leaf_crldp_key, inter_crl_cn,
                        inter_crl_key, SERIAL_LEAF_CRLDP, is_ca=False,
                        key_usage=_leaf_key_usage(),
                        crldp_uri=LEAF_CRLDP_URI)

    # ---- F55: the keyUsage-ABSENT third intermediate, and its leaf.
    #
    # Produced by the SAME _build() call as inter_crlsign and inter_nocrlsign,
    # with the same issuer, validity window, key size, signature algorithm and
    # basicConstraints. The ONE argument that differs is key_usage=None, which
    # omits the extension rather than emitting an empty one — so the three
    # intermediates form a 3-way controlled comparison over exactly one
    # variable: keyUsage asserting cRLSign / asserting-but-not-cRLSign /
    # absent entirely.
    inter_noku = _build(inter_noku_cn, inter_noku_key, root_cn, root_key,
                        SERIAL_INTER_NOKU, is_ca=True, key_usage=None)
    leaf_noku = _build(leaf_noku_cn, leaf_noku_key, inter_noku_cn,
                       inter_noku_key, SERIAL_LEAF_NOKU, is_ca=False,
                       key_usage=_leaf_key_usage())

    # ---- F60(b): the malformed CA. keyUsage asserts keyCertSign + cRLSign
    # (so X509_check_ca()'s ku_reject() gate passes and cannot be what makes it
    # return 0), while basicConstraints is the RFC-violating encoding above.
    inter_malformed = _build(inter_malformed_cn, inter_malformed_key, root_cn,
                             root_key, SERIAL_INTER_MALFORMED_CA, is_ca=True,
                             key_usage=_ca_key_usage(crl_sign=True),
                             raw_basic_constraints=MALFORMED_BASIC_CONSTRAINTS_DER)

    der = serialization.Encoding.DER
    pem_enc = serialization.Encoding.PEM

    def _key_pem(key) -> bytes:
        """PKCS#8 PEM with the F56 test-only marker in front of it.

        The marker is pre-encapsulation-boundary text, so `load_pem_private_key`
        and OpenSSL's `PEM_read_bio_PrivateKey` skip it. `do_check()` reloads
        every emitted key to prove that, rather than asserting it from the RFC.
        """
        return KEY_MARKER.encode("ascii") + key.private_bytes(
            pem_enc, serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption())

    artifacts = {
        "root.der": root.public_bytes(der),
        "inter_crlsign.der": inter_crl.public_bytes(der),
        "inter_nocrlsign.der": inter_nocrl.public_bytes(der),
        "leaf_crlsign.der": leaf_crl.public_bytes(der),
        "leaf_nocrlsign.der": leaf_nocrl.public_bytes(der),
        "leaf_crldp.der": leaf_crldp.public_bytes(der),
        "inter_noku.der": inter_noku.public_bytes(der),
        "leaf_noku.der": leaf_noku.public_bytes(der),
        "inter_malformed_ca.der": inter_malformed.public_bytes(der),
        # root_key.pem is emitted for F47 items F2 and F4 (phase-49). Two
        # scenarios need the ROOT to sign a CRL, and neither can be built
        # without its private key:
        #   * revoking the INTERMEDIATE, which only its own issuer may do —
        #     the case that proves revocation is checked along the whole path
        #     and not only at the leaf;
        #   * an INDIRECT CRL issued by an authority above the certificate it
        #     speaks about.
        # Phase-48 already held this key as a constant in this file; it was
        # simply never written out. Emitting it is purely ADDITIVE — every
        # other artifact is derived from the same fixed constants and its
        # bytes are unchanged.
        "root_key.pem": _key_pem(root_key),
        "inter_crlsign_key.pem": _key_pem(inter_crl_key),
        "inter_nocrlsign_key.pem": _key_pem(inter_nocrl_key),
        # F55/F60(b): the CRL-minting keys for the two new intermediates. The
        # keyUsage-absent intermediate must be able to sign a CRL at run time
        # for its branch to be exercised at all; the malformed CA's own CRLs
        # are signed by the ROOT, so only inter_noku needs a key emitted here.
        "inter_noku_key.pem": _key_pem(inter_noku_key),
    }

    # PKCS#12 -- NOT byte-deterministic (random salt/IV per RFC 7292). Its
    # *contents* are the deterministic artifacts above; --check verifies that.
    enc = serialization.BestAvailableEncryption(PFX_PASSWORD)
    nondet = {
        "leaf_crlsign.pfx": pkcs12.serialize_key_and_certificates(
            name=b"PDFE F47 Signer via cRLSign CA", key=leaf_crl_key,
            cert=leaf_crl, cas=[inter_crl, root], encryption_algorithm=enc),
        "leaf_nocrlsign.pfx": pkcs12.serialize_key_and_certificates(
            name=b"PDFE F47 Signer via NO-cRLSign CA", key=leaf_nocrl_key,
            cert=leaf_nocrl, cas=[inter_nocrl, root], encryption_algorithm=enc),
        "leaf_crldp.pfx": pkcs12.serialize_key_and_certificates(
            name=b"PDFE F47 Signer with CRLDP", key=leaf_crldp_key,
            cert=leaf_crldp, cas=[inter_crl, root], encryption_algorithm=enc),
        "leaf_noku.pfx": pkcs12.serialize_key_and_certificates(
            name=b"PDFE F55 Signer via NO-keyUsage CA", key=leaf_noku_key,
            cert=leaf_noku, cas=[inter_noku, root], encryption_algorithm=enc),
    }

    return {"deterministic": artifacts, "nondeterministic": nondet}


def _manifest_text(deterministic: dict) -> bytes:
    buf = io.StringIO()
    buf.write("# SHA-256 of the byte-deterministic artifacts produced by\n")
    buf.write("# tests/fixtures/gen_two_level_ca_signer.py\n")
    buf.write("#\n")
    buf.write("# The .pfx members are deliberately ABSENT: PKCS#12 uses a\n")
    buf.write("# random salt/IV so their bytes vary per run. Verify those with\n")
    buf.write("# --check, which compares their unwrapped contents instead.\n")
    for name in sorted(deterministic):
        buf.write("%s  %s\n" % (hashlib.sha256(deterministic[name]).hexdigest(), name))
    return buf.getvalue().encode("utf-8")


def do_generate(out_dir: pathlib.Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    result = generate(out_dir)
    for name, data in result["deterministic"].items():
        (out_dir / name).write_bytes(data)
    for name, data in result["nondeterministic"].items():
        (out_dir / name).write_bytes(data)
    (out_dir / "MANIFEST.sha256").write_bytes(_manifest_text(result["deterministic"]))
    for name in sorted(list(result["deterministic"]) + list(result["nondeterministic"])):
        print("wrote %s" % (out_dir / name))
    print("wrote %s" % (out_dir / "MANIFEST.sha256"))
    return 0


def do_check(out_dir: pathlib.Path) -> int:
    """Verify the committed artifacts against a fresh generation."""
    if not out_dir.is_dir():
        print("FAIL: %s does not exist" % out_dir)
        return 1
    with tempfile.TemporaryDirectory() as td:
        fresh = generate(pathlib.Path(td))

    ok = True
    for name, data in fresh["deterministic"].items():
        p = out_dir / name
        if not p.is_file():
            print("FAIL: missing %s" % name)
            ok = False
            continue
        got = p.read_bytes()
        # PEM members are ASCII with no NUL bytes, so git's binary heuristic
        # does not protect them; with core.autocrlf=true a checkout can rewrite
        # LF -> CRLF. `.gitattributes` marks them `-text` to prevent that, but a
        # rule can be lost, so tolerate CRLF here rather than reporting a
        # checkout artefact as a reproducibility failure. DER and PKCS#12 are
        # binary and are NEVER normalised — a CRLF inside them is real damage.
        if name.endswith(".pem"):
            got = got.replace(b"\r\n", b"\n")
        if got != data:
            print("FAIL: %s differs from a fresh generation" % name)
            ok = False
        else:
            print("ok (bytes)    %s" % name)

    # PKCS#12: compare unwrapped contents, since the container bytes vary.
    der = serialization.Encoding.DER
    for name in fresh["nondeterministic"]:
        p = out_dir / name
        if not p.is_file():
            print("FAIL: missing %s" % name)
            ok = False
            continue
        got_key, got_cert, got_cas = pkcs12.load_key_and_certificates(
            p.read_bytes(), PFX_PASSWORD)
        exp_key, exp_cert, exp_cas = pkcs12.load_key_and_certificates(
            fresh["nondeterministic"][name], PFX_PASSWORD)
        same = (got_cert.public_bytes(der) == exp_cert.public_bytes(der)
                and got_key.private_bytes(
                    der, serialization.PrivateFormat.PKCS8,
                    serialization.NoEncryption())
                == exp_key.private_bytes(
                    der, serialization.PrivateFormat.PKCS8,
                    serialization.NoEncryption())
                and [c.public_bytes(der) for c in got_cas]
                == [c.public_bytes(der) for c in exp_cas])
        if same:
            print("ok (contents) %s  (container bytes vary by design)" % name)
        else:
            print("FAIL: %s contents differ from a fresh generation" % name)
            ok = False

    # F56: every committed private key states in-file that it is test-only, AND
    # still parses. Both halves matter — a marker that broke PEM parsing would
    # be worse than no marker, so this LOADS the committed bytes rather than
    # trusting RFC 7468 §5.2's pre-encapsulation rule in the abstract.
    for name in sorted(fresh["deterministic"]):
        if not name.endswith("_key.pem"):
            continue
        p = out_dir / name
        if not p.is_file():
            continue  # already reported above
        raw = p.read_bytes().replace(b"\r\n", b"\n")
        if not raw.startswith(KEY_MARKER.encode("ascii")):
            print("FAIL: %s does not begin with the TEST-ONLY KEY marker" % name)
            ok = False
            continue
        try:
            serialization.load_pem_private_key(raw, password=None)
        except Exception as exc:
            print("FAIL: %s no longer parses as a private key: %s" % (name, exc))
            ok = False
            continue
        print("ok (marker)   %s" % name)

    manifest = out_dir / "MANIFEST.sha256"
    expected = _manifest_text(fresh["deterministic"])
    if not manifest.is_file():
        print("FAIL: missing MANIFEST.sha256")
        ok = False
    elif manifest.read_bytes().replace(b"\r\n", b"\n") != expected:
        print("FAIL: MANIFEST.sha256 does not match a fresh generation")
        ok = False
    else:
        print("ok (bytes)    MANIFEST.sha256")

    print("CHECK %s" % ("PASSED" if ok else "FAILED"))
    return 0 if ok else 1


def main(argv=None) -> int:
    here = pathlib.Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=str(here / "crl_chain"),
                    help="output directory (default: tests/fixtures/crl_chain)")
    ap.add_argument("--check", action="store_true",
                    help="verify committed artifacts instead of writing them")
    args = ap.parse_args(argv)
    out_dir = pathlib.Path(args.out)
    return do_check(out_dir) if args.check else do_generate(out_dir)


if __name__ == "__main__":
    sys.exit(main())
