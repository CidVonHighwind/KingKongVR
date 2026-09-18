#!/usr/bin/env python3
"""
Generate pass-through wrapper method lists for COM interfaces (Direct3D 9) by
parsing the Windows SDK headers.

Emitting these from the header (rather than hand-writing 119 methods) means the
signatures are correct by construction, and the compiler catches any method we
fail to implement because the COM interface is pure-virtual.

Output: src/device/generated/<iface>_methods.inl, a list of macro invocations:

    D3D9_METHOD(ret, Name, (type a1, type a2), (a1, a2))
    D3D9_METHOD_CUSTOM(ret, Name, (type a1), (a1))     // we implement by hand

Usage:  python tools/gen_wrapper.py
"""

import os
import re
import sys
import glob

# Methods we implement by hand in the wrapper rather than passing straight
# through. Everything else is forwarded verbatim to the real device.
CUSTOM = {
    "IDirect3D9": {
        "QueryInterface", "AddRef", "Release",
        "CreateDevice",
    },
    "IDirect3DDevice9": {
        "QueryInterface", "AddRef", "Release",
        # frame structure (wrapper.cpp)
        "Present", "Reset",
        # D3D9Ex: D3DPOOL_MANAGED is not allowed; render-target textures noted
        "CreateTexture", "CreateVertexBuffer", "CreateIndexBuffer", "CreateCubeTexture",
        "CreateVolumeTexture",
        # the proxy (recording.cpp): every state change and draw is recorded
        # into the state model and the frame recording, not executed ...
        "BeginScene", "EndScene", "Clear", "StretchRect", "ColorFill",
        "SetTransform", "SetRenderState", "SetSamplerState", "SetTextureStageState",
        "SetTexture", "SetFVF", "SetStreamSource", "SetStreamSourceFreq",
        "SetVertexDeclaration", "SetIndices", "SetVertexShader", "SetPixelShader",
        "SetVertexShaderConstantF", "SetVertexShaderConstantI", "SetVertexShaderConstantB",
        "SetPixelShaderConstantF", "SetPixelShaderConstantI", "SetPixelShaderConstantB",
        "SetLight", "LightEnable", "SetMaterial", "SetClipPlane", "SetViewport",
        "SetScissorRect", "SetRenderTarget", "SetDepthStencilSurface", "SetNPatchMode",
        "SetSoftwareVertexProcessing", "SetCurrentTexturePalette", "UpdateSurface",
        "UpdateTexture",
        "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP",
        # ... and the game's queries are answered from the state model
        "GetTransform", "GetRenderState", "GetSamplerState", "GetTextureStageState",
        "GetTexture", "GetFVF", "GetStreamSource", "GetVertexDeclaration", "GetIndices",
        "GetVertexShader", "GetPixelShader", "GetVertexShaderConstantF", "GetLight",
        "GetLightEnable", "GetMaterial", "GetClipPlane", "GetViewport", "GetScissorRect",
        "GetRenderTarget", "GetDepthStencilSurface",
    },
}

METHOD_RE = re.compile(
    r"STDMETHOD(?P<typed>_)?\s*\(\s*(?P<body>.*?)\s*\)\s*\((?P<params>.*?)\)\s*PURE\s*;",
    re.S,
)


# Headers to parse: (file name, SDK include subdirectory, interfaces).
HEADERS = [
    ("d3d9.h", "shared", ("IDirect3D9", "IDirect3DDevice9")),
]


def find_header(name, subdir):
    roots = [
        r"C:\Program Files (x86)\Windows Kits\10\Include",
        r"C:\Program Files\Windows Kits\10\Include",
    ]
    found = []
    for r in roots:
        found += glob.glob(os.path.join(r, "*", subdir, name))
    if not found:
        sys.exit("could not locate %s in the Windows SDK" % name)
    # highest SDK version wins
    return sorted(found)[-1]


def extract_interface(text, name):
    """Return the body of DECLARE_INTERFACE_(name, ...) { ... };"""
    start = text.find("DECLARE_INTERFACE_(%s," % name)
    if start < 0:
        sys.exit("interface %s not found in header" % name)
    brace = text.index("{", start)
    depth, i = 0, brace
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1 : i]
        i += 1
    sys.exit("unterminated interface body for %s" % name)


def split_params(params):
    """THIS_ a,b,c  ->  ['a','b','c'];  THIS -> []"""
    p = params.strip()
    if p.startswith("THIS_"):
        p = p[len("THIS_") :]
    elif p == "THIS" or p == "":
        return []
    p = p.strip()
    if not p:
        return []
    return [x.strip() for x in p.split(",") if x.strip()]


