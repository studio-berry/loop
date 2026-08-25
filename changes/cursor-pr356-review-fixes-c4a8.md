Category: fixed
Audience: developers
Breaking-Change: no
Summary: Address PR #356 review findings and complete the Quick shell structural cleanup by extracting DocumentViewSession, table-driving viewport commands, caching page-box geometry per revision, indexing the page-surface cache, and binding QML menu state to catalog descriptors. Extend command-catalog verification for EditorHost shell handlers, classify actionQuit as an application capability, and fix canvas forget() to avoid deleting scene-graph-owned nodes on window change.
