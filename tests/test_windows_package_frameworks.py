"""Game-free release-validator tests; no Windows, .NET or LLVM is executed.

The ZIP members are synthetic placeholders, not valid PE binaries. Only run()
(the external LLVM metadata boundary) is mocked; main(), files() and pe() run.
Run with: python -I tests/test_windows_package_frameworks.py
"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]


class WindowsPackageFrameworkTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location(
            "check_windows_files_under_test", ROOT / "scripts/check_windows_files.py")
        self.checker = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.checker)

    def check_package(self, runtime_options):
        checker = self.checker
        # Explicit fake LLVM text, not evidence that these bytes are PE files.
        def mock_llvm(*cmd):
            tool, option, path = cmd
            if tool == "mock-readobj" and option == "--coff-resources":
                return "\n".join("Type: " + t for t in checker.SETUP_RESOURCES)
            if tool == "mock-readobj" and option == "--coff-load-config":
                return "DependentLoadFlags: 0x800\n"
            if tool == "mock-objdump" and option == "-p":
                return ("file format coff-x86-64\nMagic 020b\nSubsystem 00000002\n" +
                        "\n".join("DLL Name: " + n for n in checker.SETUP_NEEDS))
            raise AssertionError("unexpected external-tool request: %r" % (cmd,))

        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "synthetic-windows.zip"
            members = {name: b"synthetic placeholder" for name in checker.TOP}
            members.update({"LongshotBuilder/" + name: b"synthetic placeholder"
                            for name in checker.BUILDER})
            members["VERSION"] = members["LongshotBuilder/tools/version.txt"] = b"v0-test\n"
            members["LongshotBuilder/LongshotBuilder-Setup.exe"] = members["LongshotBuilder.exe"]
            members["LongshotBuilder/LongshotBuilder.exe"] = (
                "Longshot Builder".encode("utf-16-le") + b"LongshotBuilder.dll")
            members["LongshotBuilder/LongshotBuilder.runtimeconfig.json"] = json.dumps(
                {"runtimeOptions": runtime_options}).encode("utf-8")
            members["LongshotBuilder/LongshotBuilder.deps.json"] = json.dumps(
                {"runtimeTarget": {"name": ".NETCoreApp,Version=v8.0/win-x64"}}).encode("utf-8")
            with zipfile.ZipFile(archive, "w") as z:
                for name, data in sorted(members.items()):
                    z.writestr(zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0)), data)
            stdout, stderr = io.StringIO(), io.StringIO()
            with patch.object(checker, "run", side_effect=mock_llvm), patch.object(
                    sys, "argv", ["check_windows_files.py", "--objdump", "mock-objdump",
                                  "--readobj", "mock-readobj", str(archive)]), \
                    contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                status = checker.main()
            return status, stdout.getvalue(), stderr.getvalue(), list(checker.problems)

    @staticmethod
    def self_contained():
        return {"tfm": "net8.0", "includedFrameworks": [
            {"name": "Microsoft.NETCore.App", "version": "8.0.0"}]}

    def test_included_framework_alone_is_accepted(self):
        status, stdout, stderr, problems = self.check_package(self.self_contained())
        self.assertEqual((status, stderr, problems), (0, "", []), stdout)
        self.assertTrue(stdout.endswith("ok\n"))

    def test_mixed_framework_object_is_rejected(self):
        options = self.self_contained()
        options["framework"] = {"name": "Microsoft.NETCore.App", "version": "8.0.0"}
        status, stdout, stderr, problems = self.check_package(options)
        self.assertEqual(status, 1, stdout + stderr)
        self.assertTrue(problems)
        self.assertIn("self-contained", stdout)
        self.assertNotIn("\nok\n", stdout)

    def assert_mixed_rejected(self, key, value):
        options = self.self_contained()
        options[key] = value
        status, stdout, stderr, problems = self.check_package(options)
        self.assertEqual(status, 1, stdout + stderr)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("self-contained", problems[0])
        self.assertIn("1 problem(s)", stderr)

    def test_mixed_frameworks_array_is_rejected(self):
        self.assert_mixed_rejected("frameworks", [
            {"name": "Microsoft.NETCore.App", "version": "8.0.0"}])

    def test_mixed_empty_frameworks_is_rejected(self):
        self.assert_mixed_rejected("frameworks", [])

    def test_mixed_null_framework_is_rejected(self):
        self.assert_mixed_rejected("framework", None)

    def test_mixed_null_frameworks_is_rejected(self):
        self.assert_mixed_rejected("frameworks", None)

    def test_net9_remains_rejected(self):
        options = {"tfm": "net9.0", "includedFrameworks": [
            {"name": "Microsoft.NETCore.App", "version": "9.0.0"}]}
        status, stdout, stderr, problems = self.check_package(options)
        self.assertEqual(status, 1, stdout + stderr)
        self.assertEqual(len(problems), 2, problems)
        self.assertIn("not net8.0", stdout)
        self.assertIn("does not carry Microsoft.NETCore.App 8", stdout)

    def test_missing_included_framework_remains_rejected(self):
        status, stdout, stderr, problems = self.check_package({"tfm": "net8.0"})
        self.assertEqual(status, 1, stdout + stderr)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("does not carry Microsoft.NETCore.App 8", stdout)

    def test_windows_desktop_remains_rejected(self):
        options = self.self_contained()
        options["includedFrameworks"].append(
            {"name": "Microsoft.WindowsDesktop.App", "version": "8.0.0"})
        status, stdout, stderr, problems = self.check_package(options)
        self.assertEqual(status, 1, stdout + stderr)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("carries Microsoft.WindowsDesktop.App", stdout)


if __name__ == "__main__":
    unittest.main()
