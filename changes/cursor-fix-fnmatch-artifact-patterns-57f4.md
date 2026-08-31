Category: fixed
Audience: developers
Breaking-Change: no
Summary: Replace recursive glob patterns in product-surface ignored_artifact_paths with fnmatch-compatible directory prefixes so nested Qt deploy artifacts such as qml/QtQuick/... are correctly excluded from first-party install validation.
