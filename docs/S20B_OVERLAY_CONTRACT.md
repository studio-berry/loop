# S20b / #143 overlay contract

Status: internal Widgets contract for transient canvas geometry.

The overlay pass is independent from cached PDF page surfaces. It owns no PDF
content and must not mutate the document, page revision, or page-surface cache.

## Provider and frame contract

An overlay provider opts in through `IDocumentOverlayInterface` while retaining
the existing `IDocumentDrawInterface` registration. The proxy builds an
immutable `PDFOverlayContext` for each visible page using the same page-to-device
matrix used by page rendering. The context contains the page index, page
geometry, viewport, transform, and a renderability flag.

Providers must treat the context as read-only and must tolerate a non-renderable
or empty geometry state. The proxy clips the painter to the intersection of the
page and viewport before invoking a provider. Off-page geometry is therefore
not painted outside the visible page; invalid geometry is skipped by the
provider without blocking the frame.

## Ordering and invalidation

Providers are ordered by `PDFOverlayLayer`, then by registration sequence:

1. page chrome;
2. guides and measurements;
3. preflight findings;
4. selection;
5. drag handles;
6. tool previews.

The dedicated overlay pass runs after page surfaces and before legacy
post-rendering callbacks. A state-only overlay update requests a widget repaint
but does not invalidate or re-render the PDF page surface. Existing
`drawPage()` and `drawPostRendering()` providers remain compatibility paths.

Overlay timing is recorded in the existing `overlays` interaction-trace stage.
Trace output contains timing and aggregate cache data only; it does not contain
finding payloads, object identifiers, revision identities, or customer geometry.

## Preflight provider

Preflight findings retain the report's stable finding id and optional object id.
The preflight plugin draws markers through the independent overlay pass and
uses the existing selected-row state for focus highlighting. Report-dock
navigation and the closed #89 scope are unchanged.
