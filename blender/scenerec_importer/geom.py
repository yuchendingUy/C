"""Vertex decoding and coordinate conversion (numpy only, no bpy)."""
import numpy as np

# Minecraft world: +X east, +Y up, +Z south.  Blender: +Z up.
# (x, y, z)_mc -> (x, -z, y)_blender is a proper rotation (det = +1), so winding is preserved.
C = np.array([[1.0, 0.0, 0.0],
              [0.0, 0.0, -1.0],
              [0.0, 1.0, 0.0]])

_TYPES = {
    "FLOAT": ("<f4", 4),
    "UBYTE": ("u1", 1),
    "BYTE": ("i1", 1),
    "USHORT": ("<u2", 2),
    "SHORT": ("<i2", 2),
    "UINT": ("<u4", 4),
    "INT": ("<i4", 4),
}


def to_blender(v):
    """(N,3) Minecraft-space vectors -> Blender space."""
    out = np.empty_like(v)
    out[:, 0] = v[:, 0]
    out[:, 1] = -v[:, 2]
    out[:, 2] = v[:, 1]
    return out


class Format:
    def __init__(self, rec):
        self.id = rec["id"]
        self.vertex_size = rec["vertex_size"]
        self.elements = {name: (usage, typ, count, off) for name, usage, typ, count, off in rec["elements"]}
        end = 0
        for _, (usage, typ, count, off) in self.elements.items():
            end = max(end, off + _TYPES[typ][1] * count)
        # BLOCK and NEW_ENTITY end with one padding byte after the normal; the mod stores the
        # emission tag there (see BufferBuilderMixin).
        self.tag_offset = self.vertex_size - 1 if ("Normal" in self.elements and end == self.vertex_size - 1) else None

    def _field(self, raw, name, n):
        e = self.elements.get(name)
        if e is None:
            return None
        usage, typ, count, off = e
        dt, size = _TYPES[typ]
        cols = raw[:, off:off + size * count]
        return np.ascontiguousarray(cols).view(dt).reshape(n, count)

    def decode(self, data, vertex_count):
        """Returns dict with positions (N,3 f8), colors (N,4 f4 0..1), uv (N,2 f4), light (N,2 i) and tag (N, u1)."""
        n = min(vertex_count, len(data) // self.vertex_size)
        raw = np.frombuffer(data, dtype=np.uint8, count=n * self.vertex_size).reshape(n, self.vertex_size)
        out = {"n": n}
        pos = self._field(raw, "Position", n)
        out["positions"] = pos.astype(np.float64) if pos is not None else np.zeros((n, 3))
        col = self._field(raw, "Color", n)
        if col is not None:
            out["colors"] = col.astype(np.float32) / 255.0
        else:
            out["colors"] = np.ones((n, 4), dtype=np.float32)
        uv = self._field(raw, "UV0", n)
        out["uv"] = uv.astype(np.float32) if uv is not None else None
        light = self._field(raw, "UV2", n)
        out["light"] = light.astype(np.int32) if light is not None else None
        out["tag"] = raw[:, self.tag_offset].copy() if self.tag_offset is not None else None
        return out


def faces_for_mode(mode, n):
    """Returns (counts, indices) for a non-indexed vertex stream drawn with the given mode."""
    if mode == "QUADS":
        q = n // 4
        return np.full(q, 4, dtype=np.int32), np.arange(q * 4, dtype=np.int32)
    if mode == "TRIANGLES":
        t = n // 3
        return np.full(t, 3, dtype=np.int32), np.arange(t * 3, dtype=np.int32)
    if mode == "TRIANGLE_STRIP":
        if n < 3:
            return np.zeros(0, np.int32), np.zeros(0, np.int32)
        i = np.arange(n - 2)
        a = np.where(i % 2 == 0, i, i + 1)
        b = np.where(i % 2 == 0, i + 1, i)
        idx = np.stack([a, b, i + 2], axis=1).reshape(-1).astype(np.int32)
        return np.full(n - 2, 3, dtype=np.int32), idx
    if mode == "TRIANGLE_FAN":
        if n < 3:
            return np.zeros(0, np.int32), np.zeros(0, np.int32)
        i = np.arange(1, n - 1)
        idx = np.stack([np.zeros_like(i), i, i + 1], axis=1).reshape(-1).astype(np.int32)
        return np.full(n - 2, 3, dtype=np.int32), idx
    return np.zeros(0, np.int32), np.zeros(0, np.int32)


def transform_points(pos, matrix9, translation, origin):
    """world = T + M p (Minecraft space, float64), relative to origin, converted to Blender space."""
    m = np.asarray(matrix9, dtype=np.float64).reshape(3, 3)
    w = pos @ m.T
    w += np.asarray(translation, dtype=np.float64) - origin
    return to_blender(w)


# Overlay quads (grass block sides, some modded models) sit exactly on top of the quad below them.
# A rasteriser draws both in order; a path tracer continues a ray through a transparent hit with
# tmin = nextafter(t) (Cycles: integrator/shade_surface.h), so the quad underneath at the very same
# distance is skipped and shows as a hole. Push every later duplicate out along its normal.
COPLANAR_STEP = 0.002


def separate_coplanar(verts, counts, idx):
    """Offsets same-facing quads that share all four corners (in place, Blender-space verts)."""
    if len(counts) == 0 or not np.all(counts == 4):
        return verts
    q = verts[idx].reshape(-1, 4, 3)
    nrm = np.cross(q[:, 1] - q[:, 0], q[:, 2] - q[:, 0])
    ln = np.linalg.norm(nrm, axis=1)
    ok = ln > 1e-12
    nrm[ok] /= ln[ok, None]
    key = np.concatenate([np.round(np.sort(q.reshape(-1, 12), axis=1), 4), np.round(nrm, 2)], axis=1)
    _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    inv = inv.reshape(-1)
    dup = cnt[inv] > 1
    if not dup.any():
        return verts
    order = np.argsort(inv, kind="stable")          # quads grouped, in draw order within a group
    sorted_inv = inv[order]
    first = np.searchsorted(sorted_inv, sorted_inv, side="left")
    rank = np.empty(len(inv), dtype=np.int64)
    rank[order] = np.arange(len(inv)) - first      # 0 for the first quad of a group, 1, 2, ...
    off = (rank * COPLANAR_STEP)[:, None] * nrm
    off[~dup | ~ok] = 0.0
    verts[idx] += np.repeat(off, 4, axis=0)
    return verts


class SpriteLookup:
    """Maps atlas UVs to sprite indices with a per-pixel index image."""

    def __init__(self, width, height, sprites):
        self.w = width
        self.h = height
        self.names = [s[0] for s in sprites]
        self.index = np.full((height, width), -1, dtype=np.int32)
        for i, (name, x, y, w, h, u0, v0, u1, v1) in enumerate(sprites):
            self.index[max(0, y):min(height, y + h), max(0, x):min(width, x + w)] = i

    def sprite_of(self, uv):
        px = np.clip((uv[:, 0] * self.w).astype(np.int64), 0, self.w - 1)
        py = np.clip((uv[:, 1] * self.h).astype(np.int64), 0, self.h - 1)
        return self.index[py, px]


# Render types whose geometry is fully emissive (names from RenderType.create in 1.21.1).
EMISSIVE_RENDER_TYPES = {
    "eyes", "energy_swirl", "beacon_beam", "end_portal", "end_gateway", "lightning",
    "entity_translucent_emissive", "dragon_rays", "dragon_rays_depth",
}


def vertex_emission(dec, mode, state, sprite_lookup, sprite_emission):
    """Per-vertex emission 0..1 (block light level / 15)."""
    n = dec["n"]
    em = np.zeros(n, dtype=np.float32)
    rt = state["render_type"]
    if rt in EMISSIVE_RENDER_TYPES:
        em[:] = 1.0
        return em
    tag = dec["tag"]
    if tag is not None:
        tagged = (tag & 0x80) != 0
        em[tagged] = (tag[tagged] & 15) / 15.0
    else:
        tagged = np.zeros(n, dtype=bool)
    untagged = ~tagged
    if not untagged.any():
        return em
    uv = dec["uv"]
    # Block geometry baked by mods and re-emitted every frame carries no tag: use the
    # sprite -> emission table built from the block registry.
    if sprite_lookup is not None and sprite_emission and uv is not None:
        counts, idx = faces_for_mode(mode, n)
        if len(counts) and np.all(counts == counts[0]):
            k = int(counts[0])
            face_uv = uv[idx].reshape(-1, k, 2).mean(axis=1)
            sid = sprite_lookup.sprite_of(face_uv)
            levels = np.array([sprite_emission.get(nm, 0.0) for nm in sprite_lookup.names] + [0.0], dtype=np.float32)
            face_em = levels[sid]  # sid == -1 -> last element (0)
            vert_em = np.repeat(face_em, k)
            sel = untagged[idx]
            em[idx[sel]] = np.maximum(em[idx[sel]], vert_em[sel])
    # Particles drawn full-bright (block light 15, e.g. flames, lava drips) glow.
    if state["shader"] == "particle" and dec["light"] is not None:
        bright = untagged & (dec["light"][:, 0] >= 240)
        em[bright] = 1.0
    return em
