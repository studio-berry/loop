# Blacksmith CI migration

Category: internal
Audience: developers
Breaking-Change: no
Summary: Run Windows_MSI on Blacksmith to shorten the ~50-minute packaging gate while keeping every other workflow on GitHub-hosted runners, preserving workflow_dispatch-only exact-SHA MSI/AppImage qualification, and keeping full linux/windows CI on the stable release-candidate path instead of every push to `dev`.
