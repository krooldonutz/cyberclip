#!/usr/bin/env python3
"""Build versioned, merged Cyberclip firmware images for the web installer.

Usage: python3 scripts/build_firmware.py [--rebuild] <major>.<minor>.<patch>

Works on macOS, Linux, and Windows. Requires Python 3.8+, npm, and PlatformIO
Core (installed with pip when it is missing).
"""

import argparse
import gzip
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
FIRMWARE_DIRECTORY = REPOSITORY_ROOT / 'firmware'
FIRMWARE_SOURCE_PATH = FIRMWARE_DIRECTORY / 'matrix_display.ino'
DEVICE_BUILD_DIRECTORY = REPOSITORY_ROOT / 'dist-device'
DEVICE_WEB_HEADER_PATH = FIRMWARE_DIRECTORY / 'generated' / 'device_web.h'
MANIFEST_PATH = REPOSITORY_ROOT / 'public' / 'firmware' / 'manifest.json'
SERVICE_WORKER_PATH = REPOSITORY_ROOT / 'public' / 'service-worker.js'
BUILD_ROOT = FIRMWARE_DIRECTORY / '.pio' / 'build'

# The manifest has no legacy top-level image fields: web app builds old
# enough to need them only flashed the original ESP32 board, which is no longer
# supported, so they reject the manifest instead of flashing the wrong chip.
FIRMWARE_TARGETS = [
    {
        'environment': 'lilygo_t_display_s3',
        'chip': 'ESP32-S3',
        'esptool_chip': 'esp32s3',
        'board': 'LilyGO T-Display-S3',
        'file_stem': 'cyberclip-lilygo-t-display-s3',
        'bootloader_offset': 0x0,
        'flash_mode': 'keep',
        'flash_freq': 'keep',
    },
]

CONTENT_TYPES = {
    '.html': 'text/html; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.json': 'application/json',
    '.svg': 'image/svg+xml',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.jpeg': 'image/jpeg',
    '.ico': 'image/x-icon',
    '.webp': 'image/webp',
    '.woff2': 'font/woff2',
}


class ReleaseError(Exception):
    pass


def read_text(path):
    with open(path, encoding='utf-8', newline='') as file:
        return file.read()


def write_text(path, content):
    """Writes UTF-8 text exactly as given, without newline translation."""
    with open(path, 'w', encoding='utf-8', newline='') as file:
        file.write(content)


def run(command, cwd=None):
    result = subprocess.run(command, cwd=cwd)
    if result.returncode != 0:
        raise ReleaseError(
            f'Command failed with exit code {result.returncode}: '
            f'{" ".join(str(part) for part in command)}')


def find_platformio():
    """Returns the command prefix that runs PlatformIO Core."""
    candidates = [[sys.executable, '-m', 'platformio']]
    pio = shutil.which('pio') or shutil.which('platformio')
    if pio:
        candidates.insert(0, [pio])
    for candidate in candidates:
        if subprocess.run(candidate + ['--version'], capture_output=True).returncode == 0:
            return candidate

    print('PlatformIO Core was not found; installing it with pip...')
    if subprocess.run([sys.executable, '-m', 'pip', 'install', '--user',
                       'platformio']).returncode != 0:
        raise ReleaseError(
            'Unable to install PlatformIO. Install it yourself, for example '
            'with "brew install platformio" or "pipx install platformio".')
    return [sys.executable, '-m', 'platformio']


def write_embedded_web_header(source_directory, header_path):
    files = sorted(
        (path for path in source_directory.rglob('*') if path.is_file()),
        key=lambda path: path.relative_to(source_directory).as_posix().lower())
    entry_path = source_directory / 'device.html'
    if not entry_path.is_file():
        entry_path = source_directory / 'index.html'
    if not files or not entry_path.is_file():
        raise ReleaseError(
            'The device web build did not produce device.html (or index.html).')

    lines = [
        '#pragma once',
        '',
        '#include <stddef.h>',
        '#include <stdint.h>',
        '',
        'namespace cyberclip {',
        'struct EmbeddedWebFile {',
        '  const char *path;',
        '  const char *contentType;',
        '  const uint8_t *data;',
        '  size_t size;',
        '};',
        '',
    ]
    for index, path in enumerate(files):
        compressed = gzip.compress(path.read_bytes(), compresslevel=9, mtime=0)
        lines.append(f'static const uint8_t kDeviceWebFile{index}[] = {{')
        for offset in range(0, len(compressed), 12):
            values = ', '.join(f'0x{byte:02X}'
                               for byte in compressed[offset:offset + 12])
            lines.append(f'    {values},')
        lines.append('};')
        lines.append('')

    lines.append('static const EmbeddedWebFile kDeviceWebFiles[] = {')
    for index, path in enumerate(files):
        relative_path = ('index.html' if path == entry_path
                         else path.relative_to(source_directory).as_posix())
        content_type = CONTENT_TYPES.get(path.suffix.lower(),
                                         'application/octet-stream')
        lines.append(
            f'    {{"/{relative_path}", "{content_type}", kDeviceWebFile{index}, '
            f'sizeof(kDeviceWebFile{index})}},')
    lines += [
        '};',
        'constexpr size_t kDeviceWebFileCount =',
        '    sizeof(kDeviceWebFiles) / sizeof(kDeviceWebFiles[0]);',
        '}  // namespace cyberclip',
        '',
    ]
    write_text(header_path, '\n'.join(lines))


