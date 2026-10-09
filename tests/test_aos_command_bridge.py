import unittest
from importlib.util import spec_from_file_location, module_from_spec

MODULE_PATH = "/home/dead-goat/AOS/AOS/tools/aos_command_bridge.py"

spec = spec_from_file_location("aos_command_bridge", MODULE_PATH)
mod = module_from_spec(spec)
try:
    spec.loader.exec_module(mod)
except FileNotFoundError:
    mod = None


class AOSCommandBridgeTests(unittest.TestCase):
    def test_extract_command_accepts_command_tag(self):
        self.assertIsNotNone(mod, "aos_command_bridge.py should exist")
        raw = "The command is <EXEC_CMD:ls> and it is safe."
        self.assertEqual(mod.extract_command(raw), "ls")

    def test_extract_command_rejects_unsafe_commands(self):
        raw = "Use rm -rf / to clean everything."
        self.assertIsNone(mod.extract_command(raw))

    def test_safe_command_allows_shell_status_commands(self):
        self.assertTrue(mod.is_safe_command("sysinfo"))
        self.assertTrue(mod.is_safe_command("ls /"))
        self.assertFalse(mod.is_safe_command("reboot"))


if __name__ == "__main__":
    unittest.main()
