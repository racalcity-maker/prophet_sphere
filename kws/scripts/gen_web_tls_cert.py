#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ipaddress
import shutil
import subprocess
import textwrap
from datetime import datetime, timedelta, timezone
from pathlib import Path

_HAS_CRYPTO = True
_CRYPTO_IMPORT_ERROR = ""
try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
except Exception as exc:  # pragma: no cover - environment dependent
    _HAS_CRYPTO = False
    _CRYPTO_IMPORT_ERROR = str(exc)
    x509 = None  # type: ignore[assignment]
    hashes = None  # type: ignore[assignment]
    serialization = None  # type: ignore[assignment]
    ec = None  # type: ignore[assignment]
    ExtendedKeyUsageOID = None  # type: ignore[assignment]
    NameOID = None  # type: ignore[assignment]


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate HTTPS cert/key PEM for ESP web portal.")
    p.add_argument(
        "--cert-dir",
        default="components/services_web/certs",
        help="Directory where servercert.pem and prvtkey.pem are written.",
    )
    p.add_argument("--cn", default="orb-s3.local", help="Certificate common name (CN).")
    p.add_argument(
        "--dns",
        action="append",
        default=None,
        help="DNS SAN entry. Can be repeated.",
    )
    p.add_argument(
        "--ip",
        action="append",
        default=None,
        help="IP SAN entry. Can be repeated.",
    )
    p.add_argument("--days", type=int, default=3650, help="Certificate validity in days.")
    p.add_argument("--ca", action="store_true", help="Sign server cert with a local CA instead of self-sign.")
    p.add_argument("--ca-cn", default="orb-local-ca", help="Common name for generated local CA.")
    p.add_argument(
        "--ca-cert-path",
        default=None,
        help="Path to CA certificate PEM. If absent and --ca is set, defaults to <cert-dir>/ca_cert.pem.",
    )
    p.add_argument(
        "--ca-key-path",
        default=None,
        help="Path to CA private key PEM. If absent and --ca is set, defaults to <cert-dir>/ca_key.pem.",
    )
    p.add_argument(
        "--ca-days",
        type=int,
        default=3650,
        help="CA certificate validity in days (used when new CA is generated).",
    )
    p.add_argument("--backup", action="store_true", help="Keep .bak backups of current cert/key.")
    return p.parse_args()


def build_san_values(cn: str, dns_list: list[str] | None, ip_list: list[str] | None) -> tuple[list[str], list[str]]:
    dns_values = dns_list[:] if dns_list else []
    ip_values = ip_list[:] if ip_list else []
    if cn and cn not in dns_values:
        dns_values.append(cn)
    if "localhost" not in dns_values:
        dns_values.append("localhost")
    if "127.0.0.1" not in ip_values:
        ip_values.append("127.0.0.1")
    if "192.168.4.1" not in ip_values:
        ip_values.append("192.168.4.1")
    return dns_values, ip_values


def build_sans(cn: str, dns_list: list[str] | None, ip_list: list[str] | None) -> list[x509.GeneralName]:
    if x509 is None:
        raise RuntimeError("cryptography backend is not available")
    dns_values, ip_values = build_san_values(cn, dns_list, ip_list)

    out: list[x509.GeneralName] = []
    for d in dns_values:
        out.append(x509.DNSName(d))
    for ip_text in ip_values:
        out.append(x509.IPAddress(ipaddress.ip_address(ip_text)))
    return out


def _openssl_path() -> str:
    openssl_bin = shutil.which("openssl")
    if not openssl_bin:
        raise RuntimeError(
            "openssl not found in PATH. Install OpenSSL or install Python package 'cryptography'."
        )
    return openssl_bin


def _write_openssl_config(path: Path, cn: str, dns_values: list[str], ip_values: list[str]) -> None:
    alt_lines: list[str] = []
    dns_idx = 1
    ip_idx = 1
    for dns in dns_values:
        alt_lines.append(f"DNS.{dns_idx} = {dns}")
        dns_idx += 1
    for ip_text in ip_values:
        alt_lines.append(f"IP.{ip_idx} = {ip_text}")
        ip_idx += 1

    config_text = textwrap.dedent(
        f"""\
        [req]
        default_bits = 2048
        prompt = no
        default_md = sha256
        x509_extensions = v3_req
        distinguished_name = dn

        [dn]
        CN = {cn}

        [v3_req]
        keyUsage = keyEncipherment, digitalSignature
        extendedKeyUsage = serverAuth
        subjectAltName = @alt_names

        [alt_names]
        {chr(10).join(alt_lines)}
        """
    )
    path.write_text(config_text, encoding="utf-8")


def _run_checked(cmd: list[str]) -> None:
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"command failed ({proc.returncode}): {' '.join(cmd)}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )


