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

    def test_cli_reports_services_memory_and_log_stats(self):
        rc, output = self.run_node_commands([
            "show services",
            "show memory",
            "log stats",
        ])

        self.assertEqual(rc, 0, output)
        self.assertIn("SVC_LOG", output)
        self.assertIn("SVC_PING", output)
        self.assertIn("SVC_REMOTE_PONG", output)
        self.assertIn("NOS Process Memory Consumption", output)
        self.assertIn("Buffer Pool", output)
        self.assertIn("Scheduler/Threads", output)
        self.assertIn("NOS Logging Statistics", output)
        self.assertIn("Dropped Messages:", output)

    def test_unload_removes_component_from_runtime_list(self):
        rc, output = self.run_node_commands([
            "show components",
            "unload Comp-Pong",
            "show components",
        ])

        self.assertEqual(rc, 0, output)
        self.assertRegex(output, r"Comp-Pong\s+11\s+Active")
        self.assertIn("Component Comp-Pong unloaded.", output)
        after_unload = output.split("Component Comp-Pong unloaded.", 1)[1]
        self.assertNotRegex(after_unload, r"Comp-Pong\s+11\s+Active")

    def test_unload_component_with_timer_keeps_runtime_usable(self):
        rc, output = self.run_node_commands([
            "show components",
            "unload Comp-1",
            "show components",
            "show memory",
            "log stats",
        ])

        self.assertEqual(rc, 0, output)
        self.assertRegex(output, r"Comp-1\s+1\s+Active")
        self.assertIn("Component Comp-1 unloaded.", output)
        after_unload = output.split("Component Comp-1 unloaded.", 1)[1]
        self.assertNotRegex(after_unload, r"Comp-1\s+1\s+Active")
        self.assertIn("NOS Process Memory Consumption", after_unload)
        self.assertIn("NOS Logging Statistics", after_unload)

    def test_remote_ipc_connect_failure_uses_backoff(self):
        rc, output = self.run_node_commands([
            "perf remote 1",
            "perf remote 1",
            "perf remote 1",
            "perf remote 1",
        ])

        self.assertEqual(rc, 0, output)
        self.assertIn("[CLI] Triggering cross-process performance test", output)
        self.assertLessEqual(output.count("IPC connect to /tmp/nos_proc_B.sock failed"), 1)

    def test_show_ipc_reports_remote_connection_state(self):
        rc, output = self.run_node_commands([
            "perf remote 1",
            "show ipc",
        ])

        self.assertEqual(rc, 0, output)
        self.assertIn("IPC Connections", output)
        self.assertIn("/tmp/nos_proc_B.sock", output)
        self.assertIn("BACKOFF", output)
        self.assertIn("IPC Traffic", output)
        self.assertIn("TX-Pkts", output)
        self.assertIn("TX-Err", output)
        self.assertIn("RX-Pkts", output)
        self.assertIn("Drop", output)
        self.assertRegex(output, r"/tmp/nos_proc_B\.sock\s+BACKOFF\s+-1\s+1")
        self.assertRegex(output, r"/tmp/nos_proc_B\.sock\s+0\s+0\s+0\s+0\s+0\s+0\s+0")

    def test_show_stats_reports_registered_metrics(self):
        rc, output = self.run_node_commands([
            "show stats",
            "show stats ipc",
        ])

        self.assertEqual(rc, 0, output)
        self.assertIn("NOS Stats", output)
        self.assertIn("ipc", output)
        self.assertIn("tx_packets", output)
        self.assertIn("rx_packets", output)
        self.assertIn("log", output)
        self.assertIn("dropped_messages", output)
        self.assertIn("buffer", output)
        self.assertIn("total_memory_bytes", output)
        self.assertIn("scheduler", output)
        self.assertIn("NOS Stats: ipc", output)


if __name__ == "__main__":
    unittest.main()
