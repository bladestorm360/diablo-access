#!/usr/bin/env python3
"""Print the quest states stored in a single-player save.

A save (single_N.hsv) is an MPQ archive. Its "hero" and "game" files are PKWARE-imploded per
sector and then encoded with the Diablo codec. This reads them without the game.

    python tools/save_quests.py            # the most recently saved slot
    python tools/save_quests.py 2          # slot 2 (single_2.hsv)
    python tools/save_quests.py path.hsv   # any save file
    python tools/save_quests.py --list     # one line per slot: character, level, where
"""

from __future__ import annotations

import argparse
import datetime
import os
import struct
import sys
from pathlib import Path

M = 0xFFFFFFFF
SAVE_DIR = Path(os.environ.get("APPDATA", "")) / "diasurgical" / "devilution"
PASSWORD_SINGLE = "xrgyrkj1"  # pfile.cpp

# ---- MPQ ---------------------------------------------------------------------------------------

CRYPT = [0] * 0x500
_seed = 0x00100001
for _i1 in range(256):
    _i2 = _i1
    for _ in range(5):
        _seed = (_seed * 125 + 3) % 0x2AAAAB
        _t1 = (_seed & 0xFFFF) << 16
        _seed = (_seed * 125 + 3) % 0x2AAAAB
        CRYPT[_i2] = _t1 | (_seed & 0xFFFF)
        _i2 += 256


def mpq_hash(name: str, hash_type: int) -> int:
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in name.upper().replace("/", "\\").encode():
        s1 = (CRYPT[hash_type * 256 + ch] ^ (s1 + s2)) & M
        s2 = (ch + s1 + s2 + (s2 << 5) + 3) & M
    return s1


def mpq_decrypt(data: bytes, key: int) -> list[int]:
    words = list(struct.unpack(f"<{len(data) // 4}I", data))
    s = 0xEEEEEEEE
    for i, w in enumerate(words):
        s = (s + CRYPT[0x400 + (key & 0xFF)]) & M
        ch = w ^ ((key + s) & M)
        key = ((((~key) << 0x15) + 0x11111111) & M) | (key >> 0x0B)
        s = (ch + s + (s << 5) + 3) & M
        words[i] = ch
    return words


# ---- PKWARE DCL explode (port of Mark Adler's blast.c) ------------------------------------------

MAXBITS = 13


class Huffman:
    def __init__(self, rep: list[int]) -> None:
        lengths: list[int] = []
        for b in rep:
            lengths += [b & 15] * ((b >> 4) + 1)
        self.count = [0] * (MAXBITS + 1)
        for n in lengths:
            self.count[n] += 1
        offs = [0] * (MAXBITS + 1)
        for n in range(1, MAXBITS):
            offs[n + 1] = offs[n] + self.count[n]
        self.symbol = [0] * len(lengths)
        for sym, n in enumerate(lengths):
            if n:
                self.symbol[offs[n]] = sym
                offs[n] += 1


LITCODE = Huffman([
    11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8, 9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11,
    7, 9, 11, 12, 6, 7, 22, 5, 7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12, 8, 12, 5, 38, 5,
    38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27, 44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61,
    12, 45, 44, 173])
LENCODE = Huffman([2, 35, 36, 53, 38, 23])
DISTCODE = Huffman([2, 20, 53, 230, 247, 151, 248])
LEN_BASE = [3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264]
LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8]


def explode(data: bytes) -> bytes:
    pos = bitbuf = bitcnt = 0

    def bits(need: int) -> int:
        nonlocal pos, bitbuf, bitcnt
        val = bitbuf
        while bitcnt < need:
            val |= data[pos] << bitcnt
            pos += 1
            bitcnt += 8
        bitbuf = val >> need
        bitcnt -= need
        return val & ((1 << need) - 1)

    def decode(h: Huffman) -> int:
        code = first = index = 0
        for n in range(1, MAXBITS + 1):
            code |= bits(1) ^ 1  # codes are stored inverted
            count = h.count[n]
            if code - count < first:
                return h.symbol[index + (code - first)]
            index += count
            first = (first + count) << 1
            code <<= 1
        raise ValueError("invalid Huffman code")

    coded_literals = bits(8)
    dict_bits = bits(8)
    out = bytearray()
    while True:
        if bits(1):
            sym = decode(LENCODE)
            length = LEN_BASE[sym] + bits(LEN_EXTRA[sym])
            if length == 519:  # end code
                break
            shift = 2 if length == 2 else dict_bits
            dist = (decode(DISTCODE) << shift) + bits(shift) + 1
            for _ in range(length):
                out.append(out[-dist])
        else:
            out.append(decode(LITCODE) if coded_literals else bits(8))
    return bytes(out)


