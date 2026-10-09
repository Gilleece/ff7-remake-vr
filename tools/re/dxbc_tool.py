"""DXBC (D3D11 shader bytecode) helper for the shaders the mod captures (`shader dump`).

    python tools/re/dxbc_tool.py check  <file.dxbc>            recompute the checksum and compare
    python tools/re/dxbc_tool.py asm    <file.dxbc> [out.asm]  disassemble (d3dcompiler_47.dll)
    python tools/re/dxbc_tool.py origin <file.dxbc> [out.dxbc] find (and patch) the view-origin add

The checksum is MD5's block function over the bytecode after the 20-byte header, with the
container's own padding (the bit count in the first word of the last block, (bits >> 2) | 1
in its last word) and no final MD5 step; the D3D runtime refuses a shader whose checksum is
wrong. `origin` looks for the pattern of Square Enix's screen-space reflections (one
`add rX, vN, cbM[R]` and one `add rY, vN, -cbM[R]`, see docs/re/engine.md, "Screen-space
reflections: the pixel shader") and writes a copy whose first add selects .zzzz, re-signed:
the same patch the mod applies at run time (src/engine/src/shaders.cpp).
"""
import ctypes
import math
import struct
import sys

_K = [int(abs(math.sin(i + 1)) * 2**32) & 0xFFFFFFFF for i in range(64)]
_S = [7, 12, 17, 22] * 4 + [5, 9, 14, 20] * 4 + [4, 11, 16, 23] * 4 + [6, 10, 15, 21] * 4


def _block(state, blk):
    m = struct.unpack('<16I', blk)
    a, b, c, d = state
    A, B, C, D = a, b, c, d
    for i in range(64):
        if i < 16:
            f, g = (B & C) | (~B & D), i
        elif i < 32:
            f, g = (D & B) | (~D & C), (5 * i + 1) % 16
        elif i < 48:
            f, g = B ^ C ^ D, (3 * i + 5) % 16
        else:
            f, g = C ^ (B | (~D & 0xFFFFFFFF)), (7 * i) % 16
        x = (A + (f & 0xFFFFFFFF) + _K[i] + m[g]) & 0xFFFFFFFF
        A, D, C = D, C, B
        B = (B + ((x << _S[i]) | (x >> (32 - _S[i])))) & 0xFFFFFFFF
    return [(a + A) & 0xFFFFFFFF, (b + B) & 0xFFFFFFFF, (c + C) & 0xFFFFFFFF, (d + D) & 0xFFFFFFFF]


def checksum(data):
    p = bytes(data[20:])
    n = len(p)
    bits = (n * 8) & 0xFFFFFFFF
    tail = ((bits >> 2) | 1) & 0xFFFFFFFF
    st = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476]
    full = n & ~63
    for i in range(0, full, 64):
        st = _block(st, p[i:i + 64])
    last = n - full
    blk = bytearray(64)
    if last >= 56:
        blk[:last] = p[full:]
        blk[last] = 0x80
        st = _block(st, bytes(blk))
        blk = bytearray(64)
        blk[0:4] = struct.pack('<I', bits)
        blk[60:64] = struct.pack('<I', tail)
        st = _block(st, bytes(blk))
    else:
        blk[0:4] = struct.pack('<I', bits)
        blk[4:4 + last] = p[full:]
        blk[4 + last] = 0x80
        blk[60:64] = struct.pack('<I', tail)
        st = _block(st, bytes(blk))
    return struct.pack('<4I', *st)


def hash_string(h):
    """The checksum as the mod logs it: four little-endian words in hex."""
    return '%08x%08x%08x%08x' % struct.unpack('<4I', bytes(h))


def disassemble(data):
    d3dc = ctypes.WinDLL('d3dcompiler_47.dll')
    blob = ctypes.c_void_p()
    hr = d3dc.D3DDisassemble(ctypes.c_char_p(bytes(data)), ctypes.c_size_t(len(data)), 0, None, ctypes.byref(blob))
    if hr != 0 or not blob:
        raise RuntimeError('D3DDisassemble failed 0x%08x' % (hr & 0xFFFFFFFF))
    vt = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(blob)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(blob)
    text = ctypes.string_at(ptr, size).decode('ascii', 'replace').rstrip('\0')
    ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])(blob)
    return text


