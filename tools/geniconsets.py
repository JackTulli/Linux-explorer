#!/usr/bin/env python3
"""Build the switchable icon sets under icons/sets/<set>/<slug>.ico.

The desktop's built-in artwork is Windows 2000 (tools/genicons.py). Each set
here is the same list of slots (lib/iconload.c's slugs) filled from another
system's shell icons; a slot a set has no good match for is left out and the
Windows 2000 icon shows in its place.

Sources:
  winxp    icons/winxp/  -- the Windows XP resource dump (<module>_14_<id>.ico)
  win7     icons/win7/   -- the Windows 7 resource dump (<module>_<id>.ico)
  reactos  a checkout of dll/win32/shell32/res/icons, user32 resources and
           the applications' icons from github.com/reactos/reactos (GPL)
  win98    the Windows 98 PNGs of github.com/alexh/vintage-icons
           (static/icons_metadata.json, PNGs embedded as data URIs)

  tools/geniconsets.py [--reactos DIR] [--win98-json FILE]
"""
import base64, glob, io, json, os, struct, sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SETS = os.path.join(ROOT, "icons", "sets")

# ---------------------------------------------------------------- Windows XP
XP_DIR = os.path.join(ROOT, "icons", "winxp", "Windows Icons - ICO")
def xp(module, rid): return os.path.join(XP_DIR, "%s_14_%s.ico" % (module, rid))
S = lambda n: xp("shell32.dll", n)
XP = {
    "app": S(3), "folder": S(4), "folder_open": S(5), "file": S(1), "file_text": S(152),
    "mycomputer": S(16), "drive_hdd": S(9), "drive_floppy": S(7), "drive_cd": S(12),
    "notepad": xp("NOTEPAD.EXE", 2), "explorer": xp("explorer.exe", 101),
    "taskmgr": xp("taskmgr.exe", 107), "mydocs": S(235), "recycle": S(32),
    "recycle_full": S(33), "network": S(18), "programs": S(20), "documents": S(21),
    "settings": S(36), "search": S(23), "help": S(24), "run": S(25), "shutdown": S(28),
    "logoff": S(45), "controlpanel": S(22), "terminal": xp("cmd.exe", "IDI_APPICON"),
    "calc": xp("calc.exe", "SC"), "paint": xp("mspaint.exe", 2),
    "charmap": xp("charmap.exe", 111), "info": xp("user32.dll", 104),
    "warning": xp("user32.dll", 101), "question": xp("user32.dll", 102),
    "error": xp("user32.dll", 103), "accessories": S(4), "desktop": S(35),
    "winupdate": S(47), "file_bitmap": xp("shimgvw.dll", 3), "file_jpeg": xp("shimgvw.dll", 4),
    "file_gif": xp("shimgvw.dll", 5), "file_wave": S(225), "file_midi": S(225),
    "file_movie": S(224), "file_media": S(227), "file_zip": xp("zipfldr.dll", 101),
    "file_ini": S(151), "file_bat": S(153), "file_sys": S(154), "file_rtf": S(2),
    "file_font": S(155), "file_unknown": S(1), "link_overlay": S(30),
    "speaker": xp("sndvol32.exe", 300), "favorites": S(44), "fonts_folder": S(39),
}

# ---------------------------------------------------------------- Windows 7
W7_ALL = {os.path.basename(p): p for p in glob.glob(os.path.join(ROOT, "icons", "win7", "win7", "*", "*.ico"))}
def w7(name): return W7_ALL.get(name + ".ico", "")
I = lambda n: w7("imageres_%d" % n)
H = lambda n: w7("shell32_%s" % n)
W7 = {
    "app": I(15), "folder": I(3), "folder_open": I(5), "file": I(2), "file_text": I(102),
    "mycomputer": I(109), "drive_hdd": I(30), "drive_floppy": I(28), "drive_cd": I(61),
    "notepad": w7("notepad_2"), "explorer": w7("explorer_ICO_MYCOMPUTER"),
    "mydocs": I(1002), "recycle": I(54), "recycle_full": I(55), "network": I(25),
    "documents": H(16771), "search": H(23), "help": H(24), "run": H(25),
    "shutdown": H(27), "controlpanel": H(22), "terminal": w7("cmd_IDI_APPICON"),
    "paint": w7("mspaint_2"), "info": I(81), "warning": I(84), "question": I(99),
    "error": I(170), "accessories": I(3), "desktop": I(110), "winupdate": H(47),
    "file_bitmap": I(70), "file_jpeg": I(71), "file_gif": I(72), "file_wave": I(1026),
    "file_midi": I(1026), "file_movie": I(193), "file_media": w7("player"),
    "file_ini": H(151), "file_bat": H(153), "file_sys": H(154), "file_rtf": w7("wordpad_128"),
    "file_font": I(124), "file_unknown": I(2), "speaker": I(176), "favorites": I(1024),
    "fonts_folder": I(77), "settings": H(270),
}

