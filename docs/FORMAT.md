# SCREC001 recording format

A recording directory contains `scene.screc`, `meta.json` and `textures/`.

## Container

```
file  := "SCREC001" block*
block := i32 rawLength, i32 compressedLength, byte[compressedLength]   (zlib/deflate)
```

All numbers are little-endian. Blocks are independent; a record never spans two blocks.
Each recorded frame is flushed as (at least) one block, so a recording that was cut off can
still be read up to the last complete block.

## Records

Every record starts with a `u8` type. `str` is `u16 length` + UTF-8 bytes.

| type | name | payload |
|---|---|---|
| 1 | FORMAT | `i32 id, i32 vertexSize, u8 n, n × (str name, str usage, str type, u8 count, i32 offset)` |
| 2 | MESH | `i32 id, i32 formatId, str mode, u8 transient, i32 vertexCount, i32 byteLength, bytes` |
| 3 | MESH_END | `i32 id` — a retained mesh is replaced/freed from here on |
| 4 | STATE | `i32 id, str renderType, str shader, i32 textureId, str transparency` |
| 5 | TEXTURE | `i32 id, str resourceLocation, str file, i32 w, i32 h, u8 isAtlas, str normalFile, str specularFile, str pbrMaskFile` |
| 6 | FRAME_BEGIN | `i32 frame, f64 gameTimeTicks, f64 camX, camY, camZ, f32 qx, qy, qz, qw, f32 fovYDeg, f32 aspect, u8 cameraType` |
| 7 | DRAW | `i32 meshId, i32 stateId, u8 pass, f64 tx, ty, tz, f32 m[9] (row-major), f32 r, g, b, a` |
| 8 | DRAW_SAME | `i32 meshId` — drawn again with the previous state and transform |
| 9 | FRAME_END | `i32 frame` |
| 10 | SPRITES | `i32 textureId, i32 n, n × (str name, i32 x, y, w, h, f32 u0, v0, u1, v1)` |
| 11 | END | — |
| 12 | SPRITE_EMISSION | `i32 n, n × (str spriteName, u8 lightLevel, u8 ambiguous)` |

### Notes

- Coordinates are Minecraft world space (+Y up). A drawn vertex `p` is at `T + M·p`.
- `transient = 1` marks meshes from the per-format immediate buffers; they are drawn once,
  right after their MESH record. Retained meshes (terrain sections, Sable sub-level sections)
  live until their MESH_END and are drawn many times.
- `mode` is a `VertexFormat.Mode` name; vertices are not indexed (QUADS = 4 consecutive vertices).
- `pass`: 0 = world, 1 = first-person hand.
- `cameraType`: 0 first person, 1 third person back, 2 third person front.
- For the BLOCK and NEW_ENTITY formats the last byte of each vertex (padding after the normal) is
  `0x80 | lightEmission` when the vertex was produced while tesselating a block, else 0.
- `pbrMaskFile` (atlases only) is white where a sprite has a `_s` map.
