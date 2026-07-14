"""Atomic, content-addressed publication helpers for generated trees."""

from __future__ import annotations

import atexit
import os
import pathlib
import shutil
import tempfile


class AtomicOutputDir:
    """Stage beside the live tree and publish it with a directory swap.

    Existing files are hard-linked into staging when possible. Changed files
    must therefore be replaced, never modified in place; ``write_if_changed``
    provides that contract. An interrupted swap is recovered on the next run.
    """

    def __init__(self, target: pathlib.Path):
        self.target = target.resolve()
        self.parent = self.target.parent
        self.parent.mkdir(parents=True, exist_ok=True)
        self.previous = self.parent / f".{self.target.name}.snesrecomp-previous"
        self.staging: pathlib.Path | None = None
        self.published = False

        self._recover_interrupted_publish()
        raw = tempfile.mkdtemp(
            prefix=f".{self.target.name}.snesrecomp-staging-",
            dir=str(self.parent))
        self.staging = pathlib.Path(raw)
        if self.target.exists():
            self._link_existing_tree(self.target, self.staging)
        atexit.register(self.cleanup)

    def _recover_interrupted_publish(self) -> None:
        if self.previous.exists() and not self.target.exists():
            os.replace(self.previous, self.target)
        elif self.previous.exists() and self.target.exists():
            shutil.rmtree(self.previous)

    @staticmethod
    def _link_existing_tree(source: pathlib.Path,
                            destination: pathlib.Path) -> None:
        for root, dirnames, filenames in os.walk(source):
            root_path = pathlib.Path(root)
            relative = root_path.relative_to(source)
            dest_root = destination / relative
            dest_root.mkdir(parents=True, exist_ok=True)
            for dirname in dirnames:
                (dest_root / dirname).mkdir(exist_ok=True)
            for filename in filenames:
                src = root_path / filename
                dst = dest_root / filename
                try:
                    os.link(src, dst)
                except OSError:
                    shutil.copy2(src, dst)

    def publish(self) -> None:
        if self.staging is None or self.published:
            raise RuntimeError("generated-output workspace is not publishable")
        if self.previous.exists():
            shutil.rmtree(self.previous)
        moved_live = False
        try:
            if self.target.exists():
                os.replace(self.target, self.previous)
                moved_live = True
            os.replace(self.staging, self.target)
            self.staging = None
            self.published = True
        except BaseException:
            if moved_live and not self.target.exists() and self.previous.exists():
                os.replace(self.previous, self.target)
            raise
        if self.previous.exists():
            shutil.rmtree(self.previous)

    def cleanup(self) -> None:
        if self.staging is not None and self.staging.exists():
            shutil.rmtree(self.staging, ignore_errors=True)
        self.staging = None


def write_if_changed(path: pathlib.Path, content: str) -> bool:
    """Atomically replace ``path`` only when normalized content differs."""
    try:
        if path.read_text(encoding="utf-8") == content:
            return False
    except (FileNotFoundError, OSError, UnicodeDecodeError):
        pass

    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(
        prefix=f".{path.name}.tmp-", dir=str(path.parent), text=True)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
        fd = -1
        os.replace(tmp_name, path)
    except BaseException:
        if fd >= 0:
            try:
                os.close(fd)
            except OSError:
                pass
        try:
            os.unlink(tmp_name)
        except OSError:
            pass
        raise
    return True
