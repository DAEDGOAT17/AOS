import os
import sys
import unittest
from importlib.util import module_from_spec, spec_from_file_location
from unittest.mock import patch

MODULE_PATH = os.path.join(os.path.dirname(__file__), "..", "tools", "harvis_driver_patch.py")
spec = spec_from_file_location("harvis_driver_patch", MODULE_PATH)
mod = module_from_spec(spec)
sys.modules[spec.name] = mod
spec.loader.exec_module(mod)


class HarvisDriverPatchTests(unittest.TestCase):
    def test_waits_for_full_result_marker_across_reads(self):
        class FragmentedTransport:
            chunks = [b"---RESULT:", b" SUCCESS---"]

            def read(self, size):
                return self.chunks.pop(0) if self.chunks else b""

        output = mod.receive_one_of(
            FragmentedTransport(),
            (b"---RESULT: SUCCESS---", b"---RESULT: COMPILE_FAILED---"),
            0.1,
        )

        self.assertIn(b"---RESULT: SUCCESS---", output)

    def test_discovers_supported_pci_device_from_boot_log(self):
        log = (
            b"PCI: Found Network Controller at bus 0 dev 3 func 0 "
            b"[vid=8086 did=100E] -> Ethernet\n"
        )

        candidates = mod.discover_devices(log)

        self.assertEqual(len(candidates), 1)
        self.assertEqual(candidates[0].driver, "e1000")
        self.assertEqual((candidates[0].vendor_id, candidates[0].device_id), (0x8086, 0x100E))

    def test_generates_kernel_compilable_patch_frame(self):
        device = mod.PciCandidate("Network Controller", 0, 3, 0, 0x8086, 0x100E, "e1000")

        patch = mod.generate_patch([device], 'Check "link" and DHCP.')

        self.assertTrue(patch.startswith(b"---BEGIN_DRIVER---\n"))
        self.assertIn(b'aos_apply_network_setup("0,3,0|Check \\\"link\\\" and DHCP.");', patch)
        self.assertTrue(patch.endswith(b"---END_DRIVER---\n"))

    def test_offline_guidance_is_hardware_specific(self):
        device = mod.PciCandidate("Network Controller", 0, 3, 0, 0x8086, 0x100E, "e1000")

        guidance = mod.offline_setup_guidance([device])

        self.assertIn("e1000", guidance)
        self.assertIn("DHCP", guidance)

    def test_retains_unknown_devices_for_setup_guidance(self):
        log = b"PCI: Found Network Controller at bus 0 dev 3 func 0 [vid=1234 did=5678]"

        devices = mod.discover_devices(log)

        self.assertEqual(len(devices), 1)
        self.assertIsNone(devices[0].driver)
        self.assertIn("1234:5678", mod.offline_setup_guidance(devices))

        patch = mod.generate_patch(devices, mod.offline_setup_guidance(devices))

        self.assertIn(b'print_string("AOS SETUP: ', patch)

    def test_uses_hosted_ai_response_when_available(self):
        device = mod.PciCandidate("Network Controller", 0, 3, 0, 0x8086, 0x100E, "e1000")

        class AIResponse:
            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc_value, traceback):
                return False

            def read(self):
                return b'{"response":"Connect Ethernet, then check link and DHCP status."}'

        with patch.object(mod, "urlopen", return_value=AIResponse()):
            guidance, used_ai = mod.generate_setup_guidance([device])

        self.assertTrue(used_ai)
        self.assertEqual(guidance, "Connect Ethernet, then check link and DHCP status.")

    def test_network_endpoint_defaults_to_aos_setup_port(self):
        self.assertEqual(mod.parse_tcp_endpoint("192.168.1.42"), ("192.168.1.42", mod.DEFAULT_SETUP_PORT))
        self.assertEqual(mod.parse_tcp_endpoint("192.168.1.42:9001"), ("192.168.1.42", 9001))


if __name__ == "__main__":
    unittest.main()