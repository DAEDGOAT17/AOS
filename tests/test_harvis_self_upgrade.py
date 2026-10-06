import os
import tempfile
import unittest
from importlib.util import module_from_spec, spec_from_file_location

MODULE_PATH = "/home/dead-goat/AOS/AOS/tools/harvis_self_upgrade.py"

spec = spec_from_file_location("harvis_self_upgrade", MODULE_PATH)
mod = module_from_spec(spec)

try:
    spec.loader.exec_module(mod)
except FileNotFoundError:
    mod = None


class HarvisSelfUpgradeTests(unittest.TestCase):
    def test_plan_contains_goal_and_steps(self):
        self.assertIsNotNone(mod, "harvis_self_upgrade.py should exist")
        plan = mod.plan_self_improvement(
            "Make the OS agentic and internet-aware",
            "Bootable kernel with agent memory and shell commands",
        )
        self.assertIn("goal", plan)
        self.assertIn("steps", plan)
        self.assertGreater(len(plan["steps"]), 0)

    def test_save_plan_writes_file(self):
        plan = {"goal": "Test goal", "steps": ["step one", "step two"]}
        fd, path = tempfile.mkstemp(suffix=".json")
        os.close(fd)
        try:
            result = mod.save_plan(plan, path)
            self.assertEqual(result, path)
            with open(path, "r", encoding="utf-8") as fh:
                saved = fh.read()
            self.assertIn("Test goal", saved)
            self.assertIn("step one", saved)
        finally:
            if os.path.exists(path):
                os.remove(path)


if __name__ == "__main__":
    unittest.main()
