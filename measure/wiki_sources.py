"""Pinned public Wikipedia inputs and a serial, cached Wikimedia API client."""

import gzip
import hashlib
import http.client
import io
import json
import re
import sqlite3
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import UTC, datetime
from pathlib import Path

USER_AGENT = (
    "AmbergrisPublicStudy/1.0 (https://www.mediawiki.org/wiki/API:Etiquette) Python-urllib/3.13"
)
API = "https://en.wikipedia.org/w/api.php"
CATEGORY_SOURCE = {
    "snapshot": "20261101",
    "url": "https://dumps.wikimedia.org/enwiki/20261101/enwiki-20261101-category.sql.gz",
    "bytes": 35130311,
    "sha1": "4171324b173a1518ed4d8f05924781e36f301bd5",
}
ROW = re.compile(r"^\((\d+),'((?:[^'\\]|\\.)*)',(-?\d+),(-?\d+),(-?\d+)\)[,;]$")
ESCAPES = {"0": "\0", "b": "\b", "n": "\n", "r": "\r", "t": "\t", "Z": "\x1a"}


def write_json(path, data):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".partial")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(path)


def sha256(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def download(url, path, *, expected_bytes=None, sha1=None):
    path = Path(path)
    if path.exists() and (expected_bytes is None or path.stat().st_size == expected_bytes):
        with path.open("rb") as source:
            if sha1 is None or hashlib.file_digest(source, "sha1").hexdigest() == sha1:
                return
    temporary = path.with_suffix(path.suffix + ".partial")
    resume = path.with_suffix(path.suffix + ".download.json")
    for attempt in range(8):
        metadata = json.loads(resume.read_text()) if resume.exists() else {}
        validator = metadata.get("etag") or metadata.get("last_modified")
        offset = temporary.stat().st_size if temporary.exists() else 0
        headers = {"User-Agent": USER_AGENT}
        if offset and metadata.get("url") == url and validator:
            headers.update({"Range": f"bytes={offset}-", "If-Range": validator})
        try:
            request = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(request, timeout=90) as source:
                append = source.status == 206 and "Range" in headers
                if append and not source.headers.get("Content-Range", "").startswith(
                    f"bytes {offset}-"
                ):
                    raise ValueError("Dump resume returned a different byte range")
                write_json(
                    resume,
                    {
                        "url": url,
                        "etag": source.headers.get("ETag"),
                        "last_modified": source.headers.get("Last-Modified"),
                        "expected_bytes": expected_bytes,
                    },
                )
                with temporary.open("ab" if append else "wb") as output:
                    while chunk := source.read(1024 * 1024):
                        output.write(chunk)
            if expected_bytes is None or temporary.stat().st_size == expected_bytes:
                break
            if temporary.stat().st_size > expected_bytes:
                raise ValueError("Public dump exceeds pinned size: " + url)
        except urllib.error.HTTPError as error:
            write_json(
                path.with_suffix(path.suffix + ".error.json"),
                {
                    "url": url,
                    "status": error.code,
                    "headers": dict(error.headers),
                    "retrieved": datetime.now(UTC).isoformat(),
                    "body": error.read().decode(errors="replace"),
                },
            )
            if error.code not in (429, 503):
                raise
            pause = max(float(error.headers.get("Retry-After", 0)), 2 ** (attempt + 1))
            while pause > 0:
                time.sleep(min(60, pause))
                pause -= 60
        except (urllib.error.URLError, TimeoutError, http.client.IncompleteRead):
            if attempt == 7:
                raise
            time.sleep(min(60, 2 ** (attempt + 1)))
    else:
        raise RuntimeError("Public dump retries exhausted: " + url)
    with temporary.open("rb") as source:
        if sha1 and hashlib.file_digest(source, "sha1").hexdigest() != sha1:
            raise ValueError("Public dump SHA-1 mismatch: " + url)
    temporary.replace(path)


def category_rows(path):
    """Parse the actual MariaDB dump's one-tuple-per-line category table."""
    active = False
    with gzip.open(path, "rt", encoding="utf-8", errors="surrogateescape") as source:
        for line in source:
            line = line.rstrip("\n")
            if line == "INSERT INTO `category` VALUES":
                active = True
                continue
            if not active:
                continue
            match = ROW.fullmatch(line)
            if not match:
                raise ValueError("Unexpected pinned category SQL row: " + line[:120])
            identity, title, pages, subcats, files = match.groups()
            title = re.sub(r"\\(.)", lambda m: ESCAPES.get(m[1], m[1]), title)
            yield (
                int(identity),
                title.encode("utf-8", errors="surrogateescape"),
                int(pages),
                int(subcats),
                int(files),
            )
            if line.endswith(";"):
                active = False


def category_universe(directory):
    """A complete category-table universe; not an enumeration of eligible trees."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / "enwiki-20261101-category.sql.gz"
    download(
        CATEGORY_SOURCE["url"],
        archive,
        expected_bytes=CATEGORY_SOURCE["bytes"],
        sha1=CATEGORY_SOURCE["sha1"],
    )
    path = directory / "category-universe.sqlite"
    metadata = directory / "category-universe.json"
    if path.exists() and metadata.exists():
        return path, json.loads(metadata.read_text())
    temporary = path.with_suffix(".partial")
    temporary.unlink(missing_ok=True)
    db = sqlite3.connect(temporary)
    db.execute(
        "CREATE TABLE original(id INTEGER PRIMARY KEY,title TEXT UNIQUE,pages,subcats,files)"
    )
    db.executemany("INSERT INTO original VALUES(?,?,?,?,?)", category_rows(archive))
    db.execute("CREATE TABLE categories(n INTEGER PRIMARY KEY,id,title,pages,subcats,files)")
    db.execute(
        "INSERT INTO categories SELECT row_number() OVER(ORDER BY title COLLATE BINARY),"
        "id,title,pages,subcats,files FROM original"
    )
    count = db.execute("SELECT COUNT(*) FROM categories").fetchone()[0]
    db.execute("DROP TABLE original")
    db.commit()
    db.execute("VACUUM")
    db.close()
    temporary.replace(path)
    record = {
        **CATEGORY_SOURCE,
        "sha256": sha256(archive),
        "categories": count,
        "ordering": "category title UTF-8/BINARY ascending",
        "sqlite_sha256": sha256(path),
    }
    write_json(metadata, record)
    return path, record


class PublicAPI:
    """One request at a time, <=171/minute; exact bodies and URL/status proofs cached."""

    def __init__(self, directory, *, interval=0.35):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.interval, self.last = interval, 0.0
        self.requests, self.seconds, self.hits = 0, 0.0, 0
        self.connections = {}

    def _fetch(self, url):
        parsed = urllib.parse.urlsplit(url)
        key = (parsed.scheme, parsed.netloc)
        path = urllib.parse.urlunsplit(("", "", parsed.path, parsed.query, ""))
        connection_type = (
            http.client.HTTPSConnection if parsed.scheme == "https" else http.client.HTTPConnection
        )
        for attempt in range(2):
            if key not in self.connections:
                self.connections[key] = connection_type(parsed.netloc, timeout=90)
            connection = self.connections[key]
            try:
                connection.request("GET", path, headers={"User-Agent": USER_AGENT})
                response = connection.getresponse()
                payload = response.read()
                headers = dict(response.headers)
                if response.status >= 400:
                    raise urllib.error.HTTPError(
                        url, response.status, response.reason, headers, io.BytesIO(payload)
                    )
                return payload, response.status, headers
            except (http.client.RemoteDisconnected, ConnectionResetError, BrokenPipeError):
                connection.close()
                del self.connections[key]
                if attempt:
                    raise

    def get(self, url):
        identity = hashlib.sha256(url.encode()).hexdigest()
        path = self.directory / (identity + ".json")
        proof_path = path.with_suffix(".proof.json")
        if path.exists() and proof_path.exists():
            self.hits += 1
            return json.loads(path.read_text()), identity
        for attempt in range(6):
            time.sleep(max(0, self.last + self.interval - time.monotonic()))
            started = time.monotonic()
            self.last = started
            try:
                payload, status, headers = self._fetch(url)
                data = json.loads(payload)
                if "error" in data:
                    if data["error"].get("code") in ("maxlag", "ratelimited"):
                        time.sleep(max(float(headers.get("Retry-After", 0)), 2 ** (attempt + 1)))
                        continue
                    raise ValueError(data["error"])
                temporary = path.with_suffix(".partial")
                temporary.write_bytes(payload)
                temporary.replace(path)
                elapsed = time.monotonic() - started
                write_json(
                    proof_path,
                    {
                        "url": url,
                        "status": status,
                        "headers": headers,
                        "retrieved": datetime.now(UTC).isoformat(),
                        "seconds": elapsed,
                        "sha256": hashlib.sha256(payload).hexdigest(),
                        "bytes": len(payload),
                    },
                )
                self.requests += 1
                self.seconds += elapsed
                return data, identity
            except urllib.error.HTTPError as error:
                body = error.read()
                write_json(
                    path.with_suffix(".error.json"),
                    {
                        "url": url,
                        "status": error.code,
                        "headers": dict(error.headers),
                        "retrieved": datetime.now(UTC).isoformat(),
                        "body": body.decode(errors="replace"),
                    },
                )
                if error.code not in (429, 503):
                    raise
                time.sleep(max(float(error.headers.get("Retry-After", 0)), 2 ** (attempt + 1)))
            except (TimeoutError, ConnectionError, http.client.IncompleteRead):
                for connection in self.connections.values():
                    connection.close()
                self.connections.clear()
                if attempt == 5:
                    raise
                time.sleep(2 ** (attempt + 1))
        raise RuntimeError("Public API retries exhausted: " + url)

    def query(self, **parameters):
        arguments = {
            "action": "query",
            "format": "json",
            "formatversion": 2,
            "maxlag": 5,
            **parameters,
        }
        return self.get(API + "?" + urllib.parse.urlencode(sorted(arguments.items())))
