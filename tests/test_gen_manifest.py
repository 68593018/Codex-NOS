import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
GEN_SCRIPT = REPO_ROOT / "scripts" / "gen_manifest.py"


BASE_CONFIG = {
    "components.yaml": """
        platform:
          provides:
            - { name: "SVC_LOG", id: 1, type: "embedded" }
        components:
          - name: "Comp-A"
            model: "ModelA"
            id: 10
            provides:
              - { name: "SVC_WORK", id: 100, type: "remote" }
            requires:
              - "SVC_LOG"
          - name: "Comp-B"
            model: "ModelB"
            id: 11
            provides:
              - { name: "SVC_HELPER", id: 101, type: "remote" }
            requires:
              - "SVC_WORK"
    """,
    "models.yaml": """
        models:
          - name: "ModelA"
            lib: "libcomp-a.so"
          - name: "ModelB"
            lib: "libcomp-b.so"
    """,
    "nodes.yaml": """
        nodes:
          - name: "ProcA"
            uds_path: "/tmp/proc_a.sock"
            buffer_profile: "default"
            busy_poll_cycles: 100
            threads:
              - name: "Worker-1"
                components: ["Comp-A", "Comp-B"]
    """,
    "profiles.yaml": """
        profiles:
          - name: "default"
            bins:
              - { size: 64, count: 8 }
    """,
}


class GenManifestTest(unittest.TestCase):
    def run_generator(self, files):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            conf = root / "conf"
            include = root / "include"
            src_core = root / "src" / "core"
            conf.mkdir()
            include.mkdir()
            src_core.mkdir(parents=True)

            for name, content in files.items():
                (conf / name).write_text(
                    textwrap.dedent(content).strip() + "\n",
                    encoding="utf-8",
                )

            out_h = include / "nos_ids.h"
            result = subprocess.run(
                [sys.executable, str(GEN_SCRIPT), str(conf), str(out_h)],
                cwd=REPO_ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            generated = {
                "header": out_h.read_text(encoding="utf-8") if out_h.exists() else "",
                "manifest": (src_core / "manifest_ProcA.c").read_text(encoding="utf-8")
                if (src_core / "manifest_ProcA.c").exists()
                else "",
            }
            return result, generated

    def test_generates_header_and_manifest_for_valid_config(self):
        result, generated = self.run_generator(BASE_CONFIG)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("#define COMP_A", generated["header"])
        self.assertIn("#define SVC_WORK", generated["header"])
        self.assertIn('.name = "ProcA"', generated["manifest"])
        self.assertIn("nos_log_init", generated["manifest"])

    def test_rejects_duplicate_component_id(self):
        files = dict(BASE_CONFIG)
        files["components.yaml"] = files["components.yaml"].replace("id: 11", "id: 10")

        result, _ = self.run_generator(files)

        self.assertEqual(result.returncode, 2)
        self.assertIn("duplicate component id", result.stderr)

    def test_rejects_unknown_required_service(self):
        files = dict(BASE_CONFIG)
        files["components.yaml"] = files["components.yaml"].replace('"SVC_WORK"', '"SVC_MISSING"', 1)

        result, _ = self.run_generator(files)

        self.assertEqual(result.returncode, 2)
        self.assertIn("requires unknown service", result.stderr)

    def test_rejects_unknown_component_in_node(self):
        files = dict(BASE_CONFIG)
        files["nodes.yaml"] = files["nodes.yaml"].replace("Comp-B", "Comp-Missing")

        result, _ = self.run_generator(files)

        self.assertEqual(result.returncode, 2)
        self.assertIn("references unknown component", result.stderr)

    def test_rejects_duplicate_service_id(self):
        files = dict(BASE_CONFIG)
        files["components.yaml"] = files["components.yaml"].replace("id: 101", "id: 100")

        result, _ = self.run_generator(files)

        self.assertEqual(result.returncode, 2)
        self.assertIn("duplicate service id", result.stderr)

    def test_rejects_unknown_model(self):
        files = dict(BASE_CONFIG)
        files["components.yaml"] = files["components.yaml"].replace("model: \"ModelB\"", "model: \"MissingModel\"")

        result, _ = self.run_generator(files)

        self.assertEqual(result.returncode, 2)
        self.assertIn("references unknown model", result.stderr)

    def test_rejects_unknown_buffer_profile(self):
        files = dict(BASE_CONFIG)
        files["nodes.yaml"] = files["nodes.yaml"].replace("buffer_profile: \"default\"", "buffer_profile: \"missing\"")

        result, _ = self.run_generator(files)

        self.assertEqual(result.returncode, 2)
        self.assertIn("references unknown buffer_profile", result.stderr)

    def test_rejects_invalid_buffer_bin(self):
        files = dict(BASE_CONFIG)
        files["profiles.yaml"] = files["profiles.yaml"].replace("count: 8", "count: 0")

        result, _ = self.run_generator(files)

        self.assertEqual(result.returncode, 2)
        self.assertIn("has invalid count", result.stderr)


if __name__ == "__main__":
    unittest.main()
