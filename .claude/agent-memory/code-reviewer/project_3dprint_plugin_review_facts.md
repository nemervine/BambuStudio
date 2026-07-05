---
name: 3dprint-plugin-review-facts
description: Verified geometry/library semantics for reviewing D:\Development\3d-printing-plugin (bambu-mcp server) — transform conventions, pyvista camera/clip semantics, trimesh to_2D pitfall
metadata:
  type: project
---

Facts verified empirically (probe scripts, 2026-07-04) while reviewing the bambu-mcp server at `D:\Development\3d-printing-plugin\server`:

- 3MF 12-value `transform` attributes are **column-major** (4 cols x 3 rows); Bambu's 16-value part `matrix` metadata is **row-major**. Oracle: `bbs_get_transform_from_3mf_specs_string` (bbs_3mf.cpp ~570) transposes; `transform3d_from_string` (Geometry.cpp ~693) does not. Any 12-value matrix handling in writer/reader must respect this or silently corrupt geometry.
- pyvista `Renderer.view_vector(v)` places the camera at `focal_point + v` looking back at the focal point (NOT "looks along v"). pyvista `clip(normal=n, invert=True)` keeps the half-space **opposite** the normal. Getting either backwards makes cutaways show the exterior — found this exact bug in render/sections.py in the 0cac629 review.
- trimesh `section(...).to_2D()` with no explicit transform re-centers (and can rotate/mirror) the section — output coords are NOT model X/Y. Use `to_planar(to_2D=trimesh.geometry.plane_transform(origin, normal))` for model-frame slices.
- The surgical ZIP copier in bambu3mf/writer.py correctly handles data-descriptor (flag 0x08) source archives — probe-verified byte-identical untouched entries; zip64 members would fail loudly via `FileHeader(False)` raising LargeZipFile.

**Why:** these took probe scripts to verify and are easy to get backwards in future reviews of this repo.
**How to apply:** when reviewing transform math, rendering, or the 3MF writer in the plugin repo, check against these conventions first. Test baseline: `cd D:\Development\3d-printing-plugin\server; uv run pytest -q` (111 passed as of 0cac629).
