# Close the L01 ICCBased color-mode and invisible-content gaps (#113, #117)

Category: fixed
Audience: prepress operators and preflight maintainers relying on color-mode and invisible-content
Breaking-Change: no
Summary: color-mode no longer classifies every ICCBased space as DeviceRGB. The embedded profile header decides the painted space, and a profile that is unreadable, not a gray, RGB or CMYK device profile, or that disagrees with the space's /N or /Alternate is reported as its own disallowed space instead of passing on the alternate. invisible-content now also reports text shown at font size 0, paint under a zero-area clipping path, and paint that falls entirely outside the active clip. Adds the color-icc-cmyk-profile, color-icc-alternate-conflict and invisible-content-* regression fixtures and lands the color-mode-icc-alternate and invisible-content-breadth backlog rows. Zero horizontal scaling, entirely empty clip paths and fully transparent soft masks are still not detected and are recorded as limitations.