# ---------------------------------------------------------------- ReactOS
def reactos_map(d):
    sh = lambda n: os.path.join(d, "shell32", "%d.ico" % n)
    u = lambda n: os.path.join(d, "user32", n + ".ico")
    a = lambda n: os.path.join(d, "apps", n + ".ico")
    ex = lambda n: os.path.join(d, "explorer", "%d.ico" % n)
    return {
        "app": sh(3), "folder": sh(4), "folder_open": sh(5), "file": sh(1),
        "file_text": sh(152), "mycomputer": sh(16), "drive_hdd": sh(9),
        "drive_floppy": sh(7), "drive_cd": sh(12), "notepad": a("notepad_notepad"),
        "explorer": ex(100), "taskmgr": a("taskmgr_taskmgr"), "mydocs": sh(235),
        "recycle": sh(32), "recycle_full": sh(33), "network": sh(18), "programs": sh(20),
        "documents": sh(21), "settings": sh(36), "search": sh(23), "help": sh(24),
        "run": sh(25), "shutdown": sh(28), "logoff": sh(45), "controlpanel": sh(22),
        "terminal": a("cmd_terminal"), "calc": a("calc_calc"), "charmap": a("charmap_charmap"),
        "info": u("oic_note"), "warning": u("oic_bang"), "question": u("oic_ques"),
        "error": u("oic_hand"), "accessories": sh(4), "desktop": sh(35), "winupdate": sh(47),
        "file_wave": sh(225), "file_midi": sh(225), "file_movie": sh(224), "file_media": sh(227),
        "file_zip": a("zipfldr_zipfldr"), "file_ini": sh(151), "file_bat": sh(153),
        "file_sys": sh(154), "file_rtf": sh(2), "file_font": sh(155), "file_unknown": sh(1),
        "link_overlay": sh(30), "speaker": a("mmsys_speaker"), "favorites": sh(44),
        "fonts_folder": sh(39),
    }

# ---------------------------------------------------------------- Windows 98
# Candidate base names in the vintage-icons catalogue, first match wins; the
# catalogue's _0.._5 suffixes are sizes and depths in no fixed order, so the
# 32 and 16 pixel variants are picked by looking at them.
W98 = {
    "app": ["executable"], "folder": ["directory_closed"], "folder_open": ["directory_open", "directory_open_cool"],
    "file": ["document"], "file_text": ["notepad_file"], "mycomputer": ["computer"],
    "drive_hdd": ["hard_disk_drive"], "drive_floppy": ["floppy_drive_3_5"], "drive_cd": ["cd_drive"],
    "notepad": ["notepad"], "explorer": ["computer_explorer"], "taskmgr": ["computer_taskmgr"],
    "mydocs": ["directory_open_file_mydocs"], "recycle": ["recycle_bin_empty"], "recycle_full": ["recycle_bin_full"],
    "network": ["network_neighborhood", "network_normal", "directory_network_conn"],
    "programs": ["directory_program_group", "program_group", "file_program_group"],
    "documents": ["directory_documents", "document_history", "documents"],
    "settings": ["settings_gear"], "search": ["search_directory", "computer_search"],
    "help": ["help_book_cool", "help_book_big"], "run": ["run", "application_hourglass"],
    "shutdown": ["shut_down_normal", "shut_down_cool"], "logoff": ["key_", "log_off"],
    "controlpanel": ["directory_control_panel"], "terminal": ["ms_dos"], "calc": ["calculator"],
    "paint": ["paint_old", "paint_file"], "charmap": ["charmap"], "info": ["msg_information"],
    "warning": ["msg_warning"], "question": ["msg_question"], "error": ["msg_error"],
    "accessories": ["directory_closed"], "desktop": ["desktop"], "winupdate": ["windows_update_old", "windows_update_large"],
    "file_bitmap": ["image_bmp", "paint_file", "file_bmp"], "file_jpeg": ["imagjpeg", "image_old_jpeg"],
    "file_gif": ["imaggif", "image_old_gif"], "file_html": ["html"], "file_wave": ["loudspeaker_wave"],
    "file_midi": ["midi_bl", "midi_gr"], "file_movie": ["video", "active_movie"], "file_media": ["media_player_file"],
    "file_zip": ["zip", "winzip", "file_zip"], "file_ini": ["settings_file", "file_ini", "ini"],
    "file_bat": ["executable_script", "batch"], "file_sys": ["system_file", "file_system"],
    "file_rtf": ["write_file", "wordpad_file", "write_wordpad"], "file_font": ["font_true_type", "true_type", "font_bitmap"],
    "file_unknown": ["file_unknown", "document_unknown", "file_question"], "speaker": ["loudspeaker_rays"],
    "favorites": ["directory_favorites"], "fonts_folder": ["directory_fonts"],
}