def replace_single_match(content, pattern, replacement, description):
    if len(re.findall(pattern, content, flags=re.MULTILINE)) != 1:
        raise ReleaseError(f'Expected exactly one {description}.')
    return re.sub(pattern, replacement, content, flags=re.MULTILINE)


def assert_embedded_file(image_path, component_path, offset):
    image = image_path.read_bytes()
    component = component_path.read_bytes()
    if len(image) < offset + len(component):
        raise ReleaseError(
            f'Merged image is too small to contain {component_path} at '
            f'offset 0x{offset:x}.')
    if image[offset:offset + len(component)] != component:
        raise ReleaseError(
            f'Merged image does not contain {component_path} at offset '
            f'0x{offset:x}.')


def parse_version(text):
    match = re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)', text)
    if not match:
        raise ReleaseError(
            'Version must use major.minor.patch format, for example 2.0.6.')
    parts = tuple(int(part) for part in match.groups())
    if max(parts) > 255:
        raise ReleaseError(
            'Each firmware version component must fit in a uint8_t (0-255).')
    return parts


def build_release(version, rebuild=False):
    version_parts = parse_version(version)
    for required_path in (FIRMWARE_SOURCE_PATH, DEVICE_WEB_HEADER_PATH,
                          MANIFEST_PATH, SERVICE_WORKER_PATH):
        if not required_path.is_file():
            raise ReleaseError(f'Required file not found: {required_path}')

    original_firmware_source = read_text(FIRMWARE_SOURCE_PATH)
    original_device_web_header = read_text(DEVICE_WEB_HEADER_PATH)
    original_manifest = read_text(MANIFEST_PATH)
    original_service_worker = read_text(SERVICE_WORKER_PATH)
    manifest = json.loads(original_manifest)
    current_parts = tuple(int(part) for part in manifest['version'].split('.'))
    if version_parts < current_parts or (
            version_parts == current_parts and not rebuild):
        raise ReleaseError(
            f'Version {version} must be newer than the current published '
            f'version {manifest["version"]}.')

    old_web_paths = [manifest['path']] if 'path' in manifest else []
    old_web_paths += [build['path'] for build in manifest.get('builds', [])]
    old_web_paths = list(dict.fromkeys(old_web_paths))
    for old_web_path in old_web_paths:
        if not re.fullmatch(r'/firmware/[^/]+\.bin', old_web_path):
            raise ReleaseError(f'Manifest firmware path is invalid: {old_web_path}')

    targets = [dict(target) for target in FIRMWARE_TARGETS]
    for target in targets:
        target['web_path'] = f'/firmware/{target["file_stem"]}-{version}.bin'
        target['binary_path'] = REPOSITORY_ROOT / 'public' / target['web_path'].lstrip('/')
        target['temporary_path'] = target['binary_path'].with_name(
            target['binary_path'].name + '.tmp')
        target['existed'] = target['binary_path'].exists()
        if target['existed'] and not rebuild:
            raise ReleaseError(
                f'Refusing to overwrite existing release image: {target["binary_path"]}')

    npm = shutil.which('npm')
    if not npm:
        raise ReleaseError('npm is required to build the embedded device web page.')
    platformio = find_platformio()
    platform_info = json.loads(subprocess.run(
        platformio + ['system', 'info', '--json-output'],
        capture_output=True, check=True, text=True).stdout)
    core_directory = Path(platform_info['core_dir']['value'])
    boot_app_path = (core_directory / 'packages' / 'framework-arduinoespressif32'
                     / 'tools' / 'partitions' / 'boot_app0.bin')

    updated_firmware_source = original_firmware_source
    for name, value in zip(('Major', 'Minor', 'Patch'), version_parts):
        updated_firmware_source = replace_single_match(
            updated_firmware_source, rf'constexpr uint8_t kFirmware{name} = \d+;',
            f'constexpr uint8_t kFirmware{name} = {value};',
            f'firmware {name.lower()} version constant')

    release_completed = False
    try:
        write_text(FIRMWARE_SOURCE_PATH, updated_firmware_source)

        print('Building the embedded device web page...')
        run([npm, 'run', 'build:device', '--', '--outDir',
             str(DEVICE_BUILD_DIRECTORY), '--emptyOutDir'], cwd=REPOSITORY_ROOT)
        write_embedded_web_header(DEVICE_BUILD_DIRECTORY, DEVICE_WEB_HEADER_PATH)

        for target in targets:
            print(f'Building Cyberclip firmware {version} for {target["board"]}...')
            run(platformio + ['run', '-e', target['environment']],
                cwd=FIRMWARE_DIRECTORY)

            build_directory = BUILD_ROOT / target['environment']
            bootloader_path = build_directory / 'bootloader.bin'
            partitions_path = build_directory / 'partitions.bin'
            application_path = build_directory / 'firmware.bin'
            for artifact_path in (bootloader_path, partitions_path,
                                  application_path, boot_app_path):
                if not artifact_path.is_file():
                    raise ReleaseError(f'Required build artifact not found: {artifact_path}')

            temporary_path = target['temporary_path']
            temporary_path.unlink(missing_ok=True)
            print(f'Merging {target["chip"]} bootloader, partitions, boot app, '
                  'and application...')
            run(platformio + [
                'pkg', 'exec', '--package', 'tool-esptoolpy', '--',
                'esptool.py', '--chip', target['esptool_chip'], 'merge_bin',
                '-o', str(temporary_path),
                '--flash_mode', target['flash_mode'],
                '--flash_freq', target['flash_freq'],
                '--flash_size', '16MB',
                f'0x{target["bootloader_offset"]:x}', str(bootloader_path),
                '0x8000', str(partitions_path),
                '0xe000', str(boot_app_path),
                '0x10000', str(application_path),
            ], cwd=FIRMWARE_DIRECTORY)

            if target['flash_mode'] == 'keep' and target['flash_freq'] == 'keep':
                assert_embedded_file(temporary_path, bootloader_path,
                                     target['bootloader_offset'])
            if (target['bootloader_offset'] == 0 and
                    temporary_path.read_bytes()[:1] != b'\xE9'):
                raise ReleaseError(
                    f'Merged {target["chip"]} image does not start with a bootloader.')
            assert_embedded_file(temporary_path, partitions_path, 0x8000)
            assert_embedded_file(temporary_path, boot_app_path, 0xe000)
            assert_embedded_file(temporary_path, application_path, 0x10000)
            target['size'] = temporary_path.stat().st_size

        updated_manifest = json.dumps({
            'version': version,
            'builds': [{
                'chip': target['chip'],
                'board': target['board'],
                'path': target['web_path'],
                'address': 0,
                'size': target['size'],
                'flashMode': target['flash_mode'],
                'flashFreq': target['flash_freq'],
            } for target in targets],
        }, indent=2) + '\n'

        cache_match = re.search(r"const CACHE_NAME = 'cyberclip-v(\d+)';",
                                original_service_worker)
        if not cache_match:
            raise ReleaseError('Unable to find the service-worker cache version.')
        next_cache_version = int(cache_match.group(1)) + 1
        updated_service_worker = replace_single_match(
            original_service_worker, r"const CACHE_NAME = 'cyberclip-v\d+';",
            f"const CACHE_NAME = 'cyberclip-v{next_cache_version}';",
            'service-worker cache version')
        updated_service_worker = re.sub(
            r"^[ \t]*'/firmware/[^']+\.bin',\r?\n", '', updated_service_worker,
            flags=re.MULTILINE)
        firmware_cache_entries = ''.join(
            f"  '{target['web_path']}',\n" for target in targets)
        updated_service_worker = replace_single_match(
            updated_service_worker, r"^([ \t]*'/firmware/manifest\.json',)\r?\n",
            lambda match: f'{match.group(1)}\n{firmware_cache_entries}',
            'service-worker firmware manifest entry')

        for target in targets:
            target['temporary_path'].replace(target['binary_path'])
        write_text(MANIFEST_PATH, updated_manifest)
        write_text(SERVICE_WORKER_PATH, updated_service_worker)

        new_binary_paths = {target['binary_path'] for target in targets}
        for old_web_path in old_web_paths:
            old_binary_path = REPOSITORY_ROOT / 'public' / old_web_path.lstrip('/')
            if old_binary_path not in new_binary_paths and old_binary_path.is_file():
                old_binary_path.unlink()

        release_completed = True
        for target in targets:
            print(f'Created {target["web_path"]} for {target["board"]} '
                  f'({target["size"]} bytes).')
        print(f'Firmware {version} is ready for the web installer.')
    finally:
        if not release_completed:
            write_text(FIRMWARE_SOURCE_PATH, original_firmware_source)
            write_text(DEVICE_WEB_HEADER_PATH, original_device_web_header)
            write_text(MANIFEST_PATH, original_manifest)
            write_text(SERVICE_WORKER_PATH, original_service_worker)
            for target in targets:
                target['temporary_path'].unlink(missing_ok=True)
                if not target['existed']:
                    target['binary_path'].unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('version', help='new firmware version, e.g. 2.0.22')
    parser.add_argument(
        '--rebuild', action='store_true',
        help='allow rebuilding the currently published version in place')
    args = parser.parse_args()
    try:
        build_release(args.version, args.rebuild)
    except (ReleaseError, subprocess.CalledProcessError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
