#!/usr/bin/env python3
"""
Download MEGA65 alternate-core archives and extract board-specific .cor/.bit files.

The public alt-core catalogue links to MEGA65 filehost IDs. The filehost may
require an authenticated browser session for some files; use --cookie or
--cookie-file if your direct run only sees the login page.
"""

from __future__ import annotations

import argparse
import html
import io
import json
import os
import re
import shutil
import sys
import tempfile
import urllib.parse
import urllib.request
import zipfile
from dataclasses import dataclass, asdict
from pathlib import Path


ALTCORE_PAGES = [
    "https://kugelblitz360.github.io/m65-altcores/computer-cores.html",
    "https://kugelblitz360.github.io/m65-altcores/arcade-cores.html",
    "https://kugelblitz360.github.io/m65-altcores/game-console-cores.html",
]

FILEHOST_ID_RE = re.compile(
    r"https?://files\.mega65\.org/(?:html/main\.php)?\??[^\"' <>\n]*\bid=([0-9a-fA-F-]{36})",
    re.I,
)
HREF_RE = re.compile(r"""href=["']([^"']+)["']""", re.I)
HEADING_RE = re.compile(r"<h([23])[^>]*>(.*?)</h\1>", re.I | re.S)
TAG_RE = re.compile(r"<[^>]+>")


@dataclass
class CoreRef:
    title: str
    filehost_id: str
    url: str
    source_page: str
    board_hint: str


@dataclass
class DownloadResult:
    title: str
    filehost_id: str
    archive_url: str | None
    extracted: str | None
    status: str


def clean_text(s: str) -> str:
    return " ".join(html.unescape(TAG_RE.sub(" ", s)).split())


def request(url: str, cookie: str | None = None, method: str = "GET") -> urllib.request.Request:
    headers = {
        "User-Agent": "mega65-altcore-fetch/1.0",
        "Accept": "*/*",
    }
    if cookie:
        headers["Cookie"] = cookie
    return urllib.request.Request(url, headers=headers, method=method)


def fetch_bytes(url: str, cookie: str | None = None, timeout: float = 30.0) -> tuple[bytes, str, str]:
    with urllib.request.urlopen(request(url, cookie), timeout=timeout) as resp:
        data = resp.read()
        ctype = resp.headers.get("Content-Type", "")
        final_url = resp.geturl()
    return data, ctype, final_url


def fetch_text(url: str, cookie: str | None = None) -> str:
    data, ctype, _ = fetch_bytes(url, cookie)
    charset = "utf-8"
    m = re.search(r"charset=([^;\s]+)", ctype, re.I)
    if m:
        charset = m.group(1)
    return data.decode(charset, errors="replace")


def nearest_heading(headings: list[tuple[int, str]], pos: int) -> str:
    title = "unknown-core"
    for hpos, text in headings:
        if hpos > pos:
            break
        title = text
    return title


def board_hint_from_context(context: str) -> str:
    text = clean_text(context).lower()
    has_r3 = bool(re.search(r"\br3\b|r3-only|r3 only|rev(?:ision)?\s*3", text))
    has_r6 = bool(re.search(r"\br6\b|r6-only|r6 only|rev(?:ision)?\s*6", text))
    if has_r3 and not has_r6:
        return "r3"
    if has_r6 and not has_r3:
        return "r6"
    if has_r3 and has_r6:
        return "both"
    return "unknown"


def scrape_altcore_refs(cookie: str | None = None) -> list[CoreRef]:
    refs: list[CoreRef] = []
    seen: set[tuple[str, str]] = set()
    for page in ALTCORE_PAGES:
        text = fetch_text(page, cookie)
        headings = [(m.start(), clean_text(m.group(2))) for m in HEADING_RE.finditer(text)]
        for m in FILEHOST_ID_RE.finditer(text):
            file_id = m.group(1).lower()
            href = m.group(0)
            title = nearest_heading(headings, m.start())
            context = text[max(0, m.start() - 400) : min(len(text), m.end() + 180)]
            hint = board_hint_from_context(context)
            key = (file_id, hint)
            if key in seen:
                continue
            seen.add(key)
            refs.append(CoreRef(title=title, filehost_id=file_id, url=href, source_page=page, board_hint=hint))
    return refs


