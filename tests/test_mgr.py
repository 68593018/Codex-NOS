import os
import socket
import subprocess
import time
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


class NosManagerTest(unittest.TestCase):
    def run_mgr_commands(self, commands, timeout=8):
        proc = subprocess.Popen(
            [str(REPO_ROOT / "nos_mgr")],
            cwd=REPO_ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        try:
            for command in commands:
                proc.stdin.write(command + "\n")
                proc.stdin.flush()
                time.sleep(0.2)
            proc.stdin.write("quit\n")
            proc.stdin.flush()
            output, _ = proc.communicate(timeout=timeout)
            return proc.returncode, output
        except Exception:
            proc.kill()
            proc.communicate(timeout=2)
            raise

    def external_proc_a_is_running(self):
        sock_path = "/tmp/nos_proc_A.sock"
        if not os.path.exists(sock_path):
            return False
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as sock:
                sock.settimeout(0.2)
                sock.connect(sock_path)
                return True
        except OSError:
            return False

    def test_mgr_lists_configured_nodes(self):
        rc, output = self.run_mgr_commands(["show nodes"])

        self.assertEqual(rc, 0, output)
        self.assertIn("NOS Manager", output)
        self.assertIn("ProcA", output)
        self.assertIn("ProcB", output)
        self.assertIn("./nos_ProcA", output)
        self.assertIn("./nos_ProcB", output)

    def test_mgr_start_stop_managed_node(self):
        if self.external_proc_a_is_running():
            self.skipTest("external ProcA is already running")

        rc, output = self.run_mgr_commands([
            "start ProcA",
            "show nodes",
            "stop ProcA",
            "show nodes",
        ], timeout=12)

        self.assertEqual(rc, 0, output)
        self.assertIn("Node ProcA started", output)
        self.assertIn("RUNNING", output)
        self.assertRegex(output, r"Node ProcA (stopped|killed after stop timeout)")


if __name__ == "__main__":
    unittest.main()
