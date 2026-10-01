# Judge off-page and obscured content on clipped geometry (#118)

Category: fixed
Audience: prepress operators and preflight maintainers relying on off-page-content and obscured-content
Breaking-Change: no
Summary: off-page-content now intersects painted bounds with the effective clip before the page test, and obscured-content covers painted, clipped vector geometry in paint order, including coverage by several opaque objects, so clipped marks and marks hidden by combined opaque paint are no longer missed or falsely reported; a blend mode or soft mask over earlier paint reports the page incomplete. Adds the off-page-content-clipped and obscured-content-clipped regression fixtures and lands the off-page-content-clipping and obscured-content-occlusion backlog rows.
