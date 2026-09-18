#!/usr/bin/env python3
"""
Reverse-engineering helper for King Kong (kingkong9d.exe).

Everything the mod patches or hooks in the game executable was found with the
commands below. `verify` checks that an executable still has every byte
the mod relies on, which is the first thing to run against another build.

Requires:  python -m pip install capstone pefile

Usage (default executable: game/kingkong9d.exe next to this repository):
  python tools/re/kkre.py identity
  python tools/re/kkre.py verify
  python tools/re/kkre.py dis 0xa051a0 [length]      disassemble, annotated
  python tools/re/kkre.py fn 0xa051a0                 disassemble to the first ret
  python tools/re/kkre.py callers 0xa05050            direct calls (E8 rel32)
  python tools/re/kkre.py refs 0x3de9368              any 32-bit reference
  python tools/re/kkre.py imports [dll-substring]
  python tools/re/kkre.py strings REGEX               ASCII + UTF-16 strings
  python tools/re/kkre.py --exe path\\to\\other.exe verify
"""

import argparse
import hashlib
import os
import re
import struct
import sys

try:
    import capstone
    import pefile
except ImportError:
    sys.exit("needs: python -m pip install capstone pefile")

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_EXE = os.path.join(HERE, "..", "..", "game", "kingkong9d.exe")

# The build all addresses in this repository refer to.
KNOWN_BUILD = {
    "size": 7376896,
    "timestamp": 0x5C5BD91B,
    "sha256": "c15dbc3ba277929ab58f501470572c2b6237afa7ef35ed3cb8b8ad1913c12281",
}

# Every byte sequence the mod depends on. Every new patch must be added here.
PATCH_SITES = [
    # (address, hex bytes, what, source file)
    (0xA00EF0, "c7056893de035007a000",
     "driver init: mov [0x3de9368], 0xa00750 (input update pointer)", "src/game/joystick.cpp"),
    (0xA051BD, "d815943ca800",
     "frame step: fcom dword ptr [0xa83c94] (0.01 s floor)", "src/game/timing.cpp"),
    (0xA051CC, "d905943ca800",
     "frame step: fld dword ptr [0xa83c94] (0.01 s floor)", "src/game/timing.cpp"),
    (0xA83C94, "0ad7233c",
     "float constant 0.01 (read by the two frame step instructions)", "src/game/timing.cpp"),
    (0x96A30A, "d90554bfae00",
     "view fit: fld dword ptr [0xaebf54] (aspect table reader)", "src/game/aspect.cpp"),
    (0x96A31E, "d9048550bfae00",
     "view fit: fld dword ptr [eax*4+0xaebf50] (aspect table reader)", "src/game/aspect.cpp"),
    (0xAEBF50, "0000803f0000803f0000103f1010103f",
     "aspect table 1.0, 1.0, 9/16, ~9/16 (entries 2-3 patched at runtime)", "src/game/aspect.cpp"),
    (0xA9150C, "8988883e",
     "float constant 0.26667 (frame step ceiling, not patched)", "reference only"),
]


class Exe:
    def __init__(self, path):
        self.path = path
        self.raw = open(path, "rb").read()
        self.pe = pefile.PE(data=self.raw)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.image = self.pe.get_memory_mapped_image()
        text = next(s for s in self.pe.sections if s.Name.startswith(b".text"))
        self.text_start = text.VirtualAddress
        self.text_end = text.VirtualAddress + text.Misc_VirtualSize
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def read(self, va, n):
        off = va - self.base
        if off < 0 or off + n > len(self.image):
            return None
        return self.image[off:off + n]

    def cstr(self, va, limit=120):
        data = self.read(va, 1)
        if data is None:
            return None
        off = va - self.base
        end = self.image.find(b"\0", off, off + limit)
        if end < 0:
            return None
        s = self.image[off:end]
        if len(s) < 3 or any(c < 32 or c > 126 for c in s):
            return None
        return s.decode()

    def annotate(self, ins):
        """Strings and float constants behind absolute operands."""
        notes = []
        for tok in re.findall(r"0x[0-9a-f]{5,8}", ins.op_str):
            va = int(tok, 16)
            s = self.cstr(va)
            if s:
                notes.append(repr(s))
                continue
            size = None
            if "qword ptr [" + tok in ins.op_str:
                size = 8
            elif "dword ptr [" + tok in ins.op_str and ins.mnemonic.startswith("f"):
                size = 4
            if size:
                data = self.read(va, size)
                in_initialised = data is not None and any(
                    s.VirtualAddress <= va - self.base < s.VirtualAddress + s.SizeOfRawData
                    for s in self.pe.sections)
                if in_initialised:
                    fmt = "<d" if size == 8 else "<f"
                    notes.append("= %g" % struct.unpack(fmt, data)[0])
                else:
                    notes.append("(runtime variable)")
        return ("   ; " + ", ".join(notes)) if notes else ""

    def disassemble(self, va, length, stop_at_ret=False):
        data = self.read(va, length)
        if data is None:
            sys.exit("address outside the image")
        for ins in self.md.disasm(data, va):
            print("%08x  %-8s %s%s" % (ins.address, ins.mnemonic, ins.op_str,
                                      self.annotate(ins)))
            if stop_at_ret and ins.mnemonic == "ret":
                break

    def callers(self, target):
        text = self.image[self.text_start:self.text_end]
        out = []
        for m in re.finditer(rb"\xe8", text):
            off = self.text_start + m.start()
            rel = struct.unpack("<i", self.image[off + 1:off + 5])[0]
            if (self.base + off + 5 + rel) & 0xFFFFFFFF == target:
                out.append(self.base + off)
        return out

    def refs(self, value):
        needle = struct.pack("<I", value)
        return [self.base + m.start() for m in re.finditer(re.escape(needle), self.image)]

    def function_start(self, va, max_back=0x4000):
        off = va - self.base
        for _ in range(max_back):
            if self.image[off - 1] == 0xCC and self.image[off - 2] == 0xCC:
                return self.base + off
            off -= 1
        return None


