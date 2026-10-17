import pytest
import struct

BASE = 0x06004000


def assemble(*words):
    """Big-endian bytes of 16-bit words, or 32-bit literals given as ("lit", value)."""
    out = b""
    for w in words:
        out += struct.pack(">I", w[1]) if isinstance(w, tuple) else struct.pack(">H", w)
    return out


@pytest.fixture(scope="session")
def m68k_lib(tmp_path_factory):
    from saturnrecomp import m68k
    return m68k.library(str(tmp_path_factory.mktemp("m68kdasm")))


def sound_program(*words, vectors=()):
    """A 68000 program at 0: the vector table, with the reset PC at 0x400 and (offset, value) longs from
    `vectors`, then `words` from 0x400."""
    table = bytearray(0x400)
    table[4:8] = struct.pack(">I", 0x400)
    for at, v in vectors:
        table[at:at + 4] = struct.pack(">I", v)
    return bytes(table) + assemble(*words)