def write_ico(path, images):
    """An ICO holding the given RGBA images as 32-bit DIBs with AND masks."""
    entries, blobs = [], []
    off = 6 + 16 * len(images)
    for im in images:
        im = im.convert("RGBA"); w, h = im.size
        px = im.load()
        xor = bytearray(); mask = bytearray()
        stride = ((w + 31) // 32) * 4
        for y in range(h - 1, -1, -1):
            row = 0
            for x in range(w):
                r, g, b, a = px[x, y]
                xor += bytes((b, g, r, a))
                if a < 128: row |= 1 << (31 - x) if w <= 32 else 0
            mask += struct.pack(">I", row) if w <= 32 else bytes(stride)
        hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
        blob = hdr + bytes(xor) + bytes(mask)
        entries.append(struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(blob), off))
        blobs.append(blob); off += len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images))); f.write(b"".join(entries)); f.write(b"".join(blobs))

def frames_16_32(path):
    """The 16 and 32 pixel images of an ICO, scaled from the nearest size
    when one is missing (Windows 7's carry 256-pixel PNGs; only these two
    are drawn, and the loader reads DIB entries)."""
    im = Image.open(path)
    sizes = sorted(im.ico.sizes(), key=lambda s: s[0])
    def pick(n):
        exact = [s for s in sizes if s == (n, n)]
        if exact: return im.ico.getimage(exact[0]).convert("RGBA")
        bigger = [s for s in sizes if s[0] > n] or sizes
        src = im.ico.getimage(bigger[0] if bigger[0][0] > n else bigger[-1]).convert("RGBA")
        return src.resize((n, n), Image.LANCZOS)
    return pick(16), pick(32)

def build_ico_set(name, mapping):
    out = os.path.join(SETS, name); os.makedirs(out, exist_ok=True)
    got, missing = 0, []
    for slug, src in sorted(mapping.items()):
        if src and os.path.exists(src):
            small, big = frames_16_32(src)
            write_ico(os.path.join(out, slug + ".ico"), [small, big])
            got += 1
        else: missing.append(slug)
    print("%-8s %d icons%s" % (name, got, ("; no source for: " + " ".join(missing)) if missing else ""))

def build_win98(jsonfile):
    cat = json.load(open(jsonfile))["icons"]
    by = {}
    for x in cat: by.setdefault(x["name"], x)
    def variants(base):
        # base_0.. base_5, but not base_2_0 (a different icon)
        out = []
        for n, x in by.items():
            if n.startswith(base + "_") and n[len(base) + 1:].isdigit(): out.append(x)
        return out
    def decode(x): return Image.open(io.BytesIO(base64.b64decode(x["src"].split(",", 1)[1])))
    out = os.path.join(SETS, "win98"); os.makedirs(out, exist_ok=True)
    got, missing = 0, []
    for slug, cands in sorted(W98.items()):
        done = False
        for base in cands:
            vs = variants(base)
            if not vs: continue
            imgs = [decode(v).convert("RGBA") for v in vs]
            big = next((i for i in imgs if i.size == (32, 32)), None)
            small = next((i for i in imgs if i.size == (16, 16)), None)
            if not big and not small: continue
            if not big: big = small.resize((32, 32), Image.NEAREST)
            if not small: small = big.resize((16, 16), Image.LANCZOS)
            write_ico(os.path.join(out, slug + ".ico"), [small, big]); got += 1; done = True; break
        if not done: missing.append(slug)
    print("%-8s %d icons%s" % ("win98", got, ("; no source for: " + " ".join(missing)) if missing else ""))

if __name__ == "__main__":
    args = sys.argv[1:]
    reactos = args[args.index("--reactos") + 1] if "--reactos" in args else None
    w98 = args[args.index("--win98-json") + 1] if "--win98-json" in args else None
    if os.path.isdir(XP_DIR): build_ico_set("winxp", XP)
    else: print("winxp: icons/winxp dump not present, skipped")
    if W7_ALL: build_ico_set("win7", W7)
    else: print("win7: icons/win7 dump not present, skipped")
    if reactos: build_ico_set("reactos", reactos_map(reactos))
    if w98: build_win98(w98)