def generate_with_openssl(args: argparse.Namespace, cert_dir: Path, cert_path: Path, key_path: Path) -> None:
    openssl_bin = _openssl_path()
    dns_values, ip_values = build_san_values(args.cn, args.dns, args.ip)
    cfg_path = cert_dir / "openssl_orb_autogen.cnf"
    _write_openssl_config(cfg_path, args.cn, dns_values, ip_values)

    _run_checked([
        openssl_bin,
        "ecparam",
        "-genkey",
        "-name",
        "prime256v1",
        "-noout",
        "-out",
        str(key_path),
    ])
    _run_checked([
        openssl_bin,
        "req",
        "-new",
        "-x509",
        "-key",
        str(key_path),
        "-out",
        str(cert_path),
        "-days",
        str(max(1, args.days)),
        "-config",
        str(cfg_path),
        "-extensions",
        "v3_req",
    ])


def rotate_file(path: Path, *, keep_backup: bool) -> None:
    if not path.exists():
        return
    if keep_backup:
        backup = path.with_suffix(path.suffix + ".bak")
        path.replace(backup)
    else:
        path.unlink(missing_ok=True)


def _load_private_key(path: Path):
    return serialization.load_pem_private_key(path.read_bytes(), password=None)


def _load_cert(path: Path) -> x509.Certificate:
    return x509.load_pem_x509_certificate(path.read_bytes())


def _generate_ca(cn: str, days: int) -> tuple[object, x509.Certificate]:
    now = datetime.now(timezone.utc)
    ca_key = ec.generate_private_key(ec.SECP256R1())
    ca_subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])
    ca_cert = (
        x509.CertificateBuilder()
        .subject_name(ca_subject)
        .issuer_name(ca_subject)
        .public_key(ca_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(days=1))
        .not_valid_after(now + timedelta(days=max(1, days)))
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                key_encipherment=False,
                content_commitment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=True,
                crl_sign=True,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .sign(private_key=ca_key, algorithm=hashes.SHA256())
    )
    return ca_key, ca_cert


def main() -> int:
    args = parse_args()
    cert_dir = Path(args.cert_dir)
    cert_dir.mkdir(parents=True, exist_ok=True)

    cert_path = cert_dir / "servercert.pem"
    key_path = cert_dir / "prvtkey.pem"
    ca_cert_path = Path(args.ca_cert_path) if args.ca_cert_path else (cert_dir / "ca_cert.pem")
    ca_key_path = Path(args.ca_key_path) if args.ca_key_path else (cert_dir / "ca_key.pem")

    rotate_file(cert_path, keep_backup=args.backup)
    rotate_file(key_path, keep_backup=args.backup)

    if not _HAS_CRYPTO:
        if args.ca:
            print("error: --ca mode requires Python package 'cryptography'.")
            print(f"import error: {_CRYPTO_IMPORT_ERROR}")
            return 2
        try:
            generate_with_openssl(args, cert_dir, cert_path, key_path)
        except Exception as exc:
            print("error: TLS cert/key generation failed without cryptography backend.")
            print(str(exc))
            return 2
        dns_values, ip_values = build_san_values(args.cn, args.dns, args.ip)
        print(f"written cert: {cert_path}")
        print(f"written key:  {key_path}")
        print(f"SAN count: {len(dns_values) + len(ip_values)}")
        print("backend: openssl (cryptography module unavailable)")
        return 0

    key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, args.cn)])
    now = datetime.now(timezone.utc)
    sans = build_sans(args.cn, args.dns, args.ip)
    signer_key = key
    issuer = subject

    if args.ca:
        if ca_cert_path.exists() and ca_key_path.exists():
            ca_cert = _load_cert(ca_cert_path)
            ca_key = _load_private_key(ca_key_path)
        else:
            ca_key, ca_cert = _generate_ca(args.ca_cn, args.ca_days)
            ca_key_path.write_bytes(
                ca_key.private_bytes(
                    encoding=serialization.Encoding.PEM,
                    format=serialization.PrivateFormat.PKCS8,
                    encryption_algorithm=serialization.NoEncryption(),
                )
            )
            ca_cert_path.write_bytes(ca_cert.public_bytes(serialization.Encoding.PEM))
        issuer = ca_cert.subject
        signer_key = ca_key

    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(days=1))
        .not_valid_after(now + timedelta(days=max(1, args.days)))
        .add_extension(x509.SubjectAlternativeName(sans), critical=False)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                key_encipherment=True,
                content_commitment=False,
                data_encipherment=False,
                key_agreement=True,
                key_cert_sign=False,
                crl_sign=False,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        .sign(private_key=signer_key, algorithm=hashes.SHA256())
    )

    key_path.write_bytes(
        key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption(),
        )
    )
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))

    print(f"written cert: {cert_path}")
    print(f"written key:  {key_path}")
    print(f"SAN count: {len(sans)}")
    if args.ca:
        print(f"ca cert:      {ca_cert_path}")
        print(f"ca key:       {ca_key_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
