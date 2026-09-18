import contextlib
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import flash_batch


class FlashBatchTests(unittest.TestCase):
    def test_native_usb_filter(self):
        ports = [
            SimpleNamespace(device="/dev/cu.usbmodem1", vid=0x303A, pid=0x1001),
            SimpleNamespace(device="/dev/tty.usbmodem1", vid=0x303A, pid=0x1001),
            SimpleNamespace(device="/dev/cu.other", vid=0x1234, pid=0x1001),
        ]
        serial = SimpleNamespace(tools=SimpleNamespace(list_ports=SimpleNamespace(comports=lambda: ports)))
        with patch.dict("sys.modules", {"serial": serial, "serial.tools": serial.tools}):
            self.assertEqual(flash_batch.stand_ports(), ["/dev/cu.usbmodem1"])

    def test_requires_unplugged_acknowledgment(self):
        with patch.object(flash_batch, "stand_ports", side_effect=[["port"], []]), \
                patch("builtins.input") as prompt, contextlib.redirect_stdout(io.StringIO()):
            flash_batch.arm_next_board()
        self.assertEqual(prompt.call_count, 2)

    def test_waits_for_stable_port(self):
        with patch.object(flash_batch, "stand_ports", side_effect=[[], ["port"], ["port"]]), \
                patch.object(flash_batch.time, "monotonic", side_effect=[0, 1, 2]), \
                patch.object(flash_batch.time, "sleep"), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(flash_batch.wait_for_board(), "port")

    def test_rejects_multiple_boards(self):
        with patch.object(flash_batch, "stand_ports", return_value=["one", "two"]), \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "Multiple"):
                flash_batch.wait_for_board()

    def test_uses_generated_build_arguments(self):
        config = {
            "extra_esptool_args": {"chip": "esp32s3", "before": "default_reset", "after": "hard_reset", "stub": True},
            "flash_files": {"0x10000": "app.bin"},
        }
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "flasher_args.json").write_text(json.dumps(config))
            (build / "flash_args").write_text("0x10000 app.bin")
            (build / "app.bin").write_bytes(b"test")
            with patch.object(flash_batch, "BUILD", build):
                command = flash_batch.flash_command("/dev/cu.test")
                self.assertIn("/dev/cu.test", command)
                self.assertIn("hard-reset", command)
                self.assertEqual(command[-2:], ["write-flash", "@flash_args"])
                config["extra_esptool_args"]["chip"] = "esp32"
                (build / "flasher_args.json").write_text(json.dumps(config))
                with self.assertRaisesRegex(RuntimeError, "not for ESP32-S3"):
                    flash_batch.flash_command("port")

    def test_external_failure_is_not_retried(self):
        error = flash_batch.subprocess.CalledProcessError(2, ["esptool"])
        with patch.object(flash_batch.subprocess, "run", side_effect=error) as runner, \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(flash_batch.subprocess.CalledProcessError):
                flash_batch.run(["esptool"], Path("."))
        runner.assert_called_once_with(["esptool"], cwd=Path("."), check=True, shell=False)

    def run_batch(self, commands):
        with patch.object(flash_batch.sys, "argv", ["flash_batch.py", "--count", "2"]), \
                patch.object(flash_batch.sys, "platform", "darwin"), \
                patch.object(flash_batch.shutil, "which", return_value="/idf/tools/idf.py"), \
                patch.object(flash_batch.importlib.util, "find_spec", return_value=object()), \
                patch.object(flash_batch, "arm_next_board") as arm, \
                patch.object(flash_batch, "wait_for_board", side_effect=["one", "two"]), \
                patch.object(flash_batch, "flash_command", side_effect=lambda port: ["flash", port]), \
                patch.object(flash_batch, "run", side_effect=commands) as runner, \
                contextlib.redirect_stdout(io.StringIO()) as output, \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            result = flash_batch.main()
        return result, arm, runner, output.getvalue(), errors.getvalue()

    def test_builds_once_and_stops_at_requested_count(self):
        result, arm, runner, output, errors = self.run_batch([None, None, None])
        self.assertEqual(result, 0)
        self.assertEqual(arm.call_count, 2)
        self.assertEqual(runner.call_count, 3)
        self.assertEqual(runner.call_args_list[0].args[0][-1], "build")
        self.assertEqual(runner.call_args_list[1].args[0], ["flash", "one"])
        self.assertEqual(runner.call_args_list[2].args[0], ["flash", "two"])
        self.assertIn("2 stand(s) flashed and verified", output)
        self.assertEqual(errors, "")

    def test_failed_flash_stops_batch_without_success(self):
        failure = flash_batch.subprocess.CalledProcessError(1, ["flash", "one"])
        result, arm, runner, output, errors = self.run_batch([None, failure])
        self.assertEqual(result, 1)
        self.assertEqual(arm.call_count, 1)
        self.assertEqual(runner.call_count, 2)
        self.assertNotIn("SUCCESS", output)
        self.assertIn("0 stand(s) completed", errors)


if __name__ == "__main__":
    unittest.main()
