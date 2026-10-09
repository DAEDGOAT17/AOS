import unittest
from importlib.util import spec_from_file_location, module_from_spec
from unittest.mock import patch

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

    def test_submit_task_sends_goal_to_native_aos_agent(self):
        with patch.object(mod, "send_to_aos", return_value="task started") as send:
            result = mod.submit_task("192.168.77.10", 9001, "run a small program")

        self.assertEqual(result, "task started")
        send.assert_called_once_with("192.168.77.10", 9001, "ask run a small program", timeout=5.0)

    def test_submit_task_rejects_multiline_goals(self):
        with self.assertRaises(ValueError):
            mod.submit_task("192.168.77.10", 9001, "first line\nsecond line")


if __name__ == "__main__":
    unittest.main()
