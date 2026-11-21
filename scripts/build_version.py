"""Set the firmware display version before PlatformIO compiles it."""
import os
from pathlib import Path
import runpy

Import('env')

root = Path(env.subst('$PROJECT_DIR'))
build_version = runpy.run_path(str(root / 'scripts/build-version.py'))['build_version']
flags = env.ParseFlags(env['BUILD_FLAGS'])['CPPDEFINES']
stamp = next(value.strip('\\"') for name, value in flags if name == 'PROXY_RELEASE_VERSION')
version = os.environ.get('GDFU_BUILD_VERSION') or build_version(root, stamp)
env.Replace(BUILD_FLAGS=[flag.replace(stamp, version) if 'PROXY_RELEASE_VERSION=' in flag else flag
                         for flag in env['BUILD_FLAGS']])
print(f'GDFU build version: {version}')
