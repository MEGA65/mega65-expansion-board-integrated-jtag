#!/usr/bin/env python3
"""
Download MEGA65 alternate-core archives and extract board-specific .cor/.bit files.

The public alt-core catalogue links to MEGA65 filehost IDs. The filehost may
require an authenticated browser session for some files; use --cookie or
--cookie-file if your direct run only sees the login page.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import http.cookiejar
import hashlib
import html
import io
import json
import os
import re
import shutil
import subprocess
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
FILEHOST_ROOTS = {"files.mega65.org", "www.files.mega65.org"}

FILEHOST_ID_RE = re.compile(
    r"https?://files\.mega65\.org/(?:html/main\.php)?\??[^\"' <>\n]*\bid=([0-9a-fA-F-]{36})",
    re.I,
)
HREF_RE = re.compile(r"""href=["']([^"']+)["']""", re.I)
HEADING_RE = re.compile(r"<h([23])[^>]*>(.*?)</h\1>", re.I | re.S)
TAG_RE = re.compile(r"<[^>]+>")
SIG_MAGIC = bytes([
    ord("M"), ord("6"), ord("5"), ord("J"), ord("T"), ord("A"), ord("G"), ord("-"),
    ord("S"), ord("I"), ord("G"), ord("B"), ord("L"), ord("O"), ord("C"), ord("K"),
    ord("-"), ord("V"), ord("1"), 0xA5, 0x65, 0x19, 0x83, 0x42,
    0x7C, 0xD1, 0x5E, 0x09, 0xBA, 0x6F, 0x34, 0xC8,
])
SIG_TRAILER_LEN = 256
ALLOWED_DOWNLOAD_EXTS = (".zip", ".bit", ".cor", ".core")
COR_MAGIC = b"MEGA65BITSTREAM0"


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


@dataclass
class DownloadCandidate:
    url: str
    filename: str | None = None
    source: str = "link"


def core_file_paths(root: Path) -> list[Path]:
    if not root.exists():
        return []
    return sorted(
        p for p in root.rglob("*")
        if p.is_file() and p.suffix.lower() in (".bit", ".cor", ".core", ".m65j")
    )


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


def request_with_data(url: str, fields: dict[str, str], cookie: str | None = None) -> urllib.request.Request:
    data = urllib.parse.urlencode(fields).encode("utf-8")
    headers = {
        "User-Agent": "mega65-altcore-fetch/1.0",
        "Accept": "*/*",
        "Content-Type": "application/x-www-form-urlencoded",
        "Content-Length": str(len(data)),
    }
    if cookie:
        headers["Cookie"] = cookie
    return urllib.request.Request(url, data=data, headers=headers, method="POST")


def fetch_bytes(url: str, cookie: str | None = None, timeout: float = 10.0) -> tuple[bytes, str, str]:
    with urllib.request.urlopen(request(url, cookie), timeout=timeout) as resp:
        data = resp.read()
        ctype = resp.headers.get("Content-Type", "")
        final_url = resp.geturl()
    return data, ctype, final_url


def post_json(url: str, fields: dict[str, str], cookie: str | None = None, timeout: float = 10.0):
    with urllib.request.urlopen(request_with_data(url, fields, cookie), timeout=timeout) as resp:
        data = resp.read()
    return json.loads(data.decode("utf-8", errors="replace"))


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


def board_label(board_id: int) -> str:
    if board_id == 3:
        return "r3"
    if board_id == 6:
        return "r6"
    return "unknown"


def expand_source_pages(pages: list[str] | None) -> list[str]:
    if not pages:
        return ALTCORE_PAGES
    expanded: list[str] = []
    for page in pages:
        parsed = urllib.parse.urlparse(page)
        if parsed.netloc.lower() in FILEHOST_ROOTS and parsed.path in ("", "/") and not parsed.query:
            expanded.extend(ALTCORE_PAGES)
        else:
            expanded.append(page)
    return expanded


def scrape_altcore_refs(cookie: str | None = None, pages: list[str] | None = None) -> list[CoreRef]:
    refs: list[CoreRef] = []
    seen: set[tuple[str, str]] = set()
    for page in expand_source_pages(pages):
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


def allowed_download_ext(value: str | None) -> str | None:
    if not value:
        return None
    parsed = urllib.parse.urlparse(html.unescape(value))
    path = urllib.parse.unquote(parsed.path or value)
    lower = path.lower()
    for ext in ALLOWED_DOWNLOAD_EXTS:
        if lower.endswith(ext):
            return ext
    return None


def safe_download_filename(name: str | None, fallback_url: str, default: str) -> str:
    candidate = name or Path(urllib.parse.unquote(urllib.parse.urlparse(fallback_url).path)).name or default
    candidate = Path(candidate).name
    candidate = re.sub(r"[^A-Za-z0-9._ -]+", "-", candidate).strip(" .-_")
    if not candidate:
        candidate = default
    ext = allowed_download_ext(candidate)
    if ext == ".core":
        candidate = f"{Path(candidate).stem}.cor"
    return candidate


def filehost_detail_candidates(file_id: str, cookie: str | None, verbose: bool = False) -> tuple[list[DownloadCandidate], str | None]:
    url = "https://files.mega65.org/php/readfiledetail.php"
    try:
        rows = post_json(url, {"fileid": file_id}, cookie)
    except Exception as exc:  # noqa: BLE001
        return [], f"filehost detail failed: {exc}"

    if not isinstance(rows, list) or not rows:
        return [], "filehost detail returned no rows"

    candidates: list[DownloadCandidate] = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        state = str(row.get("state", ""))
        if state and state != "OK":
            return [], f"filehost detail state={state}: {row.get('message', '')}"
        location = str(row.get("location") or "")
        filename = str(row.get("filename") or "")
        if not location:
            continue
        if not (allowed_download_ext(filename) or allowed_download_ext(location)):
            if verbose:
                print(f"  detail ignored non-core filename/location: {filename or location}", flush=True)
            continue
        candidates.append(DownloadCandidate(
            urllib.parse.urljoin("https://files.mega65.org/html/filedetail.php", html.unescape(location)),
            filename=filename or None,
            source="readfiledetail",
        ))
    return candidates, None if candidates else "filehost detail returned no .zip/.cor/.core/.bit location"


def candidate_archive_urls(file_id: str, page_html: str, page_url: str) -> list[DownloadCandidate]:
    urls: list[DownloadCandidate] = []
    for href in HREF_RE.findall(page_html):
        absolute = urllib.parse.urljoin(page_url, html.unescape(href))
        if allowed_download_ext(absolute):
            urls.append(DownloadCandidate(absolute, source="html-link"))

    deduped: list[DownloadCandidate] = []
    seen: set[str] = set()
    for candidate in urls:
        if candidate.url not in seen:
            seen.add(candidate.url)
            deduped.append(candidate)
    return deduped


def looks_like_zip(data: bytes, ctype: str, url: str) -> bool:
    if data.startswith(b"PK\x03\x04") or data.startswith(b"PK\x05\x06"):
        return True
    lower = (ctype + " " + url).lower()
    return "zip" in lower and not data.lstrip().lower().startswith(b"<!doctype html")


def looks_like_html(data: bytes, ctype: str) -> bool:
    return "html" in ctype.lower() or data[:512].lstrip().lower().startswith((b"<!doctype html", b"<html"))


def write_download_as_archive(cache_dir: Path,
                              ref: CoreRef,
                              data: bytes,
                              final_url: str,
                              filename_hint: str | None) -> Path:
    name = safe_name(ref.title) or ref.filehost_id
    if looks_like_zip(data, "", final_url) or allowed_download_ext(filename_hint) == ".zip" or allowed_download_ext(final_url) == ".zip":
        archive = cache_dir / f"{name}-{ref.filehost_id}.zip"
        archive.write_bytes(data)
        return archive

    member = safe_download_filename(filename_hint, final_url, f"{name}.cor")
    archive = cache_dir / f"{name}-{ref.filehost_id}-direct.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(member, data)
    return archive


def dedupe_candidates(candidates: list[DownloadCandidate]) -> list[DownloadCandidate]:
    deduped: list[DownloadCandidate] = []
    seen: set[str] = set()
    for candidate in candidates:
        if candidate.url in seen:
            continue
        seen.add(candidate.url)
        deduped.append(candidate)
    return deduped


def try_download_candidates(ref: CoreRef,
                            cache_dir: Path,
                            cookie: str | None,
                            candidates: list[DownloadCandidate],
                            verbose: bool = False) -> tuple[Path | None, str | None]:
    deduped = dedupe_candidates(candidates)
    for idx, candidate in enumerate(deduped, 1):
        try:
            if verbose:
                label = f" ({candidate.source})" if candidate.source else ""
                print(f"  download candidate {idx}/{len(deduped)}{label}: {candidate.url}", flush=True)
            data, ctype, final_url = fetch_bytes(candidate.url, cookie)
        except Exception as exc:  # noqa: BLE001
            if verbose:
                print(f"    failed: {exc}", flush=True)
            continue

        ext = allowed_download_ext(candidate.filename) or allowed_download_ext(final_url) or allowed_download_ext(candidate.url)
        if not ext:
            if verbose:
                print(f"    skipped: URL/filename has no .zip/.cor/.core/.bit extension", flush=True)
            continue
        if looks_like_html(data, ctype):
            if verbose:
                print(f"    not a core archive/file: {ctype or 'unknown content-type'} {final_url}", flush=True)
            continue
        archive = write_download_as_archive(cache_dir, ref, data, final_url, candidate.filename)
        return archive, final_url
    return None, None


def prefetch_filehost_details(refs: list[CoreRef],
                              cookie: str | None,
                              workers: int,
                              quiet: bool = False) -> dict[str, tuple[list[DownloadCandidate], str | None]]:
    unique_refs: list[CoreRef] = []
    seen: set[str] = set()
    for ref in refs:
        if ref.filehost_id in seen:
            continue
        seen.add(ref.filehost_id)
        unique_refs.append(ref)
    if not unique_refs:
        return {}
    workers = max(1, min(workers, len(unique_refs)))
    if workers <= 1:
        return {
            ref.filehost_id: filehost_detail_candidates(ref.filehost_id, cookie, verbose=False)
            for ref in unique_refs
        }

    if not quiet:
        print(f"Prefetching filehost details for {len(unique_refs)} refs with {workers} workers...", flush=True)
    out: dict[str, tuple[list[DownloadCandidate], str | None]] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {
            pool.submit(filehost_detail_candidates, ref.filehost_id, cookie, False): ref
            for ref in unique_refs
        }
        for fut in concurrent.futures.as_completed(futures):
            ref = futures[fut]
            try:
                out[ref.filehost_id] = fut.result()
            except Exception as exc:  # noqa: BLE001
                out[ref.filehost_id] = ([], f"filehost detail failed: {exc}")
    return out


def download_archive_for_ref(ref: CoreRef,
                             cache_dir: Path,
                             cookie: str | None,
                             verbose: bool = False,
                             detail: tuple[list[DownloadCandidate], str | None] | None = None) -> tuple[Path | None, str | None, str]:
    page_url = f"https://files.mega65.org/?id={ref.filehost_id}"
    if detail is None:
        candidates, detail_status = filehost_detail_candidates(ref.filehost_id, cookie, verbose=verbose)
    else:
        candidates, detail_status = detail
    archive, final_url = try_download_candidates(ref, cache_dir, cookie, candidates, verbose=verbose)
    if archive:
        return archive, final_url, "downloaded"

    page_html = ""
    try:
        if verbose:
            print(f"  catalogue fallback: {page_url}", flush=True)
        page_html = fetch_text(page_url, cookie)
    except Exception as exc:  # noqa: BLE001
        return None, None, detail_status or f"filehost page failed: {exc}"

    archive, final_url = try_download_candidates(
        ref,
        cache_dir,
        cookie,
        candidate_archive_urls(ref.filehost_id, page_html, page_url),
        verbose=verbose,
    )
    if archive:
        return archive, final_url, "downloaded"

    if "Username or email address" in page_html or "Enter the code" in page_html:
        return None, None, "filehost login/code required; set filehost_cookie or filehost_user/password in .m65j.config"
    return None, None, detail_status or "no downloadable .zip/.cor/.core/.bit link found"


def safe_name(name: str) -> str:
    name = name.strip().lower()
    name = re.sub(r"[^a-z0-9._-]+", "-", name)
    name = re.sub(r"-+", "-", name).strip("-._")
    return name[:80]


def canonical_base_name(title: str, member_name: str) -> str:
    candidates = [title, Path(member_name.split("::")[-1]).stem]
    for candidate in candidates:
        name = safe_name(candidate)
        if not name or name == "unknown-core":
            continue
        name = re.sub(r"[._]+", "-", name)
        name = re.sub(r"(?i)(^|-)(mega65)?r[36]($|-)", "-", name)
        name = re.sub(r"(?i)(^|-)(rev|revision)-?[36]($|-)", "-", name)
        name = re.sub(r"(?i)(^|-)(stable|unstable|nightly|release|released|latest)($|-)", "-", name)
        name = re.sub(r"(?i)(^|-)(build|built|b|rel)-?\d+[a-z0-9-]*($|-)", "-", name)
        name = re.sub(r"(?i)(^|-)v(?:er|ersion)?-?\d+(?:-\d+){0,4}[a-z]?($|-)", "-", name)
        name = re.sub(r"(?i)(^|-)\d+(?:-\d+){1,4}($|-)", "-", name)
        name = re.sub(r"(?i)(^|-)20\d{2}-?[01]\d-?[0-3]\d($|-)", "-", name)
        name = re.sub(r"-+", "-", name).strip("-._")
        if name:
            return name[:80]
    return "core"


def strip_signature_trailer(data: bytes) -> bytes:
    if len(data) >= SIG_TRAILER_LEN and data[-SIG_TRAILER_LEN:-SIG_TRAILER_LEN + len(SIG_MAGIC)] == SIG_MAGIC:
        return data[:-SIG_TRAILER_LEN]
    return data


def canonical_core_filename(title: str, board: str, member_name: str) -> str:
    ext = Path(member_name.split("::")[-1]).suffix.lower()
    if ext == ".core":
        ext = ".cor"
    if ext not in (".bit", ".cor"):
        ext = ".cor"
    base = canonical_base_name(title, member_name)
    board_suffix = f"-{board.lower()}"
    if not base.endswith(board_suffix):
        base += board_suffix
    return f"{base}{ext}"


def board_hint_present(text: str, board: str) -> bool:
    return bool(
        re.search(rf"(^|[^a-z0-9]){board}([^a-z0-9]|$)|rev(?:ision)?[_ -]*{board[-1]}", text) or
        f"mega65{board}" in text
    )


def fixed_header_string(data: bytes, off: int, length: int = 32) -> str:
    raw = data[off:off + length].split(b"\x00", 1)[0]
    return raw.decode("ascii", errors="replace").strip()


def cor_header_info(data: bytes) -> tuple[int, str, str, str]:
    if len(data) < 0x71 or data[:16] != COR_MAGIC:
        return 0, "", "", ""
    title = fixed_header_string(data, 0x10)
    version = fixed_header_string(data, 0x30)
    model = fixed_header_string(data, 0x50)
    return data[0x70], title, version, model


def core_board_from_data(path: str, data: bytes) -> tuple[int, str]:
    ext = Path(path.split("::")[-1]).suffix.lower()
    if ext not in (".cor", ".core"):
        return 0, ""
    model_id, _title, _version, model = cor_header_info(data)
    if model_id in (3, 6):
        return model_id, model
    lower_model = model.lower()
    if board_hint_present(lower_model, "r3") and not board_hint_present(lower_model, "r6"):
        return 3, model
    if board_hint_present(lower_model, "r6") and not board_hint_present(lower_model, "r3"):
        return 6, model
    return 0, model


def core_score(path: str, data: bytes, board: str, single: bool) -> int:
    lower = path.lower()
    leaf = Path(path.split("::")[-1]).name.lower()
    other = "r6" if board == "r3" else "r3"
    wanted_id = 3 if board == "r3" else 6
    actual_id, _model = core_board_from_data(path, data)
    score = 0
    if single:
        score += 3

    if actual_id:
        score += 200 if actual_id == wanted_id else -200

    leaf_has_board = board_hint_present(leaf, board)
    leaf_has_other = board_hint_present(leaf, other)
    path_has_board = board_hint_present(lower, board)
    path_has_other = board_hint_present(lower, other)

    if leaf_has_board:
        score += 30
    elif path_has_board and not path_has_other:
        score += 12
    elif path_has_board:
        score += 4

    if leaf_has_other:
        score -= 80
    elif path_has_other and not path_has_board:
        score -= 40

    if "mega65" in lower:
        score += 2
    if lower.endswith(".cor"):
        score += 6
    if lower.endswith(".bit"):
        score += 1
    return score


def core_member_detail(path: str, data: bytes) -> str:
    actual_id, title, version, model = cor_header_info(data)
    parts: list[str] = []
    if title:
        parts.append(f"title={title}")
    if version:
        parts.append(f"version={version}")
    if actual_id:
        parts.append(f"cor_model={board_label(actual_id).upper()}")
    if model:
        parts.append(f"model={model}")
    return " ".join(parts)


def iter_core_members(zf: zipfile.ZipFile, prefix: str = ""):
    for info in zf.infolist():
        if info.is_dir():
            continue
        name = prefix + info.filename
        lower = info.filename.lower()
        if lower.endswith(".bit") or lower.endswith(".cor") or lower.endswith(".core"):
            yield name, zf.read(info)
        elif lower.endswith(".zip"):
            nested_data = zf.read(info)
            try:
                with zipfile.ZipFile(io.BytesIO(nested_data)) as nested:
                    yield from iter_core_members(nested, prefix=name + "::")
            except zipfile.BadZipFile:
                continue


def iter_archive_files(zf: zipfile.ZipFile, prefix: str = ""):
    for info in zf.infolist():
        if info.is_dir():
            continue
        name = prefix + info.filename
        yield name
        if info.filename.lower().endswith(".zip"):
            try:
                with zipfile.ZipFile(io.BytesIO(zf.read(info))) as nested:
                    yield from iter_archive_files(nested, prefix=name + "::")
            except zipfile.BadZipFile:
                continue


def format_archive_file_list(names: list[str], max_entries: int = 120) -> str:
    if not names:
        return "zip contains no regular files"
    shown = names[:max_entries]
    lines = ["zip contains:"]
    lines.extend(f"  {name}" for name in shown)
    if len(names) > len(shown):
        lines.append(f"  ... {len(names) - len(shown)} more")
    return "\n".join(lines)


def extract_board_core(archive: Path, out_dir: Path, title: str, board: str, overwrite: bool, preserve_filenames: bool) -> tuple[Path | None, str]:
    try:
        with zipfile.ZipFile(archive) as zf:
            cores = list(iter_core_members(zf))
            archive_files = list(iter_archive_files(zf))
    except zipfile.BadZipFile:
        return None, "bad zip"

    if not cores:
        return None, "no .cor/.bit files in zip\n" + format_archive_file_list(archive_files)

    scored = sorted(
        ((core_score(name, data, board, len(cores) == 1), name, data) for name, data in cores),
        reverse=True,
    )
    score, member_name, data = scored[0]
    if score < 0:
        return None, f"no {board.upper()} .cor/.bit candidate\n" + format_archive_file_list(archive_files)

    if preserve_filenames:
        base = safe_name(Path(member_name.split("::")[-1]).name)
        if not (base.endswith(".bit") or base.endswith(".cor")):
            base += ".cor"
        if not base or base in (".bit", ".cor"):
            base = canonical_core_filename(title, board, member_name)
    else:
        base = canonical_core_filename(title, board, member_name)
    dest = out_dir / base
    existed_before = dest.exists()
    if dest.exists():
        old_payload = strip_signature_trailer(dest.read_bytes())
        if old_payload == data:
            return dest, "unchanged"
        if not overwrite:
            return dest, "changed; use --overwrite to replace"
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    detail = core_member_detail(member_name, data)
    if detail:
        detail = f" ({detail})"
    return dest, f"{'updated' if existed_before else 'extracted'} {member_name}{detail}"


def bless_core(path: Path, board: str, args: argparse.Namespace) -> str:
    tool = Path(__file__).with_name("m65j.py")
    board_id = "6" if board == "r6" else "3" if board == "r3" else "0"
    cmd = [
        sys.executable,
        str(tool),
        "bless",
        "--board",
        board_id,
        "--name",
        path.name,
        "-o",
        str(path),
    ]
    if args.key:
        cmd.extend(["--key", args.key])
    if args.key_name:
        cmd.extend(["--key-name", args.key_name])
    if args.yes:
        cmd.append("--yes")
    if args.blank_filename:
        cmd.append("--blank-filename")
    cmd.append(str(path))

    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if p.stdout:
        print(p.stdout, end="")
    if p.stderr:
        print(p.stderr, end="", file=sys.stderr)
    if p.returncode != 0:
        return f"bless failed: exit {p.returncode}"
    return "blessed"


def bless_paths(paths: list[Path], board: str, args: argparse.Namespace) -> None:
    paths = sorted(dict.fromkeys(paths))
    if not paths:
        print(f"No {board.upper()} files to bless.")
        return
    for idx, path in enumerate(paths, 1):
        print(f"Blessing {idx}/{len(paths)}: {path}", flush=True)
        status = bless_core(path, board, args)
        print(f"BLESS {path}: {status}")


def write_hash_file(out_dir: Path, hash_file: str, board: str, channel: str, paths: list[Path] | None = None) -> Path | None:
    if not hash_file:
        return None

    rows: list[tuple[str, str]] = []
    for path in sorted(dict.fromkeys(paths if paths is not None else core_file_paths(out_dir))):
        data = strip_signature_trailer(path.read_bytes())
        rel = path.relative_to(out_dir).as_posix()
        rows.append((rel, hashlib.sha256(data).hexdigest()))

    body_lines = [f"{sha}  {rel}\n" for rel, sha in rows]
    aggregate = hashlib.sha256("".join(body_lines).encode("utf-8")).hexdigest()

    dest = Path(hash_file)
    if not dest.is_absolute():
        dest = out_dir / dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    with dest.open("w", encoding="utf-8") as f:
        f.write("# m65j core mirror hash list v1\n")
        f.write(f"# channel={channel}\n")
        f.write(f"# board={board.upper()}\n")
        f.write(f"# aggregate_sha256={aggregate}\n")
        f.write("# format: <sha256>  <relative-filename>\n")
        for line in body_lines:
            f.write(line)
    return dest


def parse_client_config() -> tuple[dict[str, str], Path | None]:
    for path in (Path(".m65j.config"), Path.home() / ".m65j.config"):
        try:
            text = path.read_text(encoding="utf-8")
        except FileNotFoundError:
            continue
        cfg: dict[str, str] = {}
        for raw in text.splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line or "=" not in line:
                continue
            key, value = line.split("=", 1)
            cfg[key.strip().lower()] = value.strip()
        return cfg, path
    return {}, None


def login_filehost(user: str, password: str, cfg_path: Path | None) -> str | None:
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    data = urllib.parse.urlencode({
        "logindata[username]": user,
        "logindata[password]": password,
    }).encode("utf-8")
    req = urllib.request.Request(
        "https://files.mega65.org/php/login.php",
        data=data,
        headers={
            "User-Agent": "mega65-altcore-fetch/1.0",
            "Accept": "application/json, text/javascript, */*; q=0.01",
            "Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
        },
        method="POST",
    )
    try:
        with opener.open(req, timeout=10.0) as resp:
            body = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        print(f"NOTE filehost login failed: {exc}", file=sys.stderr)
        return None

    try:
        result = json.loads(body)
    except json.JSONDecodeError:
        print("NOTE filehost login did not return JSON; use filehost_cookie instead.", file=sys.stderr)
        return None
    if result.get("state") != "OK":
        msg = result.get("message") or result.get("state") or "not accepted"
        print(f"NOTE filehost login failed: {msg}", file=sys.stderr)
        return None

    cookies = [f"{c.name}={c.value}" for c in jar]
    if not cookies:
        print("NOTE filehost login succeeded but returned no cookies; use filehost_cookie instead.", file=sys.stderr)
        return None
    where = f" from {cfg_path}" if cfg_path else ""
    print(f"Using files.mega65.org login credentials{where}.", file=sys.stderr)
    return "; ".join(cookies)


def load_cookie(args: argparse.Namespace) -> str | None:
    if args.cookie:
        return args.cookie.strip()
    if args.cookie_file:
        return Path(args.cookie_file).read_text(encoding="utf-8").strip()
    cfg, path = parse_client_config()
    cookie = cfg.get("filehost_cookie") or cfg.get("mega65_cookie") or cfg.get("files_mega65_cookie")
    if cookie:
        return cookie
    user = cfg.get("filehost_user") or cfg.get("mega65_user") or cfg.get("files_mega65_user")
    password = cfg.get("filehost_password") or cfg.get("mega65_password") or cfg.get("files_mega65_password")
    if user and password:
        return login_filehost(user, password, path)
    if user or password:
        print(f"NOTE {path} has incomplete filehost credentials; set both filehost_user and filehost_password.", file=sys.stderr)
    return None


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--board", choices=["r3", "r6", "all"], required=True, help="MEGA65 board revision to extract")
    ap.add_argument("--output", default="sdcard/cores", help="directory for extracted .cor/.bit files")
    ap.add_argument("--cache", default=".cache/altcores", help="directory for downloaded zip archives")
    ap.add_argument("--cookie", help="raw Cookie header for files.mega65.org if login is required")
    ap.add_argument("--cookie-file", help="file containing a raw Cookie header")
    ap.add_argument("--source-url", action="append", default=[],
                    help="alternate catalogue URL to scrape; files.mega65.org aliases the default alt-core pages; repeatable")
    ap.add_argument("--keep-zips", action="store_true", help="keep downloaded zip archives in --cache")
    ap.add_argument("--overwrite", action="store_true", help="overwrite existing extracted .bit files")
    ap.add_argument("--dry-run", action="store_true", help="only list discovered filehost IDs")
    ap.add_argument("--limit", type=int, default=0, help="limit number of discovered refs processed")
    ap.add_argument("--manifest", default="", help="write JSON result manifest")
    ap.add_argument("--channel", required=True, help="mirror channel label, e.g. stable, unstable, nightly")
    ap.add_argument("--bless", action="store_true", help="append m65j signed trailer to every extracted/offered core file")
    ap.add_argument("--key", help="explicit P-256 EC private key PEM for --bless")
    ap.add_argument("--key-name", default="default", help="named key under ~/.m65jtag/keys for --bless")
    ap.add_argument("--yes", action="store_true", help="create the selected signing key without prompting")
    ap.add_argument("--blank-filename", action="store_true", help="with --bless, do not bind signatures to filenames")
    ap.add_argument("--hash-file", default=None, help="write per-file hash list; default is <channel>-rX.sha256 in output dir")
    ap.add_argument("--no-hash-file", action="store_true", help="do not write a channel hash list")
    ap.add_argument("--preserve-filenames", action="store_true", help="use archive member filenames instead of canonical <title>-<board> names")
    ap.add_argument("--quiet", action="store_true", help="suppress progress chatter")
    ap.add_argument("--detail-workers", type=int, default=8,
                    help="parallel filehost JSON detail fetches before serial downloads; 1 disables")
    args = ap.parse_args(argv)

    if args.board == "all" and args.hash_file:
        ap.error("--hash-file cannot be used with --board all; manifests are <channel>-r3.sha256 and <channel>-r6.sha256")

    cookie = load_cookie(args)
    refs = scrape_altcore_refs(cookie, args.source_url or None)
    if args.limit:
        refs = refs[: args.limit]

    board = args.board.lower()
    boards = ["r3", "r6"] if board == "all" else [board]
    wanted = [
        ref for ref in refs
        if board == "all" or ref.board_hint in ("unknown", "both", board)
    ]

    board_label_text = "R3/R6" if board == "all" else board.upper()
    print(f"Discovered {len(refs)} filehost refs, {len(wanted)} applicable to {board_label_text}.")
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
    manifest_paths: dict[str, list[Path]] = {b: [] for b in boards}
    detail_cache = prefetch_filehost_details(wanted, cookie, args.detail_workers, quiet=args.quiet)
    try:
        total = len(wanted)
        for idx, ref in enumerate(wanted, 1):
            if not args.quiet:
                print(f"\n[{idx}/{total}] {ref.title} ({ref.filehost_id}, hint={ref.board_hint})", flush=True)
            archive, archive_url, status = download_archive_for_ref(
                ref,
                cache_dir,
                cookie,
                verbose=not args.quiet,
                detail=detail_cache.get(ref.filehost_id),
            )
            if not archive:
                print(f"SKIP {ref.title}: {status}")
                results.append(DownloadResult(ref.title, ref.filehost_id, archive_url, None, status))
                continue
            if not args.quiet:
                print(f"  archive: {archive}", flush=True)
            if args.keep_zips and archive.parent != cache_root:
                shutil.copy2(archive, cache_root / archive.name)
            for one_board in boards:
                if ref.board_hint not in ("unknown", "both", one_board):
                    continue
                dest, extract_status = extract_board_core(archive, out_dir, ref.title, one_board, args.overwrite, args.preserve_filenames)
                label = f"{ref.title} [{one_board.upper()}]" if board == "all" else ref.title
                print(f"{'OK' if dest else 'SKIP'} {label}: {extract_status}")
                if dest:
                    manifest_paths[one_board].append(dest)
                results.append(DownloadResult(label, ref.filehost_id, archive_url, str(dest) if dest else None, extract_status))
    finally:
        if tmp_ctx:
            tmp_ctx.cleanup()

    if args.manifest:
        Path(args.manifest).write_text(json.dumps([asdict(r) for r in results], indent=2), encoding="utf-8")
    if args.bless:
        for one_board in boards:
            bless_paths(manifest_paths[one_board], one_board, args)
    if not args.no_hash_file:
        for one_board in boards:
            hash_name = args.hash_file or f"{safe_name(args.channel) or 'stable'}-{one_board}.sha256"
            hash_path = write_hash_file(out_dir, hash_name, one_board, args.channel, manifest_paths[one_board])
            if hash_path:
                print(f"Wrote hash file: {hash_path}")
                if args.bless:
                    status = bless_core(hash_path, one_board, args)
                    print(f"BLESS {hash_path}: {status}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
