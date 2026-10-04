import struct

BASE = 0x06004000


def assemble(*words):
    """Big-endian bytes of 16-bit words, or 32-bit literals given as ("lit", value)."""
    out = b""
    for w in words:
        out += struct.pack(">I", w[1]) if isinstance(w, tuple) else struct.pack(">H", w)
    return out
