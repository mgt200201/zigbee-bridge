#!/usr/bin/env python3
"""Recompute manifest_hash for dependencies.lock to skip online re-solve."""
import json
import os
import sys
import yaml

# Ensure idf_component_tools is importable
sys.path.insert(0, '/home/mgt/.espressif/python_env/idf5.4_py3.10_env/lib/python3.10/site-packages')

from idf_component_tools.manager import ManifestManager
from idf_component_tools.utils import ProjectRequirements

PROJECT_DIR = '/home/mgt/pro_esp/zigbee_matter_bridge'
LOCK_FILE = os.path.join(PROJECT_DIR, 'dependencies.lock')

ESP_MATTER_PATH = os.environ.get('ESP_MATTER_PATH', '/home/mgt/esp/esp-matter')
MATTER_SDK_PATH = os.path.join(ESP_MATTER_PATH, 'connectedhomeip', 'connectedhomeip')

# Directories to scan for components (mirroring CMakeLists.txt EXTRA_COMPONENT_DIRS)
SCAN_DIRS = [
    os.path.join(PROJECT_DIR, 'main'),
    os.path.join(ESP_MATTER_PATH, 'examples', 'common'),
    os.path.join(MATTER_SDK_PATH, 'config', 'esp32', 'components'),
    os.path.join(ESP_MATTER_PATH, 'components'),
    os.path.join(ESP_MATTER_PATH, 'device_hal', 'device'),
    os.path.join(ESP_MATTER_PATH, 'device_hal'),  # led_driver is here
]

def find_component_dirs():
    """Find all directories with both CMakeLists.txt and idf_component.yml"""
    result = []
    for scan_dir in SCAN_DIRS:
        if not os.path.isdir(scan_dir):
            continue
        # Check if scan_dir itself is a component
        if os.path.isfile(os.path.join(scan_dir, 'idf_component.yml')) and \
           os.path.isfile(os.path.join(scan_dir, 'CMakeLists.txt')):
            result.append((os.path.basename(scan_dir), scan_dir))
            continue
        # Check subdirectories
        try:
            for item in sorted(os.listdir(scan_dir)):
                item_path = os.path.join(scan_dir, item)
                if os.path.isdir(item_path) and \
                   os.path.isfile(os.path.join(item_path, 'idf_component.yml')) and \
                   os.path.isfile(os.path.join(item_path, 'CMakeLists.txt')):
                    result.append((item, item_path))
        except OSError:
            pass
    return result

def main():
    component_dirs = find_component_dirs()
    print(f"Found {len(component_dirs)} components with idf_component.yml:")
    for name, path in component_dirs:
        print(f"  {name}: {path}")

    manifests = []
    for name, path in component_dirs:
        try:
            manifest = ManifestManager(path, name).load()
            manifests.append(manifest)
        except Exception as e:
            print(f"  WARNING: Failed to load manifest for {name}: {e}")

    project_req = ProjectRequirements(manifests)
    new_hash = project_req.manifest_hash
    print(f"\nComputed manifest_hash: {new_hash}")

    direct_deps = project_req.direct_dep_names
    print(f"Computed direct_dep_names: {sorted(direct_deps)}")

    # Update lock file (YAML format)
    with open(LOCK_FILE, 'r') as f:
        lock_data = yaml.safe_load(f)

    old_hash = lock_data.get('manifest_hash', 'N/A')
    print(f"Old manifest_hash:     {old_hash}")
    print(f"Old direct_dependencies: {sorted(lock_data.get('direct_dependencies', []))}")

    changed = False
    if old_hash != new_hash:
        lock_data['manifest_hash'] = new_hash
        changed = True
        print(f"  manifest_hash: {old_hash} -> {new_hash}")

    # Fix direct_dependencies - only add idf if not present in direct deps already
    # The lock file typically includes 'idf' in the list even if it's not a manifest dep
    old_direct = set(lock_data.get('direct_dependencies', []))
    # computed deps + 'idf' if it was in the old list and we don't have all of them
    new_direct = set(direct_deps)
    # Also include 'idf' since lock file normally has it (from IDF's internal solver)
    if 'idf' in old_direct:
        new_direct.add('idf')
    if old_direct != new_direct:
        print(f"  direct_dependencies old: {sorted(old_direct)}")
        print(f"  direct_dependencies new: {sorted(new_direct)}")
        lock_data['direct_dependencies'] = sorted(new_direct)
        changed = True

    if not changed:
        print("\nLock file already matches. No update needed.")
        return

    with open(LOCK_FILE, 'w') as f:
        yaml.dump(lock_data, f, default_flow_style=False, sort_keys=False)

    print(f"\nUpdated dependencies.lock.")

if __name__ == '__main__':
    main()