def read_archive_file(archive: bytes, name: str) -> bytes | None:
    _sig, _hsize, _fsize, _ver, factor, hoff, boff, hcnt, bcnt = struct.unpack("<4sIIHHIIII", archive[:32])
    sector = 512 << factor
    hashes = mpq_decrypt(archive[hoff:hoff + hcnt * 16], mpq_hash("(hash table)", 3))
    blocks = mpq_decrypt(archive[boff:boff + bcnt * 16], mpq_hash("(block table)", 3))
    hash_a, hash_b = mpq_hash(name, 1), mpq_hash(name, 2)
    for i in range(hcnt):
        if hashes[i * 4] != hash_a or hashes[i * 4 + 1] != hash_b:
            continue
        block = hashes[i * 4 + 3]
        offset, _packed, size, _flags = blocks[block * 4:block * 4 + 4]
        sectors = (size + sector - 1) // sector
        table = struct.unpack(f"<{sectors + 1}I", archive[offset:offset + 4 * (sectors + 1)])
        out = bytearray()
        for s in range(sectors):
            chunk = archive[offset + table[s]:offset + table[s + 1]]
            # MpqWriter stores a sector raw when imploding would not make it smaller.
            out += chunk if len(chunk) == min(sector, size - s * sector) else explode(chunk)
        return bytes(out)
    return None


# ---- Diablo codec (codec.cpp, sha.cpp) ---------------------------------------------------------

def _rotl(word: int, bits: int) -> int:
    # Diablo's "SHA1" sign-extends on the right shift.
    if word & 0x80000000:
        return ((0xFFFFFFFF << bits) | (word >> (32 - bits))) & M
    return ((word << bits) | (word >> (32 - bits))) & M


class DiabloSha:
    def __init__(self) -> None:
        self.state = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0]

    def calculate(self, block: list[int]) -> None:
        w = list(block) + [0] * 64
        for i in range(16, 80):
            w[i] = w[i - 16] ^ w[i - 14] ^ w[i - 8] ^ w[i - 3]
        a, b, c, d, e = self.state
        for i in range(80):
            if i < 20:
                f, k = (b & c) | ((~b) & d), 0x5A827999
            elif i < 40:
                f, k = b ^ c ^ d, 0x6ED9EBA1
            elif i < 60:
                f, k = (b & c) | (b & d) | (c & d), 0x8F1BBCDC
            else:
                f, k = b ^ c ^ d, 0xCA62C1D6
            temp = (_rotl(a, 5) + (f & M) + e + w[i] + k) & M
            e, d, c, b, a = d, c, _rotl(b, 30), a, temp
        self.state = [(x + y) & M for x, y in zip(self.state, (a, b, c, d, e))]


def codec_decode(data: bytes, password: str) -> bytes | None:
    pw_bytes = password.encode() + b"\0\0\0\0"
    pw = []
    j = 0
    for _ in range(16):
        if j >= len(password):
            j = 0
        pw.append(struct.unpack("<I", pw_bytes[j:j + 4])[0])
        j += 4
    sha = DiabloSha()
    sha.calculate(pw)
    key = [
        2908958655, 4146550480, 658981742, 1113311088, 3927878744, 679301322, 1760465731, 3305370375,
        2269115995, 3928541685, 580724401, 2607446661, 2233092279, 2416822349, 4106933702, 3046442503,
    ]
    context = DiabloSha()
    context.calculate([k ^ sha.state[(i + 3) % 5] for i, k in enumerate(key)])

    size = len(data) - 8
    if size <= 0 or size % 64 != 0:
        return None
    out = bytearray()
    for off in range(0, size, 64):
        block = [b ^ context.state[i % 5] for i, b in enumerate(struct.unpack("<16I", data[off:off + 64]))]
        context.calculate(block)
        out += struct.pack("<16I", *block)
    checksum, error, last_chunk = struct.unpack("<IBB", data[size:size + 6])
    if error != 0 or checksum != context.state[0]:
        return None
    return bytes(out[:size + last_chunk - 64])


# ---- save contents -----------------------------------------------------------------------------

