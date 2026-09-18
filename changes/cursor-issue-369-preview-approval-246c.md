Category: added
Audience: developers and operators
Breaking-Change: no
Summary: Core adds a governed execution gateway that binds artifact publication to an exact
operation-plan digest, exposes separate technical and visual preview artifacts, and rejects
preflight finding waivers as operation approval. PdfTool repair reports plan_digest,
technical_preview, and visual_preview, commits through publishGovernedArtifact(), and accepts
optional --approval-file for explicit human approval.
