import platform
import os
import tempfile
import subprocess
import unittest
from pathlib import Path

from dataclasses import dataclass

THIS_DIR = Path(__file__).parent.resolve()


def run_west(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["west", *args],
        capture_output=True,
        text=True,
        cwd=THIS_DIR,
    )


@dataclass
class NotFound:
    text: str


class WestCommandsTests(unittest.TestCase):
    WEST_TOPDIR: Path
    BUILD_DIR: Path

    @classmethod
    def setUpClass(cls):
        cls.WEST_TOPDIR = Path(
            subprocess.run(
                ["west", "topdir"],
                capture_output=True,
                text=True,
            ).stdout.strip()
        )
        selected = os.environ.get("OWNER_TEST_OUTPUT")
        cls.OUTPUT = (
            Path(selected).resolve()
            if selected
            else Path(tempfile.mkdtemp(prefix="rip-owner-"))
        )
        if selected:
            cls.OUTPUT.mkdir(parents=True, exist_ok=False)
        cls.BUILD_DIR = cls.OUTPUT / "firmware"

    def test_review_regressions(self):
        result = subprocess.run(
            ["python3", "-m", "unittest", "discover", "-s", "tests/review", "-v"],
            cwd=THIS_DIR,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        (self.OUTPUT / "review.log").write_text(result.stdout)
        self.assertEqual(result.returncode, 0, result.stdout)

    @unittest.skipUnless(
        platform.system() == "Linux", "zmk-test is only supported on Linux"
    )
    def test_zmk_test(self):
        tests_build = self.OUTPUT / "native"
        result = run_west(["zmk-test", "tests", "-m", ".", "-d", str(tests_build)])
        (self.OUTPUT / "native.log").write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: studio", result.stdout, result.stdout + result.stderr)
        self.assertIn("PASS: rotation", result.stdout, result.stdout + result.stderr)
        self.assertIn("PASS: pipeline", result.stdout, result.stdout + result.stderr)
        from tests_native import verify_native

        verify_native(tests_build, THIS_DIR / "tests")

    def test_zmk_build(self):
        artifacts_and_expected_config: dict[str, list[str | NotFound]] = {
            "my_awesome_keyboard_with_custom_rpc_support": [
                "CONFIG_MY_AWESOME_KEYBOARD_SPECIAL_FEATURE=y",
                "CONFIG_ZMK_STUDIO=y",
                "CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR=y",
                "CONFIG_ZMK_RUNTIME_INPUT_ROTATION=y",
                "CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_STUDIO_RPC=y",
            ],
            "my_awesome_keyboard_without_custom_rpc_support": [
                "CONFIG_MY_AWESOME_KEYBOARD_SPECIAL_FEATURE=y",
                "# CONFIG_ZMK_STUDIO is not set",
                "CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR=y",
                "CONFIG_ZMK_RUNTIME_INPUT_ROTATION=y",
                NotFound("CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_STUDIO_RPC"),
            ],
            # Hardware-free Renode testing artifact (see README.md's
            # "Hardware-free Renode testing" section): built with the
            # renode-studio-uart snippet supplied by this module instead of
            # the real USB-carried
            # studio-rpc-usb-uart snippet. Verify the snippet's Kconfig
            # actually took effect -- the Renode-only transport enabled, the
            # real USB-gated one and USB itself disabled.
            "renode_smoke_test": [
                "CONFIG_ZMK_STUDIO=y",
                "CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR=y",
                "CONFIG_ZMK_RUNTIME_INPUT_ROTATION=y",
                "CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_STUDIO_RPC=y",
                "CONFIG_ZMK_RENODE_STUDIO_UART_TRANSPORT=y",
                "# CONFIG_ZMK_STUDIO_TRANSPORT_UART is not set",
                "# CONFIG_ZMK_USB is not set",
            ],
        }

        result = run_west(
            [
                "zmk-build",
                "tests/zmk-config/config",
                "-m",
                "tests/zmk-config",
                ".",
                "--extra-module-auto-discovery",
                "zmk-config",
                "current",
                "-q",
                "-d",
                str(self.BUILD_DIR),
                "--parallelism",
                "1",
            ]
        )
        (self.OUTPUT / "firmware.log").write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        for artifact, entries in artifacts_and_expected_config.items():
            config_path = self.BUILD_DIR / artifact / "zephyr" / ".config"
            self.assertTrue(config_path.exists(), f"{artifact} .config is missing")
            config_text = config_path.read_text()
            for entry in entries:
                if isinstance(entry, NotFound):
                    if entry.text in config_text:
                        self.fail(
                            f"{entry.text} found in {config_path} for {artifact}, but it should not be present"
                        )
                else:
                    if entry not in config_text:
                        self.fail(f"{entry} not found in {config_path} for {artifact}")
            self.assertTrue(
                (config_path.parent / "zmk.uf2").exists(),
                f"{artifact} zmk.uf2 is missing in {config_path.parent}",
            )


if __name__ == "__main__":
    unittest.main()