QUEST_NAMES = [  # quest_id order, names from assets/txtdata/quests/questdat.tsv
    "The Magic Rock", "Black Mushroom", "Gharbad The Weak", "Zhar the Mad", "Lachdanan", "Diablo",
    "The Butcher", "Ogden's Sign", "Halls of the Blind", "Valor", "Anvil of Fury", "Warlord of Blood",
    "The Curse of King Leoric", "Poisoned Water Supply", "The Chamber of Bone", "Archbishop Lazarus",
    "Grave Matters", "Farmer's Orchard", "Little Girl", "Wandering Trader", "The Defiler", "Na-Krul",
    "Cornerstone of the World", "The Jersey's Jersey",
]
QUEST_STATES = {  # quest_state in quests.h
    0: "not available", 1: "not started", 2: "active", 3: "done",
    7: "hive tease 1", 8: "hive tease 2", 9: "hive active", 10: "hive done", 255: "invalid",
}
CLASSES = ["Warrior", "Rogue", "Sorcerer", "Monk", "Bard", "Barbarian"]
DUNGEONS = [(1, "Cathedral"), (5, "Catacombs"), (9, "Caves"), (13, "Hell"), (17, "Hive"), (21, "Crypt")]


def describe_level(setlevel: bool, level: int) -> str:
    if setlevel:
        return f"quest level {level}"
    if level == 0:
        return "town"
    first, name = max(d for d in DUNGEONS if d[0] <= level)
    return f"{name} {level - first + 1}"


class Save:
    def __init__(self, path: Path) -> None:
        self.path = path
        archive = path.read_bytes()
        hero = read_archive_file(archive, "hero")
        game = read_archive_file(archive, "game")
        self.hero = codec_decode(hero, PASSWORD_SINGLE) if hero else None
        self.game = codec_decode(game, PASSWORD_SINGLE) if game else None

    def character(self) -> str:
        if self.hero is None:
            return "unreadable hero"
        name = self.hero[16:48].split(b"\0", 1)[0].decode("latin-1")
        cls = self.hero[48]
        cls_name = CLASSES[cls] if cls < len(CLASSES) else f"class {cls}"
        return f"{name}, level {self.hero[53]} {cls_name}"

    def location(self) -> str:
        if self.game is None:
            return "no game in progress"
        setlevel = self.game[4] != 0
        level = struct.unpack(">I", self.game[9:13])[0]
        return describe_level(setlevel, level)

    def quests(self) -> list[tuple[int, int, int, int]] | None:
        """(state, var1, var2, log) per quest, found by scanning for the quest table.

        The player record before it has no fixed size, but every quest record repeats its own
        index twice (_qtype and _qidx), which pins the table down.
        """
        if self.game is None:
            return None
        hellfire = self.game[:4] in (b"HELF", b"SHLF")
        count, size = (24, 48) if hellfire else (16, 44)  # LoadQuest() in loadsave.cpp
        var_at, log_at = (20, 24) if hellfire else (15, 20)
        g = self.game
        for start in range(len(g) - count * size):
            if all(g[start + i * size + 1] == i and g[start + i * size + 13] == i for i in range(count)):
                result = []
                for i in range(count):
                    r = g[start + i * size:start + (i + 1) * size]
                    result.append((r[2], r[var_at], r[var_at + 1], struct.unpack("<I", r[log_at:log_at + 4])[0]))
                return result
        return None


def slot_path(slot: int) -> Path:
    return SAVE_DIR / f"single_{slot}.hsv"


def saved_at(path: Path) -> str:
    return datetime.datetime.fromtimestamp(path.stat().st_mtime).strftime("%Y-%m-%d %H:%M")


def main() -> int:
    parser = argparse.ArgumentParser(description="Print the quest states stored in a single-player save.")
    parser.add_argument("save", nargs="?", help="slot number or .hsv path (default: most recently saved slot)")
    parser.add_argument("--list", action="store_true", help="list every slot's character and location")
    args = parser.parse_args()

    if args.list:
        for path in sorted(SAVE_DIR.glob("single_*.hsv")):
            save = Save(path)
            print(f"Slot {path.stem.split('_')[1]}: {save.character()}, {save.location()}, saved {saved_at(path)}")
        return 0

    if args.save is None:
        slots = sorted(SAVE_DIR.glob("single_*.hsv"), key=lambda p: p.stat().st_mtime)
        if not slots:
            print(f"No saves in {SAVE_DIR}", file=sys.stderr)
            return 1
        path = slots[-1]
    elif args.save.isdigit():
        path = slot_path(int(args.save))
    else:
        path = Path(args.save)
    if not path.is_file():
        print(f"No save at {path}", file=sys.stderr)
        return 1

    save = Save(path)
    print(f"{path.name}: {save.character()}, {save.location()}, saved {saved_at(path)}")
    quests = save.quests()
    if quests is None:
        print("No quest table found (no game in progress, or the save did not decode).", file=sys.stderr)
        return 1
    for i, (state, var1, var2, log) in enumerate(quests):
        state_name = QUEST_STATES.get(state, f"state {state}")
        print(f"{i:2d} {QUEST_NAMES[i]}: {state_name} (value {state}), var1 {var1}, var2 {var2}"
              + (", in quest log" if log else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
