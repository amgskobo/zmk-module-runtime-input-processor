#!/usr/bin/env python3
"""Renode owner controls for the module's explicit Studio-over-UART fixture.

Build the ``renode_smoke_test`` artifact first, then run in the prepared
container:
    west zmk-renode-test tests/renode --elf /absolute/path/to/zmk.elf

The selected zmk-west-commands helper provides the Renode platform and harness.
Its ``boot_single_real`` connects the console and UART1 sockets; this fixture's
renode-studio-uart snippet deliberately enables that UART carrier. The generic
BLE smoke checks boot liveness without a host ELF. The owner cases separately
assert an invalid-subsystem envelope and a real GetLayerInfo custom RPC reply.

The third case retains the known ListInputProcessors UART response timeout.
It is an explicit emulation limitation, not successful list delivery. If a reply
starts arriving, that assertion must fail so the case can be replaced with
positive response and notification checks. Native fixtures separately exercise
the processor/persistence/handler paths; neither proves physical transport.

Helper and Studio schema paths come from the actual selected West projects in
both parent and isolated layouts. There is no recursive or sibling fallback.
The fixture registers cormoran_rip as its sole custom RPC subsystem (index 0).
``renode_test.py`` is intentionally separate from ordinary unittest discovery;
the West Renode entry supplies the exact ELF and emulator setup.
"""

from __future__ import annotations

