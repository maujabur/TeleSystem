#!/usr/bin/env python3
"""Generate TeleSystem update manifests for firmware and CA bundle artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from urllib.parse import urlparse


ARTIFACT_TYPES = ("firmware", "ca_bundle")


def https_url(value: str) -> str:
    parsed = urlparse(value)
    if parsed.scheme != "https" or not parsed.netloc:
        raise argparse.ArgumentTypeError("URL must be an absolute https:// URL")
    return value


def non_empty(value: str) -> str:
    if not value:
        raise argparse.ArgumentTypeError("value must not be empty")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as artifact:
        for chunk in iter(lambda: artifact.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a schema-1 TeleSystem update manifest."
    )
    parser.add_argument("--artifact-type", required=True, choices=ARTIFACT_TYPES)
    parser.add_argument("--channel", required=True, type=non_empty)
    parser.add_argument("--version", required=True, type=non_empty)
    parser.add_argument("--build-id", required=True, type=non_empty)
    parser.add_argument("--url", required=True, type=https_url)
    parser.add_argument(
        "--mirror-url",
        action="append",
        default=[],
        type=https_url,
        help="Optional extra artifact mirror URL. Can be repeated.",
    )
    parser.add_argument("--file", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--min-version", default="", help="Optional minimum current firmware version.")
    parser.add_argument("--critical", action="store_true", help="Mark this update as critical.")
    parser.add_argument("--notes", default="", help="Optional operator-facing notes.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    artifact_path = args.file
    if not artifact_path.is_file():
        raise SystemExit(f"artifact file not found: {artifact_path}")

    urls = [args.url, *args.mirror_url]
    manifest: dict[str, object] = {
        "schema": 1,
        "artifact_type": args.artifact_type,
        "channel": args.channel,
        "version": args.version,
        "build_id": args.build_id,
        "url": args.url,
        "sha256": sha256_file(artifact_path),
        "size": artifact_path.stat().st_size,
        "critical": bool(args.critical),
    }

    if len(urls) > 1:
        manifest["urls"] = urls
    if args.min_version:
        manifest["min_version"] = args.min_version
    if args.notes:
        manifest["notes"] = args.notes

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
