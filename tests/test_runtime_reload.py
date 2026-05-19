import shutil
import signal
import subprocess
import time
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


class RuntimeReloadTest(unittest.TestCase):
    def run_node_commands(self, commands, mutate=None, timeout=8):
        proc = subprocess.Popen(
            [str(REPO_ROOT / "nos_ProcA")],
            cwd=REPO_ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        try:
            time.sleep(0.4)
            for idx, command in enumerate(commands):
                if mutate:
                    mutate(idx)
                proc.stdin.write(command + "\n")
                proc.stdin.flush()
                time.sleep(0.4)
            proc.stdin.write("quit\n")
            proc.stdin.flush()
            output, _ = proc.communicate(timeout=timeout)
            return proc.returncode, output
        except Exception:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate(timeout=2)
            raise

    def test_reload_success_keeps_component_active(self):
        rc, output = self.run_node_commands([
            "show components",
            "reload Comp-1",
            "show components",
        ])

        self.assertEqual(rc, 0, output)
        self.assertIn("Component Comp-1 reloaded with staged swap.", output)
        self.assertRegex(output, r"Comp-1\s+1\s+Active")

    def test_reload_staged_load_failure_keeps_old_component(self):
        lib_path = REPO_ROOT / "libcomp-1.so"
        backup_path = REPO_ROOT / "libcomp-1.so.testbak"
        self.assertTrue(lib_path.exists(), "libcomp-1.so must be built before runtime tests")
        if backup_path.exists():
            backup_path.unlink()

        def mutate(idx):
            if idx == 1:
                shutil.move(lib_path, backup_path)

        try:
            rc, output = self.run_node_commands([
                "show components",
                "reload Comp-1",
                "show components",
            ], mutate=mutate)
        finally:
            if backup_path.exists():
                shutil.move(backup_path, lib_path)

        self.assertEqual(rc, 0, output)
        self.assertIn("reload staged load failed; old component remains active", output)
        self.assertRegex(output, r"Comp-1\s+1\s+Active")


if __name__ == "__main__":
    unittest.main()
