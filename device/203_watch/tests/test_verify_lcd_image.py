import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('verify_image', Path(__file__).resolve().parents[1] / 'tools/verify_lcd_image.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ImageChecks(unittest.TestCase):
    def test_clean_mixed_and_wrong_artifact(self):
        with tempfile.TemporaryDirectory() as folder:
            image = Path(folder) / 'application.bin'
            clean = b'BUILD=HY-LCD-ONLY-1\0I2C=OFF scans=OFF\0'
            image.write_bytes(clean)
            with contextlib.redirect_stdout(io.StringIO()): module.verify(image)
            image.write_bytes(clean + b'demo 200_lcd_display hello world!')
            with self.assertRaises(ValueError): module.verify(image)
            image.write_bytes(b'old firmware or opaque container')
            with self.assertRaises(ValueError): module.verify(image)


if __name__ == '__main__': unittest.main()