def _operand(tok, i):
    t = tok[i]
    i += 1
    o = {'pos': i - 1, 'token': t, 'type': (t >> 12) & 0xFF, 'dim': (t >> 20) & 3, 'mod': 0, 'idx': [], 'imm': True}
    ext = t >> 31
    while ext:
        e = tok[i]
        i += 1
        if e & 0x3F == 1:
            o['mod'] = (e >> 6) & 0xFF
        ext = e >> 31
    if o['type'] == 4:
        i += 4 if t & 3 == 2 else 1
    if o['type'] == 5:
        i += 8 if t & 3 == 2 else 2
    for k in range(o['dim']):
        rep = (t >> (22 + 3 * k)) & 7
        if rep in (0, 3):
            o['idx'].append(tok[i])
            i += 1
        elif rep in (1, 4):
            o['idx'].append(tok[i])
            i += 2
        if rep >= 2:
            o['imm'] = False
            _, i = _operand(tok, i)
    return o, i


def find_view_origin_add(data):
    """Byte offset of the operand token to patch, or None."""
    count = struct.unpack_from('<I', data, 28)[0]
    for c in range(count):
        off = struct.unpack_from('<I', data, 32 + 4 * c)[0]
        if data[off:off + 4] not in (b'SHEX', b'SHDR'):
            continue
        size = struct.unpack_from('<I', data, off + 4)[0]
        base = off + 8
        tok = struct.unpack_from('<%dI' % (size // 4), data, base)
        n = min(tok[1], len(tok))
        plus, minus = [], []
        i = 2
        while i < n:
            t = tok[i]
            op = t & 0x7FF
            ln = tok[i + 1] if op == 53 else (t >> 24) & 0x7F
            if ln == 0:
                return None
            if op == 0:  # add
                j = i + 1
                e = t >> 31
                while e:
                    e = tok[j] >> 31
                    j += 1
                _, j = _operand(tok, j)
                a, j = _operand(tok, j)
                b, j = _operand(tok, j)
                if a['type'] == 1 and a['dim'] == 1 and a['mod'] == 0 and b['type'] == 8 and b['dim'] == 2 and b['imm']:
                    (plus if b['mod'] == 0 else minus if b['mod'] == 1 else []).append((a, b))
            i += ln
        pairs = [(p, m) for p in plus for m in minus if p[0]['idx'][0] == m[0]['idx'][0] and p[1]['idx'] == m[1]['idx']]
        if len(pairs) != 1:
            return None
        b = pairs[0][0][1]
        if b['token'] & 3 != 2 or (b['token'] >> 2) & 3 != 1:
            return None
        return base + 4 * b['pos']
    return None


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path = argv[1], argv[2]
    data = bytearray(open(path, 'rb').read())
    if data[:4] != b'DXBC':
        print('not a DXBC container')
        return 1
    if cmd == 'check':
        ok = checksum(data) == bytes(data[4:20])
        print('checksum %s: %s' % (hash_string(data[4:20]), 'matches' if ok else 'DIFFERS (recomputed %s)' % hash_string(checksum(data))))
        return 0 if ok else 1
    if cmd == 'asm':
        text = disassemble(data)
        if len(argv) > 3:
            open(argv[3], 'w', newline='\n').write(text)
        else:
            print(text)
        return 0
    if cmd == 'origin':
        at = find_view_origin_add(data)
        if at is None:
            print('no view-origin add found')
            return 1
        print('view-origin add: operand token at byte %d' % at)
        if len(argv) > 3:
            v = struct.unpack_from('<I', data, at)[0]
            struct.pack_into('<I', data, at, (v & ~(0xFF << 4)) | (0xAA << 4))
            data[4:20] = checksum(data)
            open(argv[3], 'wb').write(data)
            print('patched copy written: %s (checksum %s)' % (argv[3], hash_string(data[4:20])))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
