# Loop diagnostics and support bundles

Loop writes local diagnostics through one shared Core sink used by Editor and
PdfTool. The sink is privacy-scrubbed before every persisted line, rotates at
2 MiB, and retains at most the active log plus `.1` and `.2` backups. The
default level is `Warning`; `LOOP_LOG_LEVEL` or the `diagnostics/logLevel`
setting can select `Off`, `Error`, `Warning`, `Info`, or `Debug`.

The log directory is resolved in this order:

1. `LOOP_LOG_DIR`, when set;
2. `<settings-path>/logs` for an Editor portable/`--config` installation;
3. Qt's writable application-local data directory plus `logs`, with the
   system temporary directory as a final fallback.

## Creating a bundle

From PdfTool:

```text
PdfTool diagnostics --output <parent-directory>
```

From Editor, choose Help → Collect Diagnostics…, review the privacy notice,
choose a parent directory, and inspect the resulting directory before sharing.
The workflow works with no PDF open.

Bundle publication is staged and then renamed into place. An existing
timestamped destination is rejected; failures remove the `.partial` staging
directory and never leave a bundle that looks complete.

## Default contents and privacy boundary

The bundle contains a versioned `manifest.json`, non-secret runtime/build
metadata, optional loaded-plugin metadata, scrubbed current/rotated logs, and a
README. The manifest stores bundle-relative file names, sizes, and SHA-256
hashes only.

The default bundle excludes PDFs, document bytes/object streams, extracted
text, thumbnails/screenshots, recent files, arbitrary settings, environment
variables, command-line arguments, secrets/tokens, absolute paths, and crash
minidumps. Sentry is represented only as an enabled/disabled boolean.

Scrubbing removes home/temp directories, user and host tokens, Windows/POSIX/UNC
paths, email addresses, and IPv4/IPv6 literals. Scrubbing is defense in depth,
not permission to intentionally log passwords, authorization headers, DSNs, or
other secrets.
