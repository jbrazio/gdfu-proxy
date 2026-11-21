"""Compiles the proxy copy of the GDFU frame codec and checks Python parity."""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

from protocol import FrameError, decode_frame, encode_frame


class ProxyParityTests(unittest.TestCase):
    def test_proxy_codec_round_trips_canonical_vectors(self) -> None:
        """Catches proxy firmware framing drifting from the canonical Python codec."""
        root = Path(__file__).parents[1]
        frame_c = root / "src" / "gdfu_frame.c"
        cli_c = root / "tests" / "gdfu_frame_cli.c"
        self.assertTrue(frame_c.exists(), "src/gdfu_frame.c missing")

        with tempfile.TemporaryDirectory() as output:
            binary = Path(output) / "gdfu-proxy-frame"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(root / "src"), str(frame_c), str(cli_c), "-o", str(binary)],
                check=True,
            )
            vectors = json.loads((root / "tests" / "test-vectors.json").read_text(encoding="utf-8"))["frames"]
            for vector in vectors:
                raw_hex = vector["hex"].lower()
                try:
                    frame = decode_frame(bytes.fromhex(raw_hex))
                    expected = encode_frame(frame).hex()
                    valid = True
                except FrameError:
                    expected = "invalid"
                    valid = False
                self.assertEqual(vector.get("valid", True), valid, f"validity mismatch for {vector['name']}")
                result = subprocess.run(
                    [str(binary), "roundtrip", raw_hex],
                    capture_output=True, text=True, check=True,
                )
                self.assertEqual(result.stdout.strip(), expected, f"vector mismatch for {vector['name']}")


if __name__ == "__main__":
    unittest.main()
