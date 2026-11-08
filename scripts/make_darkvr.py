# Creates game/DARKVR.EXE: DARK.EXE with the import DLL name P2DEN.dll -> P2VR.dll (same slot, NUL padded).
import sys
src, dst = sys.argv[1], sys.argv[2]
b = bytearray(open(src, 'rb').read())
i = b.find(b'P2DEN.dll\x00')
assert i > 0 and b.count(b'P2DEN.dll\x00') == 1
b[i:i+10] = b'P2VR.dll\x00\x00'
open(dst, 'wb').write(b)
print('patched import name at file offset', hex(i))
