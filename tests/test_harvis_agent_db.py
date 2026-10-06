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


class HarvisAgentDBTests(unittest.TestCase):
    def test_store_context_writes_file(self):
        self.assertIsNotNone(mod, "harvis_agent_loop.py should exist")
        with tempfile.TemporaryDirectory() as d:
            entry_path = mod.save_context_entry("self_improvement", "goal: keep learning", db_root=d)
            self.assertTrue(os.path.exists(entry_path))
            with open(entry_path, "r", encoding="utf-8") as fh:
                text = fh.read()
            self.assertIn("goal: keep learning", text)

    def test_run_agent_cycle_can_persist_context(self):
        with tempfile.TemporaryDirectory() as d:
            result = mod.run_agent_cycle(
                "Make the OS self-improving",
                "Kernel and agent memory are present",
                prompt="status",
                endpoint="http://127.0.0.1:11434/api/generate",
                db_root=d,
            )
            self.assertIn("goal", result)
            self.assertIn("plan", result)
            context_path = os.path.join(d, "self_improvement.txt")
            self.assertTrue(os.path.exists(context_path))


if __name__ == "__main__":
    unittest.main()
