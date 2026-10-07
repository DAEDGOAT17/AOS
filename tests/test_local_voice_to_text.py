import sys
import unittest

from importlib.util import module_from_spec, spec_from_file_location

MODULE_PATH = "/home/dead-goat/AOS/AOS/tools/local_voice_to_text.py"

spec = spec_from_file_location("local_voice_to_text", MODULE_PATH)
mod = module_from_spec(spec)
sys.modules[spec.name] = mod
spec.loader.exec_module(mod)


class LocalVoiceToTextTests(unittest.TestCase):
    def test_command_mapping_works_for_common_shell_actions(self):
        self.assertEqual(mod.local_voice_to_text("show me the system status"), "sysinfo")
        self.assertEqual(mod.local_voice_to_text("tell me about the state of the machine"), "sysinfo")
        self.assertEqual(mod.local_voice_to_text("list the files"), "ls /")
        self.assertEqual(mod.local_voice_to_text("clear the screen"), "clear")
        self.assertEqual(mod.local_voice_to_text("reboot the system"), "reboot")
        self.assertEqual(mod.local_voice_to_text("help me"), "help")
        self.assertEqual(mod.local_voice_to_text("what can you do"), "help")

    def test_unknown_spoken_phrase_is_transcribed_in_place(self):
        result = mod.local_voice_to_text("hello world from the local voice engine")
        self.assertEqual(result, "hello world from the local voice engine")

    def test_normalization_is_stable_and_deterministic(self):
        self.assertEqual(mod.normalize_utterance("  HELLO   WORLD!!!  "), "hello world")

    def test_infer_os_action_returns_a_command_payload(self):
        payload = mod.infer_os_action("show me the system status")
        self.assertEqual(payload["command"], "sysinfo")
        self.assertEqual(payload["mode"], "known")


if __name__ == "__main__":
    unittest.main()
