#!/usr/bin/env python3
"""Validate that every TeleSystem component has an ESP-IDF manifest."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPONENTS_DIR = ROOT / "components"


def main() -> int:
    component_dirs = sorted(path for path in COMPONENTS_DIR.iterdir() if path.is_dir())
    failures: list[str] = []

    for component_dir in component_dirs:
        cmake_file = component_dir / "CMakeLists.txt"
        if not cmake_file.exists():
            continue
        manifest = component_dir / "idf_component.yml"
        if not manifest.exists():
            failures.append(f"{component_dir.relative_to(ROOT)} lacks idf_component.yml")
            continue

    if failures:
        print("Component manifest validation failed:")
        for failure in failures:
            print(f"- {failure}")
        return 1

    print("All component manifests are present.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
