import pefile

NEW = r"D:\INDAC\DGLab_IDAC\build\Release\DGLab_IDAC.dll"
for tag, p in [("NEW (with shim)", NEW)]:
    pe = pefile.PE(p)
    print("---", tag, p.rsplit("\\", 1)[-1])
    for e in pe.DIRECTORY_ENTRY_IMPORT:
        dl = e.dll.decode()
        if "UE4SS" not in dl:
            continue
        for imp in e.imports:
            n = imp.name.decode() if imp.name else str(imp.ordinal)
            short = n
            if n.startswith("?"):
                body = n[1:]
                nm = body.split("@")[0]
                rem = body[len(nm) + 1:]
                cls = rem.split("@@")[0] if "@@" in rem else rem
                short = "%s   (%s)" % (nm, cls)
            print("   ", short)
