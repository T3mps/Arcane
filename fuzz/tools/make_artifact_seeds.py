#!/usr/bin/env python3
"""Regenerate fuzz/corpus/artifact: small, VALID .arcart files built byte-for-byte
from the on-disk format in ArcaneCore/src/Arcane/Assets/ArtifactReader.hpp.

Every seed carries the harness's fixed guid and the sourceHash of EMPTY source
bytes (the FNV-1a 64 offset basis), so the reader accepts it and the fuzzer
starts from inside the section-parsing code rather than at the hash gate."""
import os
import struct
import sys

GUID_HI = 0x0123456789ABCDEF
GUID_LO = 0xFEDCBA9876543210
EMPTY_HASH = 0xCBF29CE484222325


def prefix(kind):
    return b"ARCA" + struct.pack("<IBQQQI", 1, kind, GUID_HI, GUID_LO, EMPTY_HASH, 1)


def container(header, sections):
    """sections: list of (tag, body). Offsets are absolute file offsets."""
    table_size = 4 + 20 * len(sections)
    at = len(header) + table_size
    table = struct.pack("<I", len(sections))
    bodies = b""
    for tag, body in sections:
        table += struct.pack("<IQQ", tag, at + len(bodies), len(body))
        bodies += body
    return header + table + bodies


def texture(fmt, w, h, mips, thumb):
    """mips: list of (w, h, bytes). thumb: (w, h)."""
    header = prefix(1) + struct.pack("<BBIIIIBII", fmt, 0, 1, w, h, len(mips), 1, thumb[0], thumb[1])
    payload = b""
    table = struct.pack("<I", len(mips))
    for mw, mh, data in mips:
        table += struct.pack("<QQII", len(payload), len(data), mw, mh)
        payload += data
    thumb_bytes = bytes((i * 37) & 0xFF for i in range(thumb[0] * thumb[1] * 4))
    return container(header, [(1, table), (2, payload), (3, thumb_bytes)])


def mesh(vertices, indices, sections):
    """sections: list of (name, indexOffset, indexCount, slotIndex)."""
    nslots = len(sections)
    header = prefix(2) + struct.pack("<IIIB", len(vertices), len(indices), nslots, 4)
    header += struct.pack("<3f3f", -1, -1, 0, 1, 1, 0)
    vdata = b"".join(struct.pack("<8f", *v) for v in vertices)
    idata = b"".join(struct.pack("<I", i) for i in indices)
    sdata = struct.pack("<I", len(sections))
    for name, off, cnt, slot in sections:
        n = name.encode()
        sdata += struct.pack("<H", len(n)) + n + struct.pack("<III", off, cnt, slot)
    return container(header, [(4, vdata), (5, idata), (6, sdata)])


def main(out):
    os.makedirs(out, exist_ok=True)
    rgba = lambda w, h: bytes((i * 13) & 0xFF for i in range(w * h * 4))
    bc7 = lambda w, h: bytes((i * 7) & 0xFF for i in range(((w + 3) // 4) * ((h + 3) // 4) * 16))
    quad = [(-1, -1, 0, 0, 0, 1, 0, 0), (1, -1, 0, 0, 0, 1, 1, 0),
            (1, 1, 0, 0, 0, 1, 1, 1), (-1, 1, 0, 0, 0, 1, 0, 1)]
    seeds = {
        "tex_rgba8_4x4_3mips.arcart": texture(0, 4, 4, [(4, 4, rgba(4, 4)), (2, 2, rgba(2, 2)), (1, 1, rgba(1, 1))], (4, 4)),
        "tex_bc7_8x4_npot.arcart": texture(1, 8, 4, [(8, 4, bc7(8, 4)), (4, 2, bc7(4, 2)), (2, 1, bc7(2, 1)), (1, 1, bc7(1, 1))], (8, 4)),
        "tex_rgba8_1x1_nothumb.arcart": texture(0, 1, 1, [(1, 1, rgba(1, 1))], (0, 0)),
        "mesh_quad_1section.arcart": mesh(quad, [0, 1, 2, 0, 2, 3], [("Body", 0, 6, 0)]),
        "mesh_quad_2sections.arcart": mesh(quad, [0, 1, 2, 0, 2, 3], [("A", 0, 3, 0), ("Second", 3, 3, 1)]),
        "mesh_empty.arcart": mesh([], [], []),
        "gltf_buffers.gltf": b'{"asset":{"version":"2.0"},"buffers":[{"byteLength":4},{"uri":"data:application/octet-stream;base64,AAAA"},{"uri":"missing%20file.bin"}]}',
    }
    json_chunk = b'{"buffers":[{"byteLength":0}]}  '
    glb = b"glTF" + struct.pack("<II", 2, 12 + 8 + len(json_chunk)) + struct.pack("<I", len(json_chunk)) + b"JSON" + json_chunk
    seeds["glb_minimal.glb"] = glb
    for name, data in seeds.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "corpus", "artifact"))
