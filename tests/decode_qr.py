"""Independent round-trip check for the production GDI QR renderer."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "build" / "qr-test-deps"))
import zxingcpp
from PIL import Image

expected = "WIFI:T:ADB;S:studio-scrdock-test;P:123456;;"
for size in (200, 300, 400):
    with Image.open(root / "build" / "tests" / f"qr-{size}.bmp") as image:
        result = zxingcpp.read_barcode(image.convert("RGB"))
    assert result and result.text == expected, f"QR decoding failed at {size}px"
    print(f"PASS independent QR decoding: {size}px")
