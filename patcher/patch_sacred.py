"""Sacred Gold (Steam, Sacred.exe md5 007580390e5c177a9144bfb66c02d6fe) ultrawide patcher.

Input : the SteamStub-free executable produced by Steamless from the original.
Output: a Sacred.exe whose display mode is WIDTH x HEIGHT and whose world
        renderer culls/centres on WIDTH instead of 1024.

Every patch is (VA, expected original bytes, new bytes); the script refuses to
write anything if an expected byte sequence does not match.

Usage: python patch_sacred.py <unpacked.exe> <out.exe> <width> <height>
"""
import struct
import sys

IMAGE_BASE = 0x400000

# Shared .rdata float constants used by the world renderer (0x623000-0x632000).
F1024 = 0x8906F4  # 1024.0f: screen width (culling bound, tile-extent)
F512 = 0x8906FC   # 512.0f : screen centre X

# New constants live in the zero slack at the end of .rdata.
NEW_W = 0x8E6F00   # WIDTH as float
NEW_HW = 0x8E6F04  # WIDTH / 2 as float
RDATA_VS_FIELD_NEW = 0x57000  # grow .rdata VirtualSize to cover the slack

# x87 "op float ptr [abs32]" sites (opcode D8 /r, 6 bytes); the absolute
# operand at +2 is redirected. Evidence: REA inspect_native_instruction.
WIDTH_SITES = [
    0x624A12,  # FMUL  1024 -> visible tile columns (FUN_00624a00)
    0x628E85,  # FCOMP 1024 -> x culling (FUN_00628d50)
    0x6291B4,  # FCOMP 1024 -> x culling (FUN_00629180)
    0x62B2CA,  # FCOMP 1024 -> x culling (FUN_0062ae90)
    0x62E8BB, 0x62F19D,  # FCOMP 1024 (FUN_0062e410)
    0x6304E6, 0x630DCF,  # FCOMP 1024 (FUN_0062ff60)
]
CENTRE_SITES = [
    0x62B315,  # FUN_0062ae90
    0x62E20C,  # FUN_0062dd00
    0x62E7F0, 0x62E819, 0x62EE68, 0x62EE82,  # FUN_0062e410
    0x63041A, 0x630445, 0x630A98, 0x630AB2,  # FUN_0062ff60
    0x631EC2, 0x631EEC,  # FUN_00631eb0
]


def imm(v):
    return struct.pack("<I", v)


def patches(w, h):
    out = [
        # WinMain (FUN_00815f70): findDisplayMode(1024, 768, bpp) before the
        # dxDriver7 is constructed. Evidence: REA ev_091c72c3..., ev_d978e8c2...
        (0x816C70, imm(768), imm(h), "display mode height"),
        (0x816C75, imm(1024), imm(w), "display mode width"),
        (NEW_W, bytes(4), struct.pack("<f", float(w)), "float width"),
        (NEW_HW, bytes(4), struct.pack("<f", w / 2.0), "float half width"),
    ]
    out += [(va + 2, imm(F1024), imm(NEW_W), "renderer width") for va in WIDTH_SITES]
    out += [(va + 2, imm(F512), imm(NEW_HW), "renderer centre x") for va in CENTRE_SITES]
    return out


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    n = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    so = pe + 24 + opt
    for i in range(n):
        yield so + 40 * i, struct.unpack_from("<8sIIII", data, so + 40 * i)


def va_to_off(data, va):
    rva = va - IMAGE_BASE
    for _, (_, vs, va_s, rs, rp) in sections(data):
        if va_s <= rva < va_s + max(vs, rs):
            return rva - va_s + rp
    raise ValueError(hex(va))


def grow_rdata(data):
    for hdr, (name, vs, va_s, rs, rp) in sections(data):
        if name.rstrip(b"\0") == b".rdata":
            assert vs <= RDATA_VS_FIELD_NEW <= rs, (hex(vs), hex(rs))
            struct.pack_into("<I", data, hdr + 8, RDATA_VS_FIELD_NEW)
            return
    raise ValueError(".rdata not found")


def main():
    src, dst, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    data = bytearray(open(src, "rb").read())
    plan = patches(w, h)
    for va, old, new, what in plan:
        off = va_to_off(data, va)
        if bytes(data[off:off + len(old)]) != old:
            sys.exit(f"mismatch at {va:#x} ({what}): {data[off:off+len(old)].hex()} != {old.hex()}")
    grow_rdata(data)
    for va, old, new, what in plan:
        off = va_to_off(data, va)
        data[off:off + len(new)] = new
        print(f"{va:#x} {what}: {old.hex()} -> {new.hex()}")
    open(dst, "wb").write(data)
    print("written", dst)


if __name__ == "__main__":
    main()
