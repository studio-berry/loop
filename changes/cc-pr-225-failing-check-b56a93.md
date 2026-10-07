# Add PdfTool rollback to the product-surface CLI inventory

Category: fixed
Audience: developers
Breaking-Change: no
Summary: List the registered `rollback` command in `docs/product-surface.json` `cli.command_inventory.required_commands` so `verify-loop-surface.ps1 -Profile loop-release` no longer fails the Linux build with "PdfTool capability command is absent from manifest CLI inventory: rollback".
