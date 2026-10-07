import sys
import unittest

from importlib.util import module_from_spec, spec_from_file_location

MODULE_PATH = "/home/dead-goat/AOS/AOS/tools/os_infer_runtime.py"

spec = spec_from_file_location("os_infer_runtime", MODULE_PATH)
mod = module_from_spec(spec)
sys.modules[spec.name] = mod
spec.loader.exec_module(mod)


class LocalOSInferenceRuntimeTests(unittest.TestCase):
    def test_status_intent_maps_to_safe_action(self):
        decision = mod.infer_os_action("show system status")
        self.assertEqual(decision.action, mod.DecisionAction.SHOW_STATUS)
        self.assertTrue(decision.safe)
        self.assertGreaterEqual(decision.confidence, 0.8)

        machine_state = mod.infer_os_action("tell me about the state of the machine")
        self.assertEqual(machine_state.action, mod.DecisionAction.SHOW_STATUS)
        self.assertTrue(machine_state.safe)

    def test_reboot_intent_is_typed_and_safe(self):
        decision = mod.infer_os_action("reboot the system")
        self.assertEqual(decision.action, mod.DecisionAction.REBOOT)
        self.assertTrue(decision.safe)

    def test_unhealthy_state_promotes_review_patch(self):
        decision = mod.infer_os_action("improve the agent", {"health": 0.2, "boot_health": 0.4})
        self.assertEqual(decision.action, mod.DecisionAction.REVIEW_PATCH)
        self.assertTrue(decision.safe)

    def test_unsafe_action_is_blocked_by_allowlist(self):
        runtime = mod.LocalOSInferenceRuntime(allowlist={mod.DecisionAction.SHOW_STATUS})
        decision = runtime.infer("reboot the system")
        self.assertFalse(decision.safe)
        self.assertEqual(decision.action, mod.DecisionAction.REBOOT)

    def test_direct_kernel_action_contract(self):
        decision = mod.infer_os_action("show system status")
        routed = mod.LocalOSInferenceRuntime().route_direct_action(decision)
        self.assertEqual(routed, "sysinfo")
        self.assertTrue(decision.safe)

    def test_local_gguf_adapter_emits_typed_actions(self):
        adapter = mod.LocalGGUFAdapter()
        decision = adapter.decide("show me the system status")
        self.assertEqual(decision.action, mod.DecisionAction.SHOW_STATUS)
        self.assertEqual(decision.target, "kernel")
        self.assertTrue(decision.safe)
        self.assertFalse(decision.requires_review)

    def test_patch_actions_require_review_gate(self):
        adapter = mod.LocalGGUFAdapter()
        decision = adapter.decide("improve the system")
        self.assertEqual(decision.action, mod.DecisionAction.PATCH_CONFIG)
        self.assertFalse(decision.safe)
        self.assertTrue(decision.requires_review)


if __name__ == "__main__":
    unittest.main()
