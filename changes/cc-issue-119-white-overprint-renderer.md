# Judge white overprint and transparency interaction on the compositor (#119)

Category: fixed
Audience: prepress operators and preflight maintainers relying on white-overprint and transparency-risk
Breaking-Change: no
Summary: white-overprint and transparency-risk now also consult the overprint-accurate compositor that Output Preview uses. Pages that declare overprint are rasterized once per run; white-overprint reports paper-white pixels overprinted through images, shadings or transparency results that the page-view scan does not inspect, transparency-risk reports overprint composited with a non-Normal blend mode, constant alpha or a knockout group, and a page the compositor cannot judge reports check-incomplete. Adds the white-overprint-image and transparency-overprint-knockout regression fixtures and lands the white-overprint-renderer and transparency-rip-interaction backlog rows.
