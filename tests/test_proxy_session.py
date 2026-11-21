"""Compiles and runs the proxy session/discovery engine on the host."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class ProxySessionTests(unittest.TestCase):
    def test_session_and_discovery_rules_run_on_host(self) -> None:
        """Catches proxy session rules diverging from PROTOCOL_DFU_GFSK.md before flashing."""
        root = Path(__file__).parents[1]
        source = root / "tests" / "test_gdfu_proxy_state.c"
        state_c = root / "src" / "gdfu_proxy_state.c"
        include = root / "src"
        self.assertTrue(source.exists(), "tests/test_gdfu_proxy_state.c missing")
        self.assertTrue(state_c.exists(), "src/gdfu_proxy_state.c missing")

        with tempfile.TemporaryDirectory() as output:
            binary = Path(output) / "gdfu-proxy-state-test"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(include), str(source), str(state_c), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_oled_schedule_rules_run_on_host(self) -> None:
        """OLED scheduling must stay bounded during streaming."""
        root = Path(__file__).parents[1]
        src = root / "src"
        test = root / "tests" / "test_oled_schedule.cpp"
        self.assertTrue(test.exists(), "tests/test_oled_schedule.cpp missing")
        self.assertTrue((src / "oled_schedule.h").exists(), "src/oled_schedule.h missing")
        with tempfile.TemporaryDirectory() as output:
            binary = Path(output) / "oled-schedule-test"
            subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(src),
                 str(test), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_file_management_rules_run_on_host(self) -> None:
        """Upload rules plus the GFSK ZIP package validator, including hostile archives."""
        root = Path(__file__).parents[1]
        src = root / "src"
        with tempfile.TemporaryDirectory() as output:
            binary = Path(output) / "gdfu-file-test"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(src),
                 str(root / "tests" / "test_gdfu_file.c"),
                 str(src / "gdfu_file.c"), str(src / "gdfu_frame.c"), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_radio_settings_rules_run_on_host(self) -> None:
        """Persisted radio settings default invalid or missing values safely."""
        root = Path(__file__).parents[1]
        src = root / "src"
        with tempfile.TemporaryDirectory() as output:
            binary = Path(output) / "gdfu-radio-settings-test"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(src),
                 str(root / "tests" / "test_gdfu_radio_settings.c"),
                 str(src / "gdfu_radio_settings.c"), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
