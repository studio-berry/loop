#!/usr/bin/env python3
"""Generate the committed signed-PDF fixture used by UnitTestsIncrementalSave.

This is a developer/CI tool, not a shipped dependency. The generated PDF and its
manifest are committed; the private key is created in a temporary directory and
is never written into the repository. Regeneration is not byte-reproducible (the
signing time and the CMS bytes differ per run), so the manifest digest must be
re-recorded whenever the fixture is regenerated.

Run it with a throwaway interpreter, never the host one:

    python -m venv "$LOCALAPPDATA/Temp/loop-fixture-venv"
    "$LOCALAPPDATA/Temp/loop-fixture-venv/Scripts/python" -m pip install "pyhanko==0.37.0" cryptography
    "$LOCALAPPDATA/Temp/loop-fixture-venv/Scripts/python" scripts/qualification/make_signed_fixture.py \
        --output UnitTests/testdata/signatures/signed-incremental-base.pdf \
        --manifest UnitTests/testdata/signatures/manifest.json
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import tempfile
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID
from pyhanko.pdf_utils.generic import StreamObject
from pyhanko.pdf_utils.writer import PageObject, PdfFileWriter
from pyhanko.sign import signers
from pyhanko.sign.fields import SigFieldSpec

FIELD_NAME = "LoopSignature"
BLANK_PAGE_MEDIA_BOX = (0, 0, 595, 842)


def build_key_pair(directory: Path) -> tuple[Path, Path, str, str]:
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Loop Save Policy Fixture"),
                                  x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Studio Berry")])
    now = datetime.datetime.now(datetime.timezone.utc)
    certificate = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=3650))
        .sign(key, hashes.SHA256())
    )
    key_path = directory / "fixture-key.pem"
    cert_path = directory / "fixture-cert.pem"
    key_path.write_bytes(
        key.private_bytes(serialization.Encoding.PEM,
                          serialization.PrivateFormat.PKCS8,
                          serialization.NoEncryption())
    )
    cert_path.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
    fingerprint = certificate.fingerprint(hashes.SHA256()).hex()
    return key_path, cert_path, fingerprint, certificate.subject.rfc4514_string()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory() as temporary:
        key_path, cert_path, fingerprint, subject = build_key_pair(Path(temporary))
        signer = signers.SimpleSigner.load(str(key_path), str(cert_path), ca_chain_files=None, key_passphrase=None)
        writer = PdfFileWriter()
        writer.insert_page(PageObject(writer.add_object(StreamObject(stream_data=b"")), BLANK_PAGE_MEDIA_BOX))
        output = arguments.output
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("wb") as output_stream:
            signers.PdfSigner(
                signers.PdfSignatureMetadata(field_name=FIELD_NAME, md_algorithm="sha256"),
                signer=signer,
                new_field_spec=SigFieldSpec(FIELD_NAME, on_page=0, box=(36, 36, 236, 96)),
            ).sign_pdf(writer, output=output_stream)

    payload = output.read_bytes()
    manifest = {
        "fixture": output.name,
        "purpose": "prove that an incremental append preserves a real signature byte range",
        "provenance": "generated in-repo by scripts/qualification/make_signed_fixture.py; not third-party",
        "license": "same as the repository (MIT)",
        "generator": "pyhanko 0.37.0 + cryptography",
        "signature_field": FIELD_NAME,
        "certificate_subject": subject,
        "certificate_sha256_fingerprint": fingerprint,
        "private_key": "ephemeral, created in a temporary directory, never committed",
        "byte_reproducible": False,
        "sha256": hashlib.sha256(payload).hexdigest(),
        "bytes": len(payload),
        "regenerated_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "expected_validator_result": {"structural": "passed", "signature": "passed"},
    }
    arguments.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"{output} {manifest['sha256']} {manifest['bytes']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
