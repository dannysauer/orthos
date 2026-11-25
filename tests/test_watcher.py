"""Integration checks for Linux. Build Orthos before running this file."""
import os
import resource
from pathlib import Path
import subprocess
import tempfile
import time
import unittest


ORTHOS = Path(__file__).resolve().parents[1] / "build/orthos"


class WatcherTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)
        self.tree = self.work / "export"
        self.tree.mkdir()
        self.sub = self.tree / "sub"
        self.sub.mkdir()
        (self.sub / "nested").write_text("original")
        (self.tree / "sibling").write_text("original")
        (self.tree / "link").symlink_to("sibling")
        repo = self.work / "repo"
        self.url = repo.as_uri()
        subprocess.run(["svnadmin", "create", repo], check=True)
        subprocess.run(["svn", "import", "-q", "-m", "initial", self.tree,
                        self.url], check=True)
        self.log = self.work / "stdout"
        self.errors = self.work / "stderr"

    def start(self, destination=None, directory=None, descriptor_limit=None):
        def limit_descriptors():
            resource.setrlimit(resource.RLIMIT_NOFILE, (descriptor_limit, descriptor_limit))

        with open(destination or self.log, "wb") as out, self.errors.open("wb") as err:
            self.process = subprocess.Popen([ORTHOS, directory or self.tree, self.url],
                                            preexec_fn=limit_descriptors if descriptor_limit else None,
                                            stdout=out, stderr=err)
        self.addCleanup(self.stop)

    def stop(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=7)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            self.fail("Watcher did not stop")

    def wait_for(self, condition):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            self.assertIsNone(self.process.poll(), self.errors.read_text())
            if condition():
                return
            time.sleep(0.05)
        self.fail(f"Timed out. Alerts: {self.log.read_text()} Errors: {self.errors.read_text()}")

    def watches(self):
        count = 0
        for entry in Path(f"/proc/{self.process.pid}/fdinfo").iterdir():
            try:
                count += entry.read_text().count("inotify wd:")
            except FileNotFoundError:
                continue
        return count

    @unittest.skipIf(os.geteuid() == 0, "root can read mode-000 files")
    def test_unreadable_subtree(self):
        self.sub.chmod(0)
        self.addCleanup(self.sub.chmod, 0o700)
        self.start()
        self.wait_for(lambda: f"unverifiable {self.sub}\n" in self.log.read_text())
        (self.tree / "sibling").write_text("edited")
        self.wait_for(lambda: f"contents differ {self.tree}/sibling\n" in self.log.read_text())
        self.sub.chmod(0o700)
        (self.sub / "nested").write_text("changed")
        self.wait_for(lambda: f"contents differ {self.sub}/nested\n" in self.log.read_text())
        self.assertEqual(self.log.read_text().count("unverifiable "), 1)

    def test_rotations_and_types(self):
        self.start()
        self.wait_for(lambda: self.watches() == 2)
        for i in range(3):
            self.sub.rename(self.work / f"rotated-{i}")
            self.wait_for(lambda: self.watches() == 1)
            self.sub.mkdir()
            self.wait_for(lambda: self.watches() == 2)
        link = self.tree / "link"
        link.unlink()
        link.symlink_to("elsewhere")
        self.wait_for(lambda: f"link differs {link}\n" in self.log.read_text())
        file = self.tree / "sibling"
        file.unlink()
        os.mkfifo(file)
        self.wait_for(lambda: f"type differs {file}\n" in self.log.read_text())

    def test_root_returns_without_a_watch(self):
        self.start()
        self.wait_for(lambda: self.watches() == 2)
        old = self.work / "old-export"
        self.tree.rename(old)
        self.wait_for(lambda: f"missing {self.tree}\n" in self.log.read_text()
                      and self.watches() == 0)
        (old / "sibling").write_text("edited")
        old.rename(self.tree)
        self.wait_for(lambda: f"contents differ {self.tree}/sibling\n" in self.log.read_text())

    def test_parent_replaced(self):
        parent = self.work / "deployment"
        parent.mkdir()
        target = parent / "export"
        self.tree.rename(target)
        self.start(directory=target)
        self.wait_for(lambda: self.watches() == 2)
        parent.rename(self.work / "retired")
        parent.mkdir()
        subprocess.run(["svn", "export", "-q", self.url, target], check=True)
        (target / "sibling").write_text("changed")
        self.wait_for(lambda: f"contents differ {target}/sibling\n" in self.log.read_text())

    def test_new_revision_export(self):
        current = self.work / "current"
        current.symlink_to(self.tree)
        self.start(directory=current)
        self.wait_for(lambda: self.watches() == 2)
        addition = self.work / "release-note"
        addition.write_text("committed in revision 2")
        subprocess.run(["svn", "import", "-q", "-m", "release 2", addition,
                        self.url + "/release-note"], check=True)
        target = self.work / "release2"
        subprocess.run(["svn", "export", "-q", self.url, target], check=True)
        replacement = self.work / "next"
        replacement.symlink_to(target)
        replacement.replace(current)

        # A new local file gives us a visible signal that the release was checked.
        probe = target / "probe"
        probe.touch()
        first = f"r2 unexpected {probe}\n"
        self.wait_for(lambda: first in self.log.read_text())
        (target / "release-note").write_text("local edit")
        second = f"r2 contents differ {target}/release-note\n"
        self.wait_for(lambda: second in self.log.read_text())
        self.assertEqual(self.log.read_text(), first + second)

    def test_descriptor_pressure(self):
        # Use a versioned deep subtree so both comparison passes need descriptors.
        deep = self.sub
        for _ in range(20):
            deep = deep / "level"
        deep.mkdir(parents=True)
        (deep / "leaf").write_text("original")
        subprocess.run(["svn", "import", "-q", "-m", "deep", self.sub / "level",
                        self.url + "/sub/level"], check=True)
        self.start(descriptor_limit=32)
        self.wait_for(lambda: "unverifiable " in self.log.read_text())
        (self.tree / "sibling").write_text("edited")
        self.wait_for(lambda: f"contents differ {self.tree}/sibling\n" in self.log.read_text())

    def test_failed_output(self):
        (self.tree / "extra").touch()
        self.start("/dev/full")
        self.assertGreater(self.process.wait(timeout=10), 0)
        self.assertIn("Writing alert to stdout", self.errors.read_text())


if __name__ == "__main__":
    unittest.main()
