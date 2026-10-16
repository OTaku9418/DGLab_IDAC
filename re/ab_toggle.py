# ---------------------------------------------------------------------------
# A/B toggle: enable/disable the DGLab_IDAC UE4SS mod.
#
# Why a script instead of hand-editing: mods.txt is a CRLF, UTF-8 file and
# UE4SS treats only lines beginning with ';' as comments. Editing by hand
# already truncated this file to 0 bytes once. This script also re-reads the
# file after writing and refuses to leave it in a broken state.
#
# Usage:
#   python re\ab_toggle.py --off      disable our mod
#   python re\ab_toggle.py --on       enable our mod
#   python re\ab_toggle.py --status   print current state
# ---------------------------------------------------------------------------
import argparse
import os
import sys

MODS_DIR = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods"
MODS_TXT = os.path.join(MODS_DIR, "mods.txt")
ENABLED_TXT = os.path.join(MODS_DIR, "DGLab_IDAC", "enabled.txt")
ENTRY = "DGLab_IDAC : 1"
ENTRY_OFF = ";DGLab_IDAC : 1    <- A/B TEST: our mod temporarily OFF"


def read_lines(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    nl = "\r\n" if b"\r\n" in raw else "\n"
    return raw.decode("utf-8").split(nl), nl


def write_lines(path, lines, nl):
    while lines and lines[-1] == "":
        lines.pop()
    with open(path, "wb") as fh:
        fh.write((nl.join(lines) + nl).encode("utf-8"))


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--on", action="store_true", help="enable our mod")
    g.add_argument("--off", action="store_true", help="disable our mod")
    g.add_argument("--status", action="store_true", help="print current state")
    args = ap.parse_args()

    if not os.path.isfile(MODS_TXT):
        print("ERROR: mods.txt not found at %s" % MODS_TXT)
        return 1

    lines, nl = read_lines(MODS_TXT)
    if len(lines) < 5:
        print("ERROR: mods.txt looks truncated (%d lines). Restoring from backup first." % len(lines))
        bak = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_ab_backup", "mods.txt.bak")
        if os.path.isfile(bak):
            with open(bak, "rb") as fh:
                data = fh.read()
            with open(MODS_TXT, "wb") as fh:
                fh.write(data)
            print("restored %d bytes from %s" % (len(data), bak))
            lines, nl = read_lines(MODS_TXT)
        else:
            print("ERROR: no backup at %s" % bak)
            return 1

    def is_on_line(s):
        return s.strip() == ENTRY or s.strip().startswith("DGLab_IDAC")

    def is_off_line(s):
        return s.strip().startswith(";DGLab_IDAC")

    has_on = any(is_on_line(l) for l in lines)
    has_off = any(is_off_line(l) for l in lines)
    enabled_txt = os.path.isfile(ENABLED_TXT)

    if args.status:
        print("mods.txt    : %s" % ("ENABLED" if has_on else
                                    ("present but commented OUT" if has_off else "absent")))
        print("enabled.txt : %s" % ("present" if enabled_txt else "absent"))
        print("=> our mod is %s" % ("ON" if (has_on or enabled_txt) else "OFF"))
        return 0

    if args.off:
        lines = [ENTRY_OFF if is_on_line(l) else l for l in lines]
        write_lines(MODS_TXT, lines, nl)
        if enabled_txt:
            os.remove(ENABLED_TXT)
            print("removed %s" % ENABLED_TXT)
        print("our mod is now OFF")
    else:
        lines = [ENTRY if is_off_line(l) else l for l in lines]
        if not any(is_on_line(l) for l in lines):
            lines.append(ENTRY)
        write_lines(MODS_TXT, lines, nl)
        if not os.path.isfile(ENABLED_TXT):
            open(ENABLED_TXT, "wb").close()
            print("created %s" % ENABLED_TXT)
        print("our mod is now ON")

    # post-write verification: never leave the file broken
    lines2, _ = read_lines(MODS_TXT)
    if len(lines2) < 5:
        print("ERROR: post-write check failed, mods.txt has %d lines!" % len(lines2))
        return 1
    print("post-write mods.txt: %d lines, %d CRLF"
          % (len(lines2) + 1, open(MODS_TXT, "rb").read().count(b"\r\n")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
