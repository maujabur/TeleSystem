# Update Artifact Manifests

`generate_manifest.py` creates schema-1 manifests consumed by `tele_manifest`,
`tele_ca_updater`, and firmware OTA by manifest.

Firmware example:

```bash
python tools/update_artifacts/generate_manifest.py \
  --artifact-type firmware \
  --channel pilot \
  --version 0.6.10 \
  --build-id 2026-06-24T12:00:00Z-0.6.10 \
  --url https://updates.example.com/telesystem/pilot/TeleSystem.bin \
  --file build/TeleSystem.bin \
  --out build/TeleSystem.manifest.json
```

CA bundle example:

```bash
python tools/update_artifacts/generate_manifest.py \
  --artifact-type ca_bundle \
  --channel stable \
  --version 2026.06.24 \
  --build-id 2026-06-24T12:00:00Z-ca \
  --url https://updates.example.com/ca/stable/bundle_ca.bin \
  --file artifacts/ca/stable/bundle_ca.bin \
  --out artifacts/ca/stable/bundle_ca.manifest.json
```

The firmware repository should not become the artifact repository. Publish
`.bin`, CA bundles, and generated manifests to a release bucket, CDN, GitHub
Release, or a separate artifact repository. Local `artifacts/` output is ignored
by git.

Use `--mirror-url` to add extra HTTPS artifact mirrors. Use `--critical`,
`--min-version`, and `--notes` when publishing operator-managed rollout
metadata.
