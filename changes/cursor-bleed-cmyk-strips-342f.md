# CMYK-aware bleed strip synthesis

Category: fixed
Audience: operators
Breaking-Change: no
Summary: Bleed edge strips are synthesized as RGB888 with alpha flattened onto white, and CMYK press jobs embed ICC-based DeviceCMYK image XObjects instead of DeviceRGB strips from QPainter. Undecodable output-intent ICC profile streams now fail the bleed operation instead of silently falling back to DeviceRGB strips.
