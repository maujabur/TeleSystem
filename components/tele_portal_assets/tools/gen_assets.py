#!/usr/bin/env python3

import argparse
import re
from pathlib import Path

# TODO: fix probable conflict with same name files in different directories


def symbol_name(file_path: Path) -> str:
    name = file_path.name
    s = re.sub(r"[^A-Za-z0-9_]", "_", name)
    return f"_binary_{s}"


def uri_for_file(input_dir: Path, file_path: Path) -> str:
    rel = file_path.relative_to(input_dir).as_posix()

    if rel == "index.html":
        return "/"

    return "/" + rel


def mime_for_file(file_path: Path) -> str:
    suffix = file_path.suffix.lower()

    return {
        ".html": "text/html; charset=utf-8",
        ".css": "text/css; charset=utf-8",
        ".js": "application/javascript; charset=utf-8",
        ".json": "application/json; charset=utf-8",
        ".svg": "image/svg+xml",
        ".png": "image/png",
        ".jpg": "image/jpeg",
        ".jpeg": "image/jpeg",
        ".ico": "image/x-icon",
        ".txt": "text/plain; charset=utf-8",
    }.get(suffix, "application/octet-stream")


def c_string(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--out-c", required=True)
    parser.add_argument("--out-h", required=True)
    parser.add_argument("files", nargs="+")

    args = parser.parse_args()

    input_dir = Path(args.input_dir).resolve()
    out_c = Path(args.out_c)
    out_h = Path(args.out_h)

    files = sorted(
        Path(f).resolve()
        for f in args.files
        if Path(f).is_file()
    )

    externs = []
    entries = []

    for file_path in files:
        base_sym = symbol_name(file_path)

        start_sym = f"{base_sym}_start"
        end_sym = f"{base_sym}_end"

        uri = uri_for_file(input_dir, file_path)
        mime = mime_for_file(file_path)

        requires_logs_enabled = "true" if file_path.name == "logs.html" else "false"

        externs.append(f"extern const unsigned char {start_sym}[];")
        externs.append(f"extern const unsigned char {end_sym}[];")

        entries.append(
            "    {\n"
            f'        .uri = "{c_string(uri)}",\n'
            f'        .content_type = "{c_string(mime)}",\n'
            f"        .start = {start_sym},\n"
            f"        .end = {end_sym},\n"
            f"        .requires_logs_enabled = {requires_logs_enabled},\n"
            "    }"
        )

    out_h.write_text(
        """#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *uri;
    const char *content_type;
    const unsigned char *start;
    const unsigned char *end;
    bool requires_logs_enabled;
} tele_portal_asset_t;

extern const tele_portal_asset_t tele_portal_assets[];
extern const size_t tele_portal_assets_count;
""",
        encoding="utf-8",
    )

    out_c.write_text(
        """#include "tele_portal_assets_generated.h"

"""
        + "\n".join(externs)
        + """

const tele_portal_asset_t tele_portal_assets[] = {
"""
        + ",\n".join(entries)
        + """
};

const size_t tele_portal_assets_count =
    sizeof(tele_portal_assets) / sizeof(tele_portal_assets[0]);
""",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()