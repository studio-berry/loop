# Blacksmith CI migration

Category: internal
Audience: developers
Breaking-Change: no
Summary: Run selected GitHub Actions workflows on Blacksmith runners while keeping Windows_MSI and Linux_AppImage as workflow_dispatch-only exact-SHA qualification, and keeping full linux/windows CI on the stable release-candidate path instead of every push to `dev`.
