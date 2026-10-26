"""Update signing: ECDSA P-256 / SHA-256, IEEE P1363 (r||s, 64 bytes), base64.

The private key lives only on the owner's laptop. The public key is the
uncompressed point X||Y (64 bytes), base64.
"""
import base64
import binascii
import os
import subprocess
import sys
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature, encode_dss_signature)

KEY_PATH = Path(os.environ.get("USERPROFILE", str(Path.home()))) / ".reborn" / "update-signing-key.pem"


def _public_b64(key: ec.EllipticCurvePrivateKey) -> str:
    n = key.public_key().public_numbers()
    return base64.b64encode(n.x.to_bytes(32, "big") + n.y.to_bytes(32, "big")).decode()


def _load(path: Path) -> ec.EllipticCurvePrivateKey:
    return serialization.load_pem_private_key(Path(path).read_bytes(), password=None)


def _restrict_to_current_user(path: Path) -> None:
    """Best effort: only the current user may read the key file."""
    if os.name != "nt":
        os.chmod(path, 0o600)
        return
    user = os.environ.get("USERNAME")
    if user:
        subprocess.run(["icacls", str(path), "/inheritance:r", "/grant:r", f"{user}:F"],
                       capture_output=True)


def keygen(path: Path = KEY_PATH) -> str:
    """Make a new private key at path; refuse to overwrite. Returns the public key."""
    path = Path(path)
    if path.exists():
        raise FileExistsError(f"{path} already exists; not overwriting the signing key")
    path.parent.mkdir(parents=True, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.private_bytes(serialization.Encoding.PEM,
                            serialization.PrivateFormat.PKCS8,
                            serialization.NoEncryption())
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    _restrict_to_current_user(path)
    return _public_b64(key)


def public_key_b64(path: Path = KEY_PATH) -> str:
    return _public_b64(_load(path))


def sign(data: bytes, path: Path = KEY_PATH) -> str:
    der = _load(path).sign(data, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    return base64.b64encode(r.to_bytes(32, "big") + s.to_bytes(32, "big")).decode()


def verify(data: bytes, sig_b64: str, pub_b64: str) -> bool:
    try:
        sig = base64.b64decode(sig_b64, validate=True)
        pub = base64.b64decode(pub_b64, validate=True)
        if len(sig) != 64 or len(pub) != 64:
            return False
        point = b"\x04" + pub
        key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), point)
        der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
        key.verify(der, data, ec.ECDSA(hashes.SHA256()))
        return True
    except (InvalidSignature, ValueError, binascii.Error):
        return False


def main(argv: list[str]) -> int:
    if len(argv) != 1 or argv[0] not in ("keygen", "public"):
        print("usage: python -m tools.signing keygen|public", file=sys.stderr)
        return 2
    try:
        print(keygen() if argv[0] == "keygen" else public_key_b64())
    except (FileExistsError, FileNotFoundError) as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
