import os
import tempfile
import unittest
from importlib.util import module_from_spec, spec_from_file_location

MODULE_PATH = "/home/dead-goat/AOS/AOS/tools/harvis_agent_loop.py"

spec = spec_from_file_location("harvis_agent_loop", MODULE_PATH)
mod = module_from_spec(spec)

try:
    spec.loader.exec_module(mod)
except FileNotFoundError:
    mod = None


class HarvisAgentLoopTests(unittest.TestCase):
    def test_run_agent_cycle_returns_plan_and_ai(self):
        self.assertIsNotNone(mod, "harvis_agent_loop.py should exist")
        result = mod.run_agent_cycle(
            "Make the OS self-improving",
            "Kernel and agent memory are present",
            prompt="status",
            endpoint="http://127.0.0.1:11434/api/generate",
        )
        self.assertIn("goal", result)
        self.assertIn("plan", result)
        self.assertIn("ai_response", result)
        self.assertIn("steps", result["plan"])

    def test_save_cycle_writes_json(self):
        payload = {"goal": "Demo goal", "plan": {"steps": ["step a"]}, "ai_response": "ok"}
        fd, path = tempfile.mkstemp(suffix=".json")
        os.close(fd)
        try:
            out = mod.save_agent_cycle(payload, path)
            self.assertEqual(out, path)
            with open(path, "r", encoding="utf-8") as fh:
                data = fh.read()
            self.assertIn("Demo goal", data)
            self.assertIn("step a", data)
        finally:
            if os.path.exists(path):
                os.remove(path)


if __name__ == "__main__":
    unittest.main()