IDENT_RE = re.compile(r"[A-Za-z_]\w*")
# cv-qualifiers never count as the type identifier.
QUALIFIERS = {"CONST", "const", "volatile"}
# Built-in type keywords can never be a declarator name, so if one lands in
# trailing position the parameter is unnamed ('unsigned int', 'void*').
BUILTINS = {"unsigned", "signed", "long", "short", "int", "char", "float",
            "double", "void", "struct", "enum", "union"}


def split_type_and_name(param):
    """Split a parameter into its type text and declarator name.

    C guarantees every parameter has a type, so the rule is: if there is only
    one identifier once qualifiers are discounted, that identifier is the type
    and the parameter is unnamed. Otherwise the last identifier is the name.
    This beats guessing from capitalisation, which gets both 'DWORD FVF' (an
    ALL-CAPS name) and 'CONST D3DMATRIX*' (no name) wrong.

        'CONST RECT* pSourceRect'          -> ('CONST RECT*', 'pSourceRect')
        'D3DCAPS9 *pCaps'                  -> ('D3DCAPS9 *', 'pCaps')
        'DWORD FVF'                        -> ('DWORD', 'FVF')
        'CONST D3DMATRIX*'                 -> ('CONST D3DMATRIX*', None)
        'D3DTRANSFORMSTATETYPE'            -> ('D3DTRANSFORMSTATETYPE', None)
    """
    p = param.strip()
    if "[" in p:  # array declarator: the name sits before the brackets
        p = p[: p.index("[")].strip()

    idents = [t for t in IDENT_RE.findall(p) if t not in QUALIFIERS]
    if len(idents) < 2 or idents[-1] in BUILTINS:
        return p, None

    name = idents[-1]
    return p[: p.rfind(name)].strip(), name


def normalize(params):
    """Return (param_decl_list, arg_name_list) with every parameter named.

    Declarations are rebuilt as '<type> <name>' so that synthesized and real
    names are handled identically, and the result is self-checking: every
    declaration must end with the argument name we pass along.
    """
    decls, names = [], []
    for i, raw in enumerate(params):
        type_text, name = split_type_and_name(raw)
        if name is None:
            name = "a%d" % (i + 1)
        decl = "%s %s" % (type_text, name)
        if not decl.endswith(" " + name):
            sys.exit("failed to normalize parameter %r" % raw)
        decls.append(decl)
        names.append(name)
    return decls, names


def parse(body):
    out = []
    for m in METHOD_RE.finditer(body):
        inner = m.group("body")
        if m.group("typed"):
            # STDMETHOD_(RetType, Name)
            ret, name = inner.rsplit(",", 1)
            ret, name = ret.strip(), name.strip()
        else:
            ret, name = "HRESULT", inner.strip()
        decls, names = normalize(split_params(m.group("params")))
        out.append((ret, name, decls, names))
    return out


def emit(header, iface, methods):
    lines = [
        "// GENERATED by tools/gen_wrapper.py -- do not edit by hand.",
        "// Source: Windows SDK %s, interface %s" % (os.path.basename(header), iface),
        "// %d methods" % len(methods),
        "",
    ]
    for ret, name, params, names in methods:
        macro = "D3D9_METHOD_CUSTOM" if name in CUSTOM[iface] else "D3D9_METHOD"
        plist = ", ".join(params) if params else ""
        alist = ", ".join(names) if names else ""
        lines.append("%s(%s, %s, (%s), (%s))" % (macro, ret, name, plist, alist))
    lines.append("")
    return "\n".join(lines)


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    outdir = os.path.join(here, "src", "device", "generated")
    os.makedirs(outdir, exist_ok=True)
    for name, subdir, interfaces in HEADERS:
        generate(find_header(name, subdir), interfaces, outdir, here)


def generate(header, interfaces, outdir, here):
    text = open(header, "r", encoding="utf-8", errors="replace").read()
    print("header: %s" % header)
    for iface in interfaces:
        methods = parse(extract_interface(text, iface))
        custom = [m for m in methods if m[1] in CUSTOM[iface]]
        missing = CUSTOM[iface] - {m[1] for m in methods}
        if missing:
            sys.exit("CUSTOM lists unknown methods for %s: %s" % (iface, sorted(missing)))
        path = os.path.join(outdir, "%s_methods.inl" % iface.lower())
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(emit(header, iface, methods))
        print("  %-22s %3d methods (%d custom) -> %s"
              % (iface, len(methods), len(custom), os.path.relpath(path, here)))


if __name__ == "__main__":
    main()