def candidate_archive_urls(file_id: str, page_html: str, page_url: str) -> list[str]:
    urls: list[str] = []
    for href in HREF_RE.findall(page_html):
        absolute = urllib.parse.urljoin(page_url, html.unescape(href))
        lower = absolute.lower()
        if ".zip" in lower or "download" in lower or "file" in lower:
            urls.append(absolute)

    # Best-effort fallbacks for filehost deployments. The script verifies that
    # the response is actually a zip before accepting any of these.
    bases = [
        f"https://files.mega65.org/html/download.php?id={file_id}",
        f"https://files.mega65.org/html/filedownload.php?id={file_id}",
        f"https://files.mega65.org/php/download.php?id={file_id}",
        f"https://files.mega65.org/php/download_file.php?id={file_id}",
        f"https://files.mega65.org/html/main.php?download=1&id={file_id}",
    ]
    urls.extend(bases)

    deduped: list[str] = []
    seen: set[str] = set()
    for url in urls:
        if url not in seen:
            seen.add(url)
            deduped.append(url)
    return deduped


def looks_like_zip(data: bytes, ctype: str, url: str) -> bool:
    if data.startswith(b"PK\x03\x04") or data.startswith(b"PK\x05\x06"):
        return True
    lower = (ctype + " " + url).lower()
    return "zip" in lower and not data.lstrip().lower().startswith(b"<!doctype html")


def download_archive_for_ref(ref: CoreRef, cache_dir: Path, cookie: str | None) -> tuple[Path | None, str | None, str]:
    page_url = f"https://files.mega65.org/?id={ref.filehost_id}"
    try:
        page_html = fetch_text(page_url, cookie)
    except Exception as exc:  # noqa: BLE001
        return None, None, f"filehost page failed: {exc}"

    for url in candidate_archive_urls(ref.filehost_id, page_html, page_url):
        try:
            data, ctype, final_url = fetch_bytes(url, cookie)
        except Exception:
            continue
        if not looks_like_zip(data, ctype, final_url):
            continue
        name = safe_name(ref.title) or ref.filehost_id
        archive = cache_dir / f"{name}-{ref.filehost_id}.zip"
        archive.write_bytes(data)
        return archive, final_url, "downloaded"

    if "Username or email address" in page_html or "Enter the code" in page_html:
        return None, None, "filehost login/code required; pass --cookie or download manually"
    return None, None, "no downloadable zip link found"


def safe_name(name: str) -> str:
    name = name.strip().lower()
    name = re.sub(r"[^a-z0-9._-]+", "-", name)
    name = re.sub(r"-+", "-", name).strip("-._")
    return name[:80]


def core_score(path: str, board: str, single: bool) -> int:
    lower = path.lower()
    other = "r6" if board == "r3" else "r3"
    score = 0
    if single:
        score += 3
    if re.search(rf"(^|[^a-z0-9]){board}([^a-z0-9]|$)|rev(?:ision)?[_ -]*{board[-1]}", lower):
        score += 20
    if re.search(rf"(^|[^a-z0-9]){other}([^a-z0-9]|$)|rev(?:ision)?[_ -]*{other[-1]}", lower):
        score -= 50
    if "mega65" in lower:
        score += 2
    if lower.endswith(".cor"):
        score += 6
    if lower.endswith(".bit"):
        score += 1
    return score


def iter_core_members(zf: zipfile.ZipFile, prefix: str = ""):
    for info in zf.infolist():
        if info.is_dir():
            continue
        name = prefix + info.filename
        lower = info.filename.lower()
        if lower.endswith(".bit") or lower.endswith(".cor"):
            yield name, zf.read(info)
        elif lower.endswith(".zip"):
            nested_data = zf.read(info)
            try:
                with zipfile.ZipFile(io.BytesIO(nested_data)) as nested:
                    yield from iter_core_members(nested, prefix=name + "::")
            except zipfile.BadZipFile:
                continue


