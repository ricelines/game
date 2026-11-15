"""Small embedded-buffer glTF writer for already measured triangle geometry."""

import json
import struct
from pathlib import Path

import numpy as np


def write_glb(path: Path, vertices: list, indices: list, *, flat: bool = False) -> int:
    if not indices:
        document = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": []}]}
        binary = b""
    else:
        raw = np.asarray(vertices, dtype=np.float32)
        if flat:
            points = raw[indices][:, [0, 2, 1]].copy()
            compact = np.arange(len(indices), dtype=np.uint32)
        else:
            used, compact = np.unique(np.asarray(indices, dtype=np.uint32), return_inverse=True)
            points = raw[used][:, [0, 2, 1]].copy()
        points[:, 2] *= -1
        triangles = compact.reshape(-1, 3)
        normals = np.zeros_like(points)
        face_normals = np.cross(
            points[triangles[:, 1]] - points[triangles[:, 0]],
            points[triangles[:, 2]] - points[triangles[:, 0]],
        )
        for corner in range(3):
            np.add.at(normals, triangles[:, corner], face_normals)
        lengths = np.linalg.norm(normals, axis=1)
        normals /= np.maximum(lengths[:, None], 1e-20)
        position_bytes = points.astype("<f4").tobytes()
        normal_bytes = normals.astype("<f4").tobytes()
        index_bytes = compact.astype("<u4").tobytes()
        binary = position_bytes + normal_bytes + index_bytes
        document = {
            "asset": {"version": "2.0", "generator": "Ambergris bake"},
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": [{"name": "geometry", "mesh": 0}],
            "meshes": [
                {
                    "primitives": [
                        {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}
                    ]
                }
            ],
            "materials": [
                {
                    "name": "surface",
                    "pbrMetallicRoughness": {
                        "baseColorFactor": [1, 1, 1, 1],
                        "metallicFactor": 0,
                        "roughnessFactor": 0.65,
                    },
                }
            ],
            "buffers": [{"byteLength": len(binary)}],
            "bufferViews": [
                {"buffer": 0, "byteOffset": 0, "byteLength": len(position_bytes), "target": 34962},
                {
                    "buffer": 0,
                    "byteOffset": len(position_bytes),
                    "byteLength": len(normal_bytes),
                    "target": 34962,
                },
                {
                    "buffer": 0,
                    "byteOffset": len(position_bytes) + len(normal_bytes),
                    "byteLength": len(index_bytes),
                    "target": 34963,
                },
            ],
            "accessors": [
                {
                    "bufferView": 0,
                    "componentType": 5126,
                    "count": len(points),
                    "type": "VEC3",
                    "min": points.min(axis=0).tolist(),
                    "max": points.max(axis=0).tolist(),
                },
                {"bufferView": 1, "componentType": 5126, "count": len(points), "type": "VEC3"},
                {"bufferView": 2, "componentType": 5125, "count": len(compact), "type": "SCALAR"},
            ],
        }
    encoded = json.dumps(document, separators=(",", ":"), allow_nan=False).encode()
    encoded += b" " * (-len(encoded) % 4)
    chunks = struct.pack("<I4s", len(encoded), b"JSON") + encoded
    if binary:
        chunks += struct.pack("<I4s", len(binary), b"BIN\0") + binary
    path.write_bytes(struct.pack("<4sII", b"glTF", 2, 12 + len(chunks)) + chunks)
    return len(indices) // 3
