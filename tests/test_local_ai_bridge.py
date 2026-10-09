import json
import unittest
from importlib.util import spec_from_file_location, module_from_spec


MODULE_PATH = "/home/dead-goat/AOS/AOS/tools/harvis_local_ai.py"


spec = spec_from_file_location("harvis_local_ai", MODULE_PATH)
mod = module_from_spec(spec)


try:
    spec.loader.exec_module(mod)
except FileNotFoundError:
    mod = None


class HarvisLocalAIBridgeTests(unittest.TestCase):
    def test_build_response_contains_prompt_and_system(self):
        self.assertIsNotNone(mod, "harvis_local_ai.py should exist")
        payload = {
            "model": "harvis-local",
            "system": "System prompt",
            "prompt": "List files in /",
            "stream": False,
        }
        response = mod.build_response(payload)
        self.assertIn("response", response)
        self.assertIn("List files in /", response["response"])
        self.assertIn("System prompt", response["response"])

    def test_parse_model_name_falls_back_cleanly(self):
        self.assertEqual(mod.resolve_model_name(""), "gemma3:4b")
        self.assertEqual(mod.resolve_model_name("  "), "gemma3:4b")

    def test_local_os_context_is_loaded_and_local_only(self):
        self.assertIsNotNone(mod)
        context = mod.load_local_os_context()
        self.assertIn("local-only", context.lower())
        self.assertIn("not cloud-hosted", context.lower())

        payload = {"model": "harvis-local", "prompt": "status"}
        response = mod.build_response(payload)
        self.assertIn("local", response["response"].lower())


if __name__ == "__main__":
    unittest.main()