def extract_board_core(archive: Path, out_dir: Path, title: str, board: str, overwrite: bool) -> tuple[Path | None, str]:
    try:
        with zipfile.ZipFile(archive) as zf:
            cores = list(iter_core_members(zf))
    except zipfile.BadZipFile:
        return None, "bad zip"

    if not cores:
        return None, "no .cor/.bit files in zip"

    scored = sorted(
        ((core_score(name, board, len(cores) == 1), name, data) for name, data in cores),
        reverse=True,
    )
    score, member_name, data = scored[0]
    if score < 0:
        return None, f"no {board.upper()} .cor/.bit candidate"

    base = safe_name(Path(member_name.split("::")[-1]).name)
    if not (base.endswith(".bit") or base.endswith(".cor")):
        base += ".cor"
    if not base or base in (".bit", ".cor"):
        base = f"{safe_name(title)}-{board}.cor"
    dest = out_dir / base
    if dest.exists() and not overwrite:
        return dest, "exists"
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    return dest, f"extracted {member_name}"


def load_cookie(args: argparse.Namespace) -> str | None:
    if args.cookie:
        return args.cookie.strip()
    if args.cookie_file:
        return Path(args.cookie_file).read_text(encoding="utf-8").strip()
    return None


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--board", choices=["r3", "r6"], required=True, help="MEGA65 board revision to extract")
    ap.add_argument("--output", default="sdcard/cores", help="directory for extracted .cor/.bit files")
    ap.add_argument("--cache", default=".cache/altcores", help="directory for downloaded zip archives")
    ap.add_argument("--cookie", help="raw Cookie header for files.mega65.org if login is required")
    ap.add_argument("--cookie-file", help="file containing a raw Cookie header")
    ap.add_argument("--keep-zips", action="store_true", help="keep downloaded zip archives in --cache")
    ap.add_argument("--overwrite", action="store_true", help="overwrite existing extracted .bit files")
    ap.add_argument("--dry-run", action="store_true", help="only list discovered filehost IDs")
    ap.add_argument("--limit", type=int, default=0, help="limit number of discovered refs processed")
    ap.add_argument("--manifest", default="", help="write JSON result manifest")
    args = ap.parse_args(argv)

    cookie = load_cookie(args)
    refs = scrape_altcore_refs(cookie)
    if args.limit:
        refs = refs[: args.limit]

    board = args.board.lower()
    wanted = [
        ref for ref in refs
        if ref.board_hint in ("unknown", "both", board)
    ]

    print(f"Discovered {len(refs)} filehost refs, {len(wanted)} applicable to {board.upper()}.")
    for ref in wanted:
        print(f"{ref.filehost_id} {ref.board_hint:7} {ref.title}")
    if args.dry_run:
        return 0

    out_dir = Path(args.output)
    cache_root = Path(args.cache)
    cache_root.mkdir(parents=True, exist_ok=True)
    tmp_ctx = tempfile.TemporaryDirectory(prefix="altcores-") if not args.keep_zips else None
    cache_dir = Path(tmp_ctx.name) if tmp_ctx else cache_root
    cache_dir.mkdir(parents=True, exist_ok=True)

    results: list[DownloadResult] = []
    try:
        for ref in wanted:
            archive, archive_url, status = download_archive_for_ref(ref, cache_dir, cookie)
            if not archive:
                print(f"SKIP {ref.title}: {status}")
                results.append(DownloadResult(ref.title, ref.filehost_id, archive_url, None, status))
                continue
            dest, extract_status = extract_board_core(archive, out_dir, ref.title, board, args.overwrite)
            if args.keep_zips and archive.parent != cache_root:
                shutil.copy2(archive, cache_root / archive.name)
            print(f"{'OK' if dest else 'SKIP'} {ref.title}: {extract_status}")
            results.append(DownloadResult(ref.title, ref.filehost_id, archive_url, str(dest) if dest else None, extract_status))
    finally:
        if tmp_ctx:
            tmp_ctx.cleanup()

    if args.manifest:
        Path(args.manifest).write_text(json.dumps([asdict(r) for r in results], indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
