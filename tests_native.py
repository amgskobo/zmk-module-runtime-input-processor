"""Execute every native fixture three times with preserved, fresh initial state."""

import hashlib
import json
from pathlib import Path
import re
import subprocess


def verify_native(output: Path, tests: Path):
    expected = {p.parent.name for p in tests.glob("*/native_sim.keymap")}
    binaries = {p.parent.parent.name: p for p in output.rglob("zmk.exe")}
    if set(binaries) != expected:
        raise AssertionError(f"Native fixture mismatch: {set(binaries)} != {expected}")
    for case, binary in sorted(binaries.items()):
        fixture = tests / case
        destination = binary.parent.parent / "verified-runs"
        if case in ("pipeline", "rotation"):
            # Dispatch the owner's canonical repeat script, not a substitute.
            subprocess.run(
                ["bash", str(fixture / "repeat.sh"), str(binary), str(destination)],
                check=True,
                timeout=65,
            )
        else:
            destination.mkdir()
            for trial in range(1, 4):
                run = destination / str(trial)
                run.mkdir()
                result = subprocess.run(
                    [str(binary)],
                    cwd=run,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    timeout=20,
                )
                (run / "raw.log").write_text(result.stdout)
                (run / "result.json").write_text(
                    json.dumps(
                        {
                            "exit": result.returncode,
                            "binary_sha256": hashlib.sha256(
                                binary.read_bytes()
                            ).hexdigest(),
                        }
                    )
                    + "\n"
                )
                if result.returncode or re.search(
                    r"FAIL:|ASSERTION FAIL|FATAL ERROR|assertion .*failed",
                    result.stdout,
                    re.I,
                ):
                    raise AssertionError(f"{case}/{trial}: failed, see {run}")
                filtered = subprocess.check_output(
                    [
                        "sed",
                        "-n",
                        "-f",
                        str(fixture / "events.patterns"),
                        str(run / "raw.log"),
                    ],
                    text=True,
                )
                (run / "events.log").write_text(filtered)
                if filtered != (fixture / "keycode_events.snapshot").read_text():
                    raise AssertionError(f"{case}/{trial}: snapshot mismatch")
                for marker in ("rip_persist_snapshot", "rip_rpc_widths_and_get_flags"):
                    if f"PASS: {marker}" not in result.stdout:
                        raise AssertionError(f"{case}/{trial}: missing {marker}")
