# Вызов из консоли (пример): python .\pack_ssdz.py . --out ShapeShiftDrums.sndlib

#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse, io, struct, sys, os, secrets
from pathlib import Path
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

MAGIC_SSD = b"SSD1"   # ваш исходный формат (плейн)
MAGIC_SSZ = b"SSDX"   # наш новый контейнер: зашифрованный SSD (без пароля)
VERSION    = 1
ALG_CHACHA20P1305 = 1

# === ФИКСИРОВАННЫЙ КЛЮЧ (32 байта) ===
# Замените на свой. Можно сгенерировать так:  python - <<<'import secrets; print(secrets.token_hex(32))'
KEY_HEX = "00112233445566778899aabbccddeefffedcba98765432100123456789abcdef"
KEY = bytes.fromhex(KEY_HEX)

SRC_EXTS = (".cpp",".cxx",".cc",".h",".hpp",".hh",".hxx",".c")

def iter_sources(root: Path):
    for p in root.rglob("*"):
        if p.is_file() and (p.suffix.lower() in SRC_EXTS):
            yield p

def norm_rel_path(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()

def build_plain_ssd(root_dir: Path, out_file: Path, verbose: bool) -> bytes:
    files = sorted(iter_sources(root_dir))
    files = [p for p in files if p.resolve() != out_file.resolve()]

    if not files:
        print("Ничего не найдено (ни .cpp, ни .h).", file=sys.stderr)
        sys.exit(1)

    if verbose:
        print(f"Найдено файлов: {len(files)}")
        for f in files:
            print(f"  + {f}")

    buf = io.BytesIO()
    buf.write(MAGIC_SSD)
    buf.write(struct.pack("<I", len(files)))  # count

    for i, path in enumerate(files, 1):
        name = norm_rel_path(path, root_dir).encode("utf-8")
        data = path.read_bytes()
        buf.write(struct.pack("<I", len(name)))     # name_len
        buf.write(struct.pack("<Q", len(data)))     # data_len
        buf.write(name)
        buf.write(data)
        if verbose:
            print(f"[{i:>4}/{len(files)}] {path}  ({len(data)} bytes)")

    return buf.getvalue()

def encrypt_ssd(plain_ssd: bytes) -> bytes:
    # Заголовок:
    # 0..3  : "SSDX"
    # 4..5  : version (LE)
    # 6..7  : alg=1 (ChaCha20-Poly1305 IETF)
    # 8..19 : nonce (12 bytes)
    # 20..27: ct_len (uint64 LE)
    # 28..  : ciphertext || 16-byte tag
    nonce = secrets.token_bytes(12)
    aead = ChaCha20Poly1305(KEY)
    aad  = b"SSDXv1"
    ct   = aead.encrypt(nonce, plain_ssd, aad)

    out = io.BytesIO()
    out.write(MAGIC_SSZ)
    out.write(struct.pack("<H", VERSION))
    out.write(struct.pack("<H", ALG_CHACHA20P1305))
    out.write(nonce)
    out.write(struct.pack("<Q", len(ct)))
    out.write(ct)
    return out.getvalue()

def decrypt_ssdz(ssdz: bytes) -> bytes:
    if len(ssdz) < 28:
        raise ValueError("короткий .ssdz")
    if ssdz[:4] != MAGIC_SSZ:
        raise ValueError("неверная сигнатура .ssdz")
    ver = struct.unpack("<H", ssdz[4:6])[0]
    alg = struct.unpack("<H", ssdz[6:8])[0]
    if ver != VERSION or alg != ALG_CHACHA20P1305:
        raise ValueError("неподдерживаемая версия/алгоритм")
    nonce = ssdz[8:20]
    (ct_len,) = struct.unpack("<Q", ssdz[20:28])
    if 28 + ct_len > len(ssdz):
        raise ValueError("обрезанные данные")
    ct = ssdz[28:28+ct_len]
    aead = ChaCha20Poly1305(KEY)
    aad  = b"SSDXv1"
    plain = aead.decrypt(nonce, ct, aad)  # бросит исключение при неверном ключе/повреждении
    if plain[:4] != MAGIC_SSD:
        raise ValueError("внутренний payload не SSD")
    return plain

def extract_ssd(plain_ssd: bytes, out_dir: Path, verbose: bool):
    rd = io.BytesIO(plain_ssd)
    if rd.read(4) != MAGIC_SSD:
        print("Ошибка: неверная сигнатура .ssd", file=sys.stderr); sys.exit(2)
    (count,) = struct.unpack("<I", rd.read(4))
    if verbose:
        print(f"Файлов в пакете: {count}")
    for i in range(count):
        raw = rd.read(12)
        if len(raw) < 12:
            print("Ошибка: повреждённый заголовок элемента", file=sys.stderr); sys.exit(3)
        name_len, data_len = struct.unpack("<IQ", raw)
        name = rd.read(name_len)
        if len(name) != name_len:
            print("Ошибка: повреждённое имя файла", file=sys.stderr); sys.exit(4)
        rel = name.decode("utf-8")
        data = rd.read(data_len)
        if len(data) != data_len:
            print("Ошибка: повреждённые данные файла", file=sys.stderr); sys.exit(5)
        out_path = out_dir.joinpath(rel)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(data)
        if verbose:
            print(f"[{i+1:>4}/{count}] -> {out_path}  ({data_len} bytes)")

def main():
    ap = argparse.ArgumentParser(
        description="Упаковывает .cpp/.h в один .ssdz (шифр) и умеет распаковывать обратно (без пароля).")
    ap.add_argument("path", nargs="?", default=".", help="корневая папка (по умолчанию текущая) ИЛИ .ssdz в режиме --extract")
    ap.add_argument("--out", "-o", default="Source.ssdz", help="имя выходного .ssdz файла при упаковке")
    ap.add_argument("--extract", "-x", action="store_true", help="режим распаковки .ssdz -> папка")
    ap.add_argument("--to", default="unpacked", help="папка для распаковки (в режиме --extract)")
    ap.add_argument("--quiet", "-q", action="store_true", help="меньше логов")
    args = ap.parse_args()

    if args.extract:
        ssdz_path = Path(args.path).resolve()
        data = ssdz_path.read_bytes()
        plain_ssd = decrypt_ssdz(data)
        out_dir = Path(args.to).resolve()
        extract_ssd(plain_ssd, out_dir, verbose=not args.quiet)
        print(f"OK: распаковано в {out_dir}")
    else:
        root = Path(args.path).resolve()
        out = Path(args.out).resolve()
        plain_ssd = build_plain_ssd(root, out, verbose=not args.quiet)
        ssdz = encrypt_ssd(plain_ssd)
        out.write_bytes(ssdz)
        print(f"OK: упаковано и зашифровано -> {out}")

if __name__ == "__main__":
    sys.exit(main())
