"""Minimal readers for the FromSoftware file formats needed to build the item list.

Supports: encrypted regulation.bin (AES-256-CBC), DCX (DFLT / ZSTD / KRAK),
BND4 containers, PARAM tables (64-bit layout) and FMG text files (version 2).
Only uses the Python standard library plus Windows' own bcrypt.dll and the
game's oo2core_6_win64.dll for Oodle Kraken decompression.
"""
import ctypes
import struct
import zlib
from ctypes import wintypes

# Elden Ring regulation.bin AES-256 key (public, same one SoulsFormats/Smithbox use).
ER_REGULATION_KEY = bytes.fromhex(
    "99BFFC366A6BC8C6F5827D093602D676C42892A01C207FB024D3AF4E493FEF99")


def aes256_cbc_decrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
    bcrypt = ctypes.WinDLL("bcrypt")
    alg = ctypes.c_void_p()
    status = bcrypt.BCryptOpenAlgorithmProvider(ctypes.byref(alg), ctypes.c_wchar_p("AES"), None, 0)
    if status != 0:
        raise OSError(f"BCryptOpenAlgorithmProvider failed: {status:#x}")
    try:
        mode = ctypes.create_unicode_buffer("ChainingModeCBC")
        status = bcrypt.BCryptSetProperty(alg, ctypes.c_wchar_p("ChainingMode"), mode,
                                          ctypes.sizeof(mode), 0)
        if status != 0:
            raise OSError(f"BCryptSetProperty failed: {status:#x}")
        hkey = ctypes.c_void_p()
        status = bcrypt.BCryptGenerateSymmetricKey(alg, ctypes.byref(hkey), None, 0,
                                                   key, len(key), 0)
        if status != 0:
            raise OSError(f"BCryptGenerateSymmetricKey failed: {status:#x}")
        try:
            iv_buf = ctypes.create_string_buffer(iv, len(iv))
            out = ctypes.create_string_buffer(len(data))
            written = wintypes.ULONG()
            status = bcrypt.BCryptDecrypt(hkey, data, len(data), None, iv_buf, len(iv),
                                          out, len(data), ctypes.byref(written), 0)
            if status != 0:
                raise OSError(f"BCryptDecrypt failed: {status:#x}")
            return out.raw[:written.value]
        finally:
            bcrypt.BCryptDestroyKey(hkey)
    finally:
        bcrypt.BCryptCloseAlgorithmProvider(alg, 0)


def decrypt_regulation(raw: bytes) -> bytes:
    return aes256_cbc_decrypt(ER_REGULATION_KEY, raw[:16], raw[16:])


_oodle = None


def _oodle_decompress(comp: bytes, raw_size: int, oodle_dll: str) -> bytes:
    global _oodle
    if _oodle is None:
        _oodle = ctypes.WinDLL(oodle_dll)
        fn = _oodle.OodleLZ_Decompress
        fn.restype = ctypes.c_int64
        fn.argtypes = [ctypes.c_char_p, ctypes.c_int64, ctypes.c_char_p, ctypes.c_int64,
                       ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int64,
                       ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int64,
                       ctypes.c_int]
    out = ctypes.create_string_buffer(raw_size)
    n = _oodle.OodleLZ_Decompress(comp, len(comp), out, raw_size, 1, 0, 0, None, 0,
                                  None, None, None, 0, 3)
    if n != raw_size:
        raise ValueError(f"Oodle decompression failed ({n} != {raw_size})")
    return out.raw


def dcx_decompress(data: bytes, oodle_dll: str | None = None) -> bytes:
    if data[:4] != b"DCX\0":
        return data
    raw_size, comp_size = struct.unpack_from(">II", data, 0x1C)
    fmt = data[0x28:0x2C]
    header_size = struct.unpack_from(">I", data, 0x14)[0]
    comp = data[header_size:header_size + comp_size]
    if fmt == b"DFLT":
        out = zlib.decompress(comp)
    elif fmt == b"ZSTD":
        from compression import zstd  # Python 3.14+
        out = zstd.decompress(comp)
    elif fmt == b"KRAK":
        if not oodle_dll:
            raise ValueError("KRAK DCX needs the game's oo2core_6_win64.dll")
        out = _oodle_decompress(comp, raw_size, oodle_dll)
    else:
        raise ValueError(f"Unsupported DCX format {fmt!r}")
    if len(out) != raw_size:
        raise ValueError(f"DCX size mismatch {len(out)} != {raw_size}")
    return out


def _read_utf16z(data: bytes, off: int) -> str:
    end = off
    while data[end:end + 2] != b"\0\0":
        end += 2
    return data[off:end].decode("utf-16-le")


def read_bnd4(data: bytes, oodle_dll: str | None = None) -> dict[str, bytes]:
    if data[:4] != b"BND4":
        raise ValueError("Not a BND4 file")
    file_count = struct.unpack_from("<i", data, 0x0C)[0]
    file_header_size = struct.unpack_from("<q", data, 0x20)[0]
    unicode = data[0x30] != 0
    files = {}
    for i in range(file_count):
        h = 0x40 + i * file_header_size
        comp_size = struct.unpack_from("<q", data, h + 0x08)[0]
        if file_header_size == 0x24:
            data_off, _fid, name_off = struct.unpack_from("<IiI", data, h + 0x18)
        elif file_header_size == 0x28:
            data_off, = struct.unpack_from("<q", data, h + 0x18)
            _fid, name_off = struct.unpack_from("<iI", data, h + 0x20)
        else:
            raise ValueError(f"Unsupported BND4 file header size {file_header_size:#x}")
        if unicode:
            name = _read_utf16z(data, name_off)
        else:
            end = data.index(b"\0", name_off)
            name = data[name_off:end].decode("shift_jis")
        blob = data[data_off:data_off + comp_size]
        files[name.replace("\\", "/").split("/")[-1]] = dcx_decompress(blob, oodle_dll)
    return files


class Param:
    """PARAM table with 64-bit row headers (Elden Ring)."""

    def __init__(self, data: bytes):
        self.data = data
        row_count = struct.unpack_from("<H", data, 0x0A)[0]
        self.rows: dict[int, int] = {}
        offsets = []
        for i in range(row_count):
            rid, data_off = struct.unpack_from("<i4xq", data, 0x40 + i * 0x18)
            self.rows[rid] = data_off
            offsets.append(data_off)
        offsets.sort()
        self.row_size = offsets[1] - offsets[0] if len(offsets) > 1 else 0

    def ids(self):
        return list(self.rows.keys())

    def get(self, rid: int, off: int, fmt: str):
        return struct.unpack_from("<" + fmt, self.data, self.rows[rid] + off)[0]

    def bit(self, rid: int, off: int, bit: int) -> bool:
        return bool(self.data[self.rows[rid] + off] >> bit & 1)


def read_fmg(data: bytes) -> dict[int, str]:
    if data[2] != 2:
        raise ValueError(f"Unsupported FMG version {data[2]}")
    group_count, string_count = struct.unpack_from("<ii", data, 0x0C)
    str_offsets_off = struct.unpack_from("<q", data, 0x18)[0]
    out = {}
    for g in range(group_count):
        index, first, last = struct.unpack_from("<iii", data, 0x28 + g * 0x10)
        for k, rid in enumerate(range(first, last + 1)):
            soff = struct.unpack_from("<q", data, str_offsets_off + (index + k) * 8)[0]
            if soff > 0:
                out[rid] = _read_utf16z(data, soff)
    return out
