#!/usr/bin/env python3
"""Install ONLY the fixed-pin panel test into an existing WS63 SDK; no flashing.
All overwritten files and the previous cache marker are backed up first.
"""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
from datetime import datetime


def install(sdk, demo=None, config=None):
    sdk = Path(sdk).resolve()
    candidates = [sdk / 'application/samples/demo', sdk / 'src/application/samples/demo',
                  sdk / 'device/soc/hisilicon/ws63v100/sdk/application/samples/demo']
    demo = Path(demo).resolve() if demo else next((p for p in candidates if (p / 'Kconfig').is_file()), None)
    if demo is None:
        raise ValueError('Cannot locate demo/. Pass --demo-dir explicitly; no files changed.')
    config = Path(config).resolve() if config else sdk / 'build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config'
    cmake = demo / 'CMakeLists.txt'
    kconfig = demo / 'Kconfig'
    for path in (cmake, kconfig, config):
        if not path.is_file():
            raise ValueError(f'Missing {path}; pass --demo-dir/--config as needed; no files changed.')
    source = Path(__file__).resolve().parents[1] / 'src'
    names = ['lcd_only_main.c', 'lcd_bringup.c', 'lcd_bringup.h', 'lcd_vendor_init.inc']
    for name in names:
        if not (source / name).is_file():
            raise ValueError(f'Missing source file: {name}')
    cmake_text, kconfig_text, config_text = [p.read_text(encoding='utf-8') for p in (cmake, kconfig, config)]
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    backup = sdk / 'hongyian-backups' / stamp
    backup.mkdir(parents=True)
    dest = demo / '203_watch'
    if dest.exists():
        shutil.copytree(dest, backup / '203_watch')
    for label, path in [('demo-CMakeLists.txt', cmake), ('demo-Kconfig', kconfig), ('target.config', config)]:
        shutil.copy2(path, backup / label)
    dest.mkdir(parents=True, exist_ok=True)
    for name in names:
        shutil.copy2(source / name, dest / name)
    # Deliberately bypass old main.c/watch_ui.c/lcd_diag.c even if present in SDK.
    (dest / 'CMakeLists.txt').write_text('''message(STATUS "Hongyian: HY-LCD-ONLY-1 no I2C/scans")
set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/lcd_only_main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/lcd_bringup.c"
    PARENT_SCOPE)
''', encoding='utf-8')
    if not re.search(r'add_subdirectory_if_exist\s*\(\s*203_watch\s*\)', cmake_text):
        pos = cmake_text.rfind('set(SOURCES')
        if pos < 0: pos = len(cmake_text)
        block = '\nif(DEFINED CONFIG_SAMPLE_SUPPORT_203_WATCH)\n    add_subdirectory_if_exist(203_watch)\nendif()\n\n'
        cmake_text = cmake_text[:pos] + block + cmake_text[pos:]
    if not re.search(r'^config SAMPLE_SUPPORT_203_WATCH\s*$', kconfig_text, re.M):
        kconfig_text += '\nconfig SAMPLE_SUPPORT_203_WATCH\n    bool "Hongyian fixed-pin LCD test"\n    default n\n    depends on ENABLE_DEMO_SAMPLE\n'
    config_text = re.sub(r'^CONFIG_SAMPLE_SUPPORT_([A-Za-z0-9_]+)=y[ \t]*$',
                         lambda m: '# CONFIG_SAMPLE_SUPPORT_' + m[1] + ' is not set', config_text, flags=re.M)
    # Remove any previous selected/unselected copy before adding one canonical setting.
    for key in ['CONFIG_SAMPLE_SUPPORT_203_WATCH', 'CONFIG_ENABLE_DEMO_SAMPLE']:
        config_text = re.sub(r'^(?:' + key + r'=.*|# ' + key + r' is not set)\n?', '', config_text, flags=re.M)
    config_text += '\nCONFIG_ENABLE_DEMO_SAMPLE=y\nCONFIG_SAMPLE_SUPPORT_203_WATCH=y\n'
    cmake.write_text(cmake_text, encoding='utf-8')
    kconfig.write_text(kconfig_text, encoding='utf-8')
    config.write_text(config_text, encoding='utf-8')
    cache = sdk / 'output/ws63/acore/ws63-liteos-app/cmake_command.txt'
    if cache.is_file():
        shutil.copy2(cache, backup / 'cmake_command.txt')
        cache.unlink()  # force SDK build configuration to be regenerated
    print('Installed: HY-LCD-ONLY-1')
    print('Demo:', dest)
    print('Config:', config)
    print('Backup:', backup)
    for name in names:
        print(hashlib.sha256((dest / name).read_bytes()).hexdigest(), name)
    print('Build with your existing SDK environment: python build.py -nhso -j8 ws63-liteos-app')
    print('Do not use old custom pack_fw.py silently: flash this build output using the validated SDK workflow.')
    return dest, backup


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, help='WS63 SDK root containing build.py')
    parser.add_argument('--demo-dir', help='Override demo directory')
    parser.add_argument('--config', help='Override ws63_liteos_app.config path')
    args = parser.parse_args()
    try:
        install(args.sdk, args.demo_dir, args.config)
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + '\n')
