Category: fixed
Audience: developers
Breaking-Change: no
Summary: Release Gate Linux configures with `LOUPE_LOUPE_DISTRIBUTION=ON` but was verifying the developer product-surface profile, which expects a second LoupeEditor desktop entry that distribution installs omit. Linux CI now checks the `loupe-release` profile instead.
