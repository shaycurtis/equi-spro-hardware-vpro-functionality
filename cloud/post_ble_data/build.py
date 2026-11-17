"""
Build the postBleData deployment zips into cloud/post_ble_data/dist/ (git-ignored build output):

  post_ble_data.zip      the function: post_ble_data.py + ble_dedup.py + gateway/equi_protocol.py
  cryptography_layer.zip a Lambda layer with `cryptography` (AES-CCM), Linux wheels for the runtime

    python cloud/post_ble_data/build.py [--python 3.12] [--arch x86_64|arm64]

Match --python / --arch to the function's runtime (use the same runtime as postLoraData, since the
equimetrics_common layer uses pip's vendored requests).
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DIST = HERE / "dist"


def build_function():
    out = DIST / "post_ble_data.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(HERE / "post_ble_data.py", "post_ble_data.py")
        z.write(HERE / "ble_dedup.py", "ble_dedup.py")
        z.write(REPO / "gateway" / "equi_protocol.py", "equi_protocol.py")
    print(f"function: {out}  (handler: post_ble_data.lambda_handler)")


def build_layer(py: str, arch: str):
    platform = "manylinux2014_x86_64" if arch == "x86_64" else "manylinux2014_aarch64"
    out = DIST / "cryptography_layer.zip"
    with tempfile.TemporaryDirectory() as tmp:
        target = Path(tmp) / "python"
        subprocess.run([sys.executable, "-m", "pip", "install", "cryptography", "--target", str(target),
                        "--platform", platform, "--python-version", py, "--implementation", "cp",
                        "--only-binary=:all:", "--upgrade", "--quiet"], check=True)
        base = out.with_suffix("")
        shutil.make_archive(str(base), "zip", tmp, "python")
    print(f"layer:    {out}  (compatible runtime python{py}, {arch})")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--python", default="3.12")
    ap.add_argument("--arch", default="x86_64", choices=["x86_64", "arm64"])
    ap.add_argument("--no-layer", action="store_true")
    a = ap.parse_args()
    DIST.mkdir(exist_ok=True)
    build_function()
    if not a.no_layer:
        build_layer(a.python, a.arch)
