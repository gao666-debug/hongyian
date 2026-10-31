import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('installer', Path(__file__).resolve().parents[1] / 'tools/install_lcd_only.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InstallerTests(unittest.TestCase):
    def test_backup_config_isolation_and_reinstallation(self):
        with tempfile.TemporaryDirectory() as temp:
            sdk = Path(temp)
            demo = sdk / 'application/samples/demo'
            demo.mkdir(parents=True)
            (demo / 'Kconfig').write_text('config ENABLE_DEMO_SAMPLE\n    bool\n')
            (demo / 'CMakeLists.txt').write_text('set(SOURCES "${SOURCES}" PARENT_SCOPE)\n')
            config = sdk / 'build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config'
            config.parent.mkdir(parents=True)
            original = 'CONFIG_SAMPLE_SUPPORT_200_LCD_DISPLAY=y\nCONFIG_OTHER=y\nCONFIG_SAMPLE_SUPPORT_202_HEART_RATE_OXIMETRY_SENSOR=y\n'
            config.write_text(original)
            dest = demo / '203_watch'
            dest.mkdir()
            (dest / 'lcd_diag.c').write_text('old diagnostic')
            cache = sdk / 'output/ws63/acore/ws63-liteos-app/cmake_command.txt'
            cache.parent.mkdir(parents=True)
            cache.write_text('old cache')
            with contextlib.redirect_stdout(io.StringIO()):
                _, backup = installer.install(sdk)
                installer.install(sdk)
            self.assertEqual((backup / 'target.config').read_text(), original)
            self.assertEqual((backup / '203_watch/lcd_diag.c').read_text(), 'old diagnostic')
            self.assertEqual((backup / 'cmake_command.txt').read_text(), 'old cache')
            self.assertFalse(cache.exists())
            selected = [line for line in config.read_text().splitlines() if line.startswith('CONFIG_SAMPLE_SUPPORT_') and line.endswith('=y')]
            self.assertEqual(selected, ['CONFIG_SAMPLE_SUPPORT_203_WATCH=y'])
            self.assertIn('CONFIG_OTHER=y\n', config.read_text())
            self.assertEqual((demo / 'CMakeLists.txt').read_text().count('add_subdirectory_if_exist(203_watch)'), 1)
            self.assertEqual((demo / 'CMakeLists.txt').read_text().count('# HY_LCD_ONLY_GUARD_BEGIN'), 1)
            self.assertIn('HY-LCD-ONLY entry missing', (demo / 'CMakeLists.txt').read_text())
            self.assertNotIn('lcd_diag.c', (dest / 'CMakeLists.txt').read_text())
            self.assertIn('lcd_bringup.c', (dest / 'CMakeLists.txt').read_text())

    def test_bad_sdk_does_not_create_backup(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(ValueError):
                installer.install(temp)
            self.assertEqual(list(Path(temp).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
