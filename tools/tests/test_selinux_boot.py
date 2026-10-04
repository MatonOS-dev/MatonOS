#!/usr/bin/env python3
"""Host-only boot-profile regressions; no signing, image builds, disks or VMs."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BootProfiles(unittest.TestCase):
    def shell(self, code, **profile):
        env = {k: v for k, v in os.environ.items()
               if k not in ('MATON_SELINUX_PERMISSIVE', 'MATON_RELEASE',
                            'MATON_REQUIRE_2023_SHIM', 'TARGET_BUILD_VARIANT')}
        env.update(profile)
        return subprocess.run(['bash', '-eu', '-c',
            'source tools/selinux-boot.sh; maton_selinux_init; ' + code],
            cwd=ROOT, env=env, text=True, capture_output=True)

    def test_default_and_development(self):
        for profile, mode in [({}, 'enforcing'),
                              ({'MATON_SELINUX_PERMISSIVE': '1'}, 'permissive')]:
            result = self.shell('maton_selinux_cmdline "quiet"', **profile)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), f'quiet androidboot.selinux={mode}')

    def test_overrides_cannot_bypass_gate(self):
        for option in ['androidboot.selinux=permissive', 'enforcing=0', 'selinux=0',
                       'androidboot.enforcing=0', 'androidboot.selinux=disabled',
                       'security=apparmor', 'lsm=capability', '--',
                       '"androidboot.selinux=permissive"',
                       'androidboot.selinux=enforcing\tandroidboot.selinux=permissive']:
            with self.subTest(option=option):
                result = self.shell('maton_selinux_validate "$BAD_OPTION"', BAD_OPTION=option)
                self.assertNotEqual(result.returncode, 0)

    def test_permissive_only_explicitly_authorized(self):
        result = self.shell('maton_selinux_cmdline "quiet androidboot.selinux=permissive"',
                            MATON_SELINUX_PERMISSIVE='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count('androidboot.selinux='), 1)
        for option in ['selinux=0', 'enforcing=0', '--',
                       'androidboot.selinux=enforcing androidboot.selinux=permissive']:
            self.assertNotEqual(self.shell('maton_selinux_validate "$BAD_OPTION"',
                MATON_SELINUX_PERMISSIVE='1', BAD_OPTION=option).returncode, 0)

    def test_existing_enforcing_option_is_not_duplicated(self):
        for cmdline in ['quiet androidboot.selinux=enforcing',
                        'quiet\tandroidboot.selinux=enforcing']:
            result = self.shell('maton_selinux_cmdline "$CMDLINE"', CMDLINE=cmdline)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.count('androidboot.selinux='), 1)

    def test_release_and_invalid_profiles(self):
        for marker in ['MATON_RELEASE', 'MATON_REQUIRE_2023_SHIM', 'TARGET_BUILD_VARIANT']:
            result = self.shell(':', MATON_SELINUX_PERMISSIVE='1',
                                **{marker: 'user' if marker == 'TARGET_BUILD_VARIANT' else '1'})
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('release packaging refuses', result.stderr)
        for profile in [{'MATON_SELINUX_PERMISSIVE': 'yes'}, {'MATON_RELEASE': 'yes'}]:
            self.assertNotEqual(self.shell(':', **profile).returncode, 0)
        self.assertEqual(self.shell(':', MATON_RELEASE='1').returncode, 0)

    def test_packagers_refuse_before_inputs_or_output(self):
        # No valid input paths: guards must run before even looking for them.
        for script in ['make-live.sh', 'make-payload.sh']:
            result = self.shell(f'bash tools/{script} -R', MATON_SELINUX_PERMISSIVE='1')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('release packaging refuses', result.stderr)
            result = self.shell(f'bash tools/{script} -c "enforcing=0"')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('unsupported SELinux/LSM boot override', result.stderr)

    def test_actual_live_and_installed_command_lines(self):
        source = (ROOT / 'tools/make-live.sh').read_text()
        live = source[source.index('cmdline="console='):source.index('info "Building ESP')]
        installed = source[source.index('  installed_a='):source.index('  sb_build_uki "$work/matonos-installed-a')]
        for profile, mode in [({}, 'enforcing'), ({'MATON_SELINUX_PERMISSIVE': '1'}, 'permissive')]:
            result = self.shell('esp_uuid=test; EXTRA_CMDLINE=""; ' + live + installed +
                                '\nprintf "%s\\n" "$cmdline" "$debug_cmdline" "$installed_a" "$installed_b"',
                                **profile)
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = result.stdout.splitlines()
            self.assertIn(f'androidboot.selinux={mode}', lines[0])
            self.assertNotIn('selinux=', lines[1])
            for line in lines[2:]:
                self.assertIn('androidboot.selinux=enforcing', line)
                self.assertNotIn('permissive', line)
            self.assertIn('androidboot.slot_suffix=_a', lines[2])
            self.assertIn('androidboot.slot_suffix=_b', lines[3])

    def test_legacy_installer_checks_normal_and_debug_before_disk_operations(self):
        source = (ROOT / 'tools/installer.sh').read_text()
        # Run the actual scalar parser and guard only, stopping before assets,
        # disk enumeration and all destructive operations.
        parser = source[source.index('declare -A conf=()'):
                        source.index('# secureboot: CPU microcode is a required')]
        for normal, debug, profile, success in [
            ('quiet', 'loglevel=7', {}, True),
            ('quiet androidboot.selinux=permissive', '', {}, False),
            ('quiet', 'androidboot.selinux=permissive', {}, False),
            ('quiet androidboot.selinux=permissive', '',
             {'MATON_SELINUX_PERMISSIVE': '1'}, True),
            ('quiet', 'selinux=0', {'MATON_SELINUX_PERMISSIVE': '1'}, False),
        ]:
            with self.subTest(normal=normal, debug=debug, profile=profile):
                with tempfile.TemporaryDirectory() as directory:
                    Path(directory, 'payload.conf').write_text(
                        'SUPER_SIZE_BYTES=1048576\nSUPER_SHA256=' + '0' * 64 +
                        '\nESP_SIZE_MIB=512\nMIN_USERDATA_MIB=8192\n' +
                        f'KERNEL_CMDLINE="{normal}"\nDEBUG_CMDLINE="{debug}"\n')
                    result = self.shell(
                        'die() { echo "$*" >&2; exit 1; }; ' +
                        'set --; ' + parser.replace(
                            '$(dirname "$(readlink -f "$0")")/selinux-boot.sh',
                            str(ROOT / 'tools/selinux-boot.sh')) +
                        '\nprintf "%s\\n" "$KERNEL_CMDLINE"',
                        PAYLOAD_DIR=directory, LABEL="Android", **profile)
                    self.assertEqual(result.returncode == 0, success, result.stderr)
                    if success and not profile:
                        self.assertIn('androidboot.selinux=enforcing', result.stdout)

    def test_installer_templates(self):
        for path in ['rn-apps/settings/src/installer/createV1Plan.ts',
                     'install/tools/test-install.sh']:
            source = (ROOT / path).read_text()
            self.assertEqual(source.count('androidboot.selinux=enforcing'), 2)
            self.assertNotIn('androidboot.selinux=permissive', source)


if __name__ == '__main__':
    unittest.main()
