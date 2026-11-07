import re, struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
n = struct.unpack_from('<H', d, pe+6)[0]; opt = struct.unpack_from('<H', d, pe+20)[0]
secs = []
for i in range(n):
    nm, vs, va, rs, rp = struct.unpack_from('<8sIIII', d, pe+24+opt+40*i)
    secs.append((nm.rstrip(b'\0').decode(), va, rs, rp))
def va(off):
    for nm, v, rs, rp in secs:
        if rp <= off < rp+rs: return 0x400000+v+off-rp, nm
    return None, None
pats = {
 'mov [abs],1024': rb'\xC7\x05....\x00\x04\x00\x00',
 'mov [abs],768':  rb'\xC7\x05....\x00\x03\x00\x00',
 'mov [r+d8],1024': rb'\xC7[\x40-\x47\x49-\x4F].\x00\x04\x00\x00',
 'mov [r+d8],768':  rb'\xC7[\x40-\x47\x49-\x4F].\x00\x03\x00\x00',
 'cmp r,1024': rb'\x81[\xF8-\xFF]\x00\x04\x00\x00',
 'cmp r,768':  rb'\x81[\xF8-\xFF]\x00\x03\x00\x00',
}
for k, p in pats.items():
    hits = [va(m.start()) for m in re.finditer(p, d, re.S)]
    hits = [h for h in hits if h[1] == '.text']
    print(k, len(hits), ' '.join(hex(h[0]) for h in hits[:40]))
for k, f in [('1024.0f', 1024.0), ('768.0f', 768.0), ('512.0f', 512.0), ('384.0f', 384.0)]:
    b = struct.pack('<f', f)
    hits = [va(m.start()) for m in re.finditer(re.escape(b), d)]
    print(k, [(hex(a), s) for a, s in hits if s in ('.rdata', '.data')][:20])