import os
import sys
import subprocess
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def selected_west_project(name: str) -> Path:
    result = subprocess.run(
        ["west", "list", name, "-f", "{abspath}"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    paths = result.stdout.strip().splitlines()
    if len(paths) != 1 or not Path(paths[0]).is_absolute():
        raise RuntimeError("West must select exactly one absolute project: " + name)
    project = Path(paths[0]).resolve(strict=True)
    if not project.is_dir():
        raise RuntimeError("Selected West project is not a directory: " + name)
    return project


# Direct execution selects the same declared helper as the canonical West entry.
try:
    import renode_harness
except ImportError:
    helper = selected_west_project("zmk-west-commands")
    if not (helper / "scripts/lib/renode/renode_harness.py").is_file():
        raise RuntimeError("Selected zmk-west-commands has no Renode harness")
    sys.path.insert(0, str(helper / "scripts/lib/renode"))
    import renode_harness


SUBSYSTEM_IDENTIFIER = "cormoran_rip"
# This module registers exactly one custom subsystem in the test build, so
# its index is deterministically 0 -- but see the KNOWN RENODE-ENVIRONMENT
# LIMITATION note above: `ListCustomSubsystemRequest` (the normal way to
# discover this at runtime) returns a large response (identifier + UI URL,
# well past the observed size threshold) and so also times out under
# Renode; this test hardcodes the index rather than discovering it.
KNOWN_SUBSYSTEM_INDEX = 0
# Always out of range regardless of how many custom subsystems a given
# module registers -- used to exercise the *working* fast-path dispatch
# (see test_custom_rpc_invalid_index_dispatch).
INVALID_SUBSYSTEM_INDEX = 99


class RenodeRuntimeInputProcessorTests(unittest.TestCase):
    """Boots this module's own Renode-testable ELF once for the whole class
    (like the skill's own T0/T1 tests, boot is the slow part) and exercises
    the cormoran_rip custom subsystem's envelope/dispatch machinery."""

    renode_path: str
    elf: Path
    studio_pb2 = None
    rip_pb2 = None

    @classmethod
    def setUpClass(cls):
        cls.renode_path = renode_harness.find_or_install_renode()
        if cls.renode_path is None:
            raise unittest.SkipTest(
                "Renode is not installed and could not be auto-installed"
            )

        elf_env = os.environ.get("ZMK_RENODE_ELF")
        if not elf_env:
            raise unittest.SkipTest(
                "ZMK_RENODE_ELF not set -- build the Renode-testable ELF first (see README.md)"
            )
        cls.elf = Path(elf_env)
        if not cls.elf.is_file():
            raise unittest.SkipTest(f"ZMK_RENODE_ELF does not exist: {cls.elf}")

        # Core zmk.studio.* messages (Request/Response envelope, core.proto,
        # custom.proto for the generic custom-subsystem envelope).
        studio_proto_dir = selected_west_project("zmk-studio-messages") / "proto/zmk"
        for filename in ("studio.proto", "custom.proto"):
            if not (studio_proto_dir / filename).is_file():
                raise RuntimeError("Selected Studio schema is missing " + filename)
        cls.studio_pb2 = renode_harness.load_studio_pb2(studio_proto_dir)

        # This module's own proto (proto/cormoran/rip/custom.proto, package
        # cormoran.rip) -- compiled separately since it lives outside
        # zmk-studio-messages.
        out_dir = renode_harness.compile_protos(
            [REPO_ROOT / "proto" / "cormoran" / "rip" / "custom.proto"],
            include_dirs=[REPO_ROOT / "proto"],
        )
        sys.path.insert(0, str(out_dir))
        import cormoran.rip.custom_pb2 as rip_pb2  # type: ignore

        cls.rip_pb2 = rip_pb2

    def setUp(self):
        self.session, self.console, self.rpc = renode_harness.boot_single_real(
            self.renode_path, self.elf
        )
        self.addCleanup(self.session.stop)
        self.addCleanup(self.console.close)
        self.addCleanup(self.rpc.close)

        banner = renode_harness.wait_for_text(
            self.console._sock, "Welcome to ZMK", timeout=15
        )
        self.assertIn(
            "Welcome to ZMK", banner, f"never saw ZMK boot banner; got:\n{banner}"
        )

    def _send_call(self, subsystem_index: int, payload: bytes, request_id: int = 1):
        req = self.studio_pb2.Request()
        req.request_id = request_id
        req.custom.call.subsystem_index = subsystem_index
        req.custom.call.payload = payload
        self.rpc.send(req.SerializeToString())

    # -- Affirmative proof the custom-subsystem envelope works -----------

    def test_custom_rpc_invalid_index_dispatch(self):
        """`custom.call` to a subsystem index that doesn't exist proves the
        whole custom-subsystem *envelope* round-trips correctly end to end
        (Request.custom oneof selection, CallRequest field encoding,
        subsystem-count/index validation, meta.simple_error response
        encoding/decoding)."""
        self._send_call(INVALID_SUBSYSTEM_INDEX, b"", request_id=7)

        resp_bytes = self.rpc.read_frame(timeout=10.0)
        self.assertIsNotNone(
            resp_bytes, "no response to custom.call with an invalid index (timeout)"
        )
        resp = self.studio_pb2.Response()
        resp.ParseFromString(resp_bytes)
        self.assertEqual(resp.WhichOneof("type"), "request_response")
        self.assertEqual(resp.request_response.request_id, 7)
        self.assertEqual(resp.request_response.WhichOneof("subsystem"), "meta")
        self.assertEqual(
            resp.request_response.meta.WhichOneof("response_type"), "simple_error"
        )
        # zmk.meta.ErrorConditions.RPC_NOT_FOUND == 2
        self.assertEqual(resp.request_response.meta.simple_error, 2)

    # -- Affirmative proof cormoran_rip's own handler responds ------------

    def test_custom_rpc_get_layer_info_round_trip(self):
        """Sends a real GetLayerInfoRequest to this module's own registered
        subsystem (index 0) and checks the response round-trips as a real
        GetLayerInfoResponse listing the test build's keymap layer(s). The
        test keymap has exactly one layer (see
        tests/zmk-config/boards/shields/my_awesome_keyboard/my_awesome_keyboard.keymap)
        with no explicit display name, so the encoded response stays tiny
        (well under the known Renode-only threshold documented in this
        file's module docstring) -- this is a genuine, reliable end-to-end
        proof that cormoran_rip's own handler (not just the generic
        custom-subsystem envelope) runs and responds under Renode."""
        inner_req = self.rip_pb2.Request()
        inner_req.get_layer_info.SetInParent()
        self._send_call(KNOWN_SUBSYSTEM_INDEX, inner_req.SerializeToString())

        resp_bytes = self.rpc.read_frame(timeout=10.0)
        self.assertIsNotNone(
            resp_bytes,
            "no response to a real GetLayerInfoRequest against cormoran_rip (timeout)",
        )
        resp = self.studio_pb2.Response()
        resp.ParseFromString(resp_bytes)
        self.assertEqual(resp.WhichOneof("type"), "request_response")
        self.assertEqual(resp.request_response.request_id, 1)
        self.assertEqual(resp.request_response.WhichOneof("subsystem"), "custom")

        inner_resp = self.rip_pb2.Response()
        inner_resp.ParseFromString(resp.request_response.custom.call.payload)
        self.assertEqual(inner_resp.WhichOneof("response_type"), "get_layer_info")
        layers = inner_resp.get_layer_info.layers
        self.assertEqual(len(layers), 1, f"expected exactly one layer, got {layers}")
        self.assertEqual(layers[0].index, 0)

    # -- Known Renode limitation: documented, asserted, not silently skipped --

    def test_custom_rpc_list_input_processors_round_trip_KNOWN_BROKEN_UNDER_RENODE(
        self,
    ):
        """Documents the known Renode-environment limitation (see this
        file's module docstring): sending a real ListInputProcessorsRequest
        to this module's own registered subsystem should get back the
        (deliberately empty) ListInputProcessorsResponse, followed by one
        InputProcessorChangedNotification per configured processor (3 in the
        test build) -- and does, on real hardware -- but under Renode the
        burst of callback-encoded notifications this request triggers stalls
        the TX path before/around the small RPC response itself,
        reproducing the same kind of read timeout as the template's
        SampleResponse and fast-keymap's Snapshot before it (see the module
        docstring's 2026-07-09 findings for why this differs from a single
        large response). This test asserts *that exact failure* (a read
        timeout) so it will start failing -- loudly, as a signal to update
        this test to assert the real round trip (and, ideally, drain and
        assert the 3 expected notifications too) -- the day the underlying
        emulation/harness limitation is fixed."""
        inner_req = self.rip_pb2.Request()
        inner_req.list_input_processors.SetInParent()
        self._send_call(KNOWN_SUBSYSTEM_INDEX, inner_req.SerializeToString())

        resp_bytes = self.rpc.read_frame(timeout=10.0)
        self.assertIsNone(
            resp_bytes,
            "custom.call ListInputProcessorsRequest got a response under Renode -- the "
            "known Renode-only limitation documented in this file's module docstring "
            "appears to be fixed! Update this test to assert the real "
            "ListInputProcessorsResponse round trip instead (see "
            "test_custom_rpc_get_layer_info_round_trip for the "
            "request-building/response-parsing pattern), and consider draining/asserting "
            "the 3 expected InputProcessorChangedNotification frames and re-adding "
            "subsystem discovery via ListCustomSubsystemRequest.",
        )


if __name__ == "__main__":
    unittest.main()
