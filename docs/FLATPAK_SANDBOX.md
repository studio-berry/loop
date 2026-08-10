# Flatpak sandbox policy

Loupe's Flatpak manifest intentionally does not request `--filesystem=host`.
The shipped policy grants `--filesystem=home`, which covers ordinary work in
the operator's home directory while keeping system paths, other users, and
unrelated mounted volumes outside the app sandbox. Speech-dispatcher access
remains limited to its two named runtime/cache paths.

## Access outside home

For a press share or hot folder mounted outside the home directory, grant only
the named path that the operator needs:

```text
flatpak override --user --filesystem=/mnt/press:rw io.github.mberrys.Loupe-pdf
```

Use the actual mounted path; do not restore `--filesystem=host`. To inspect the
effective permissions:

```text
flatpak override --user --show io.github.mberrys.Loupe-pdf
```

An operator can remove the override again with:

```text
flatpak override --user --nofilesystem=/mnt/press io.github.mberrys.Loupe-pdf
```

## Workflow expectations

`QFileDialog` is portal-aware under Flatpak when an xdg-desktop-portal backend
is installed. Open and Save As therefore use a user-selected file without
requiring host-wide access. New outputs are written to the selected output
path; the source PDF is not modified implicitly.

The following cases are the release smoke matrix:

| Workflow | Expected result under `--filesystem=home` | Evidence required |
|---|---|---|
| Open and Save As inside home | Works | Flatpak smoke run |
| Open a file outside home through the portal | Works without a manifest path grant | Select a file outside home and reopen it from Recent |
| PageMaster batch export | Works for a home directory or an explicitly granted named path | Export multiple outputs and `.loupe-batch.json` |
| Preflight report export | Works for a home directory or an explicitly granted named path | Export JSON/HTML/XML report |
| Diagnostics bundle | Works in the portal-selected destination; rotating logs stay in app-local data | Collect a bundle and inspect its manifest |
| OCR sidecar | Only applicable when an operator supplies a compatible sidecar; the V1 Flatpak does not bundle the OCR UI or sidecar | Run `PdfTool ocr` with an explicit sidecar and verify its temp/output paths |

If a portal-selected file cannot be reached after the grant expires, Loupe
reports the read failure and removes the unreachable Recent entry. It does not
silently retry with broader filesystem access.

## Portal-only evaluation

Portal-only permissions (`finish-args` with no `--filesystem` entry) are the
long-term target, but are not enabled by this change. Ordinary file dialogs
are already the right integration point; the unresolved case is resumed batch
work. PageMaster persists native directory paths in its workspace/export
settings, while a document-portal grant needs a persisted document identifier
and a revalidation step on the next session. The current code does not retain
that identifier, so a portal-only build would make resumed batch exports
unverified and potentially inaccessible.

The next portal-only validation should run the matrix above in a clean Flatpak
environment with `--filesystem=home` removed, including a batch resume after
logout/restart. Record which workflows pass and open a follow-up issue if the
document-portal path can be made durable. Until that evidence exists, keep the
bounded `home` policy and use named-path overrides for production volumes.
