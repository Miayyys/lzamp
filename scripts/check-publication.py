#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only publication metadata check; not a legal or security audit."""
import json
import subprocess
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[1]
    manifest = json.loads((root / "manifests/publication.json").read_text())
    pending = []
    for name, repo in manifest["repositories"].items():
        if not repo.get("remote"):
            pending.append(f"{name}: publication remote not selected")
    if not manifest["repositories"]["kernel"].get("integration_commit"):
        pending.append("kernel: integration commit not recorded")
    if manifest["license_review"]["status"] != "complete":
        pending.append("license/provenance review incomplete")
    if not (root / ".gitmodules").is_file():
        pending.append("kernel submodule not registered")
    else:
        result = subprocess.run(["git", "ls-files", "--stage", "--", "linux"],
                                cwd=root, text=True, capture_output=True)
        expected = manifest["repositories"]["kernel"].get("integration_commit")
        if result.returncode or result.stdout.strip() != f"160000 {expected} 0\tlinux":
            pending.append("kernel gitlink missing or differs from manifest")
    suffixes = {".c", ".h", ".S", ".py", ".sh"}
    missing = []
    for directory in ("agent", "mailmsg", "zephyr", "scripts", "tests", "tools"):
        for path in sorted((root / directory).rglob("*")):
            if path.is_symlink() or not path.is_file():
                continue
            if "__pycache__" in path.parts:
                continue
            if path.suffix not in suffixes and path.name != "lzamp-runtime":
                continue
            if "SPDX-License-Identifier:" not in path.read_text(errors="replace")[:2048]:
                missing.append(str(path.relative_to(root)))
    if missing:
        pending.append("some source files lack SPDX identifiers (review required)")
    print(json.dumps({"ready": not pending, "pending": pending,
                      "missing_spdx": missing,
                      "scope": "metadata only; no deployment or Git mutations"}, indent=2))
    return 1 if pending else 0


if __name__ == "__main__":
    raise SystemExit(main())
