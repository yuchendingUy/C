"""Reader for SCREC001 recordings written by the Scene Recorder mod.

Pure Python + struct/zlib so it runs both inside Blender and standalone.
The record layout mirrors mod/src/main/java/dev/scenerec/io/Rec.java and docs/FORMAT.md.
"""
import struct
import zlib

MAGIC = b"SCREC001"

FORMAT, MESH, MESH_END, STATE, TEXTURE, FRAME_BEGIN, DRAW, DRAW_SAME, FRAME_END, SPRITES, END, SPRITE_EMISSION = range(1, 13)

PASS_WORLD = 0
PASS_HAND = 1


class Reader:
    def __init__(self, data: bytes, pos: int = 0):
        self.d = data
        self.p = pos

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def f32(self):
        v = struct.unpack_from("<f", self.d, self.p)[0]
        self.p += 4
        return v

    def f64(self):
        v = struct.unpack_from("<d", self.d, self.p)[0]
        self.p += 8
        return v

    def floats(self, n):
        v = struct.unpack_from("<%df" % n, self.d, self.p)
        self.p += 4 * n
        return v

    def str(self):
        n = struct.unpack_from("<H", self.d, self.p)[0]
        self.p += 2
        s = self.d[self.p:self.p + n].decode("utf-8")
        self.p += n
        return s

    def raw(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v


def iter_blocks(path):
    """Yields the decompressed bytes of each block."""
    with open(path, "rb") as f:
        if f.read(8) != MAGIC:
            raise ValueError("not a SCREC001 file: %s" % path)
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                return
            raw_len, comp_len = struct.unpack("<ii", hdr)
            comp = f.read(comp_len)
            if len(comp) < comp_len:
                return  # truncated (recording interrupted); keep what we have
            data = zlib.decompress(comp)
            if len(data) != raw_len:
                raise ValueError("corrupt block")
            yield data


def iter_records(path):
    """Yields (type, payload-dict) for every record in the file.

    Records never span blocks (the mod flushes whole frames), so each block is parsed on its own.
    """
    for block in iter_blocks(path):
        r = Reader(block)
        n = len(block)
        while r.p < n:
            t = r.u8()
            if t == FORMAT:
                fid = r.i32()
                vsize = r.i32()
                elems = []
                for _ in range(r.u8()):
                    name = r.str()
                    usage = r.str()
                    typ = r.str()
                    count = r.u8()
                    off = r.i32()
                    elems.append((name, usage, typ, count, off))
                yield t, {"id": fid, "vertex_size": vsize, "elements": elems}
            elif t == MESH:
                mid = r.i32()
                fid = r.i32()
                mode = r.str()
                transient = r.u8() != 0
                vcount = r.i32()
                blen = r.i32()
                data = r.raw(blen)
                yield t, {"id": mid, "format": fid, "mode": mode, "transient": transient, "vertex_count": vcount, "data": data}
            elif t == MESH_END:
                yield t, {"id": r.i32()}
            elif t == STATE:
                sid = r.i32()
                rt = r.str()
                shader = r.str()
                tex = r.i32()
                transp = r.str()
                yield t, {"id": sid, "render_type": rt, "shader": shader, "texture": tex, "transparency": transp}
            elif t == TEXTURE:
                tid = r.i32()
                name = r.str()
                color = r.str()
                w = r.i32()
                h = r.i32()
                atlas = r.u8() != 0
                normal = r.str()
                spec = r.str()
                mask = r.str()
                yield t, {"id": tid, "name": name, "file": color, "width": w, "height": h, "atlas": atlas,
                          "normal": normal, "specular": spec, "pbr_mask": mask}
            elif t == FRAME_BEGIN:
                fi = r.i32()
                gt = r.f64()
                pos = (r.f64(), r.f64(), r.f64())
                q = r.floats(4)
                fov = r.f32()
                aspect = r.f32()
                ctype = r.u8()
                yield t, {"frame": fi, "game_time": gt, "pos": pos, "rot": q, "fov": fov, "aspect": aspect, "camera_type": ctype}
            elif t == DRAW:
                mid = r.i32()
                sid = r.i32()
                ps = r.u8()
                trans = (r.f64(), r.f64(), r.f64())
                m = r.floats(9)
                col = r.floats(4)
                yield t, {"mesh": mid, "state": sid, "pass": ps, "translation": trans, "matrix": m, "color": col}
            elif t == DRAW_SAME:
                yield t, {"mesh": r.i32()}
            elif t == FRAME_END:
                yield t, {"frame": r.i32()}
            elif t == SPRITES:
                tid = r.i32()
                sprites = []
                for _ in range(r.i32()):
                    name = r.str()
                    x, y, w, h = r.i32(), r.i32(), r.i32(), r.i32()
                    u0, v0, u1, v1 = r.floats(4)
                    sprites.append((name, x, y, w, h, u0, v0, u1, v1))
                yield t, {"texture": tid, "sprites": sprites}
            elif t == SPRITE_EMISSION:
                entries = []
                for _ in range(r.i32()):
                    name = r.str()
                    em = r.u8()
                    amb = r.u8() != 0
                    entries.append((name, em, amb))
                yield t, {"entries": entries}
            elif t == END:
                yield t, {}
            else:
                raise ValueError("unknown record type %d at offset %d" % (t, r.p - 1))
