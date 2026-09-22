# Charge image decode pixels before raster allocation

Category: fixed
Audience: security
Breaking-Change: no
Summary: Untrusted PDF image XObjects now validate sample-byte length and reserve render-pixel budget before QImage or codec output buffers are allocated, closing the image-decode budget bypass.