def cmd_identity(exe, _):
    pe = exe.pe
    digest = hashlib.sha256(exe.raw).hexdigest()
    print("file       ", exe.path)
    print("size        %d" % len(exe.raw))
    print("timestamp   0x%08X" % pe.FILE_HEADER.TimeDateStamp)
    print("image base  0x%X (dynamic base: %s)" % (
        exe.base, bool(pe.OPTIONAL_HEADER.DllCharacteristics & 0x40)))
    print("sha256      %s" % digest)
    print("known build:", digest == KNOWN_BUILD["sha256"])


def cmd_verify(exe, _):
    ok = True
    for va, expected, what, source in PATCH_SITES:
        got = exe.read(va, len(expected) // 2)
        match = got is not None and got.hex() == expected
        ok &= match
        print("%s %08x  %-22s %s  [%s]" % ("ok  " if match else "FAIL", va, expected,
                                           what, source))
        if not match:
            print("      found: %s" % (got.hex() if got else "(outside image)"))
    print("all patch sites match" if ok else "MISMATCH: do not use the mod's patches on this build")
    return 0 if ok else 1


def cmd_dis(exe, args):
    exe.disassemble(int(args.address, 16), int(args.length, 0))


def cmd_fn(exe, args):
    va = int(args.address, 16)
    start = exe.function_start(va) or va
    if start != va:
        print("; function starts at %08x" % start)
    exe.disassemble(start, 0x2000, stop_at_ret=True)


def cmd_callers(exe, args):
    for c in exe.callers(int(args.address, 16)):
        f = exe.function_start(c)
        print("%08x  in function %s" % (c, "%08x" % f if f else "?"))


def cmd_refs(exe, args):
    for r in exe.refs(int(args.address, 16)):
        in_code = exe.text_start <= r - exe.base < exe.text_end
        print("%08x  %s" % (r, "code" if in_code else "data"))


def cmd_imports(exe, args):
    for entry in exe.pe.DIRECTORY_ENTRY_IMPORT:
        dll = entry.dll.decode()
        if args.filter and args.filter.lower() not in dll.lower():
            continue
        print("%s  (OriginalFirstThunk 0x%x, FirstThunk 0x%x)" % (
            dll, entry.struct.OriginalFirstThunk, entry.struct.FirstThunk))
        for imp in entry.imports:
            name = imp.name.decode() if imp.name else "#%d" % imp.ordinal
            print("    %08x  %s" % (imp.address, name))


def cmd_strings(exe, args):
    pattern = re.compile(args.regex, re.I)
    for m in re.finditer(rb"[\x20-\x7e]{4,}", exe.image):
        s = m.group().decode()
        if pattern.search(s):
            print("%08x  A  %s" % (exe.base + m.start(), s))
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){4,}", exe.image):
        s = m.group().decode("utf-16le")
        if pattern.search(s):
            print("%08x  W  %s" % (exe.base + m.start(), s))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", default=DEFAULT_EXE)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("identity")
    sub.add_parser("verify")
    p = sub.add_parser("dis"); p.add_argument("address"); p.add_argument("length", nargs="?", default="0x80")
    p = sub.add_parser("fn"); p.add_argument("address")
    p = sub.add_parser("callers"); p.add_argument("address")
    p = sub.add_parser("refs"); p.add_argument("address")
    p = sub.add_parser("imports"); p.add_argument("filter", nargs="?")
    p = sub.add_parser("strings"); p.add_argument("regex")
    args = parser.parse_args()

    exe = Exe(os.path.abspath(args.exe))
    handler = {
        "identity": cmd_identity, "verify": cmd_verify, "dis": cmd_dis, "fn": cmd_fn,
        "callers": cmd_callers, "refs": cmd_refs, "imports": cmd_imports,
        "strings": cmd_strings,
    }[args.command]
    sys.exit(handler(exe, args) or 0)


if __name__ == "__main__":
    main()
