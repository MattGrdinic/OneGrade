#!/usr/bin/env python3
"""Gather look-reference stills from film-grab.com, politely and reproducibly.

    ./fetch.py index                       # build/refresh the local film index (5 requests)
    ./fetch.py find "blade runner"         # look up slugs to put in a films.txt
    ./fetch.py get ../../training-data/looks/moody

WHAT IS TRACKED AND WHAT IS NOT
-------------------------------
The films.txt curation lists are committed; the downloaded stills are not. The list IS the
experiment -- deciding that these thirty films are "moody" is an editorial judgement and it is the
ground truth the targets get fitted to. Anyone with the list can reproduce the corpus, and nothing
copyrighted enters the repository.

ONE FRAME PER FILM, ON PURPOSE
------------------------------
The site's image sitemap publishes exactly one still per film, and that is the sampling design we
want anyway. Thirty frames from three films is not n=30: same DP, same grade, same scenes, so the
frames are strongly correlated and the spread that separability is measured against would come out
far too small. Thirty frames from thirty films is a real n=30. It is also one request per film
instead of thirty.

BEING A GOOD CITIZEN
--------------------
robots.txt (checked 2026-08-13) disallows only /wp-admin/, so the gallery is fair to crawl. This
still rate-limits, identifies itself with a contact address, honours Retry-After, backs off on
5xx, and skips anything already on disk so a re-run costs nothing. Requests are serial by design;
there is no parallel mode and adding one would be the wrong optimisation for a corpus this size.
"""
import argparse
import os
import re
import sys
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from datetime import datetime, timezone

UA = "Mozilla/5.0 (compatible; OneGrade-research/1.0; +mattgrdinic@yahoo.com)"
SITEMAP = "https://film-grab.com/image-sitemap-%d.xml"
SHARDS = 5
NS = {"sm": "http://www.sitemaps.org/schemas/sitemap/0.9",
      "image": "http://www.google.com/schemas/sitemap-image/1.1"}

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_INDEX = os.path.join(HERE, "..", "..", "training-data", "index.tsv")


def get(url, delay, tries=4):
    """One request, with the politeness the module docstring promises."""
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=45) as r:
                body = r.read()
            time.sleep(delay)
            return body
        except urllib.error.HTTPError as e:
            # 429/503 mean slow down, and the server may say by how much. Anything else is a
            # real error and retrying it is just noise on someone else's box.
            if e.code in (429, 503):
                wait = float(e.headers.get("Retry-After") or (5 * (attempt + 1)))
                sys.stderr.write("  %s -> %d, waiting %.0fs\n" % (url, e.code, wait))
                time.sleep(wait)
                continue
            if 500 <= e.code < 600:
                time.sleep(3 * (attempt + 1))
                continue
            # A 404 is an answer, not a failure of the run. The sitemap is a published index and
            # some of its entries point at images that have since moved; letting that propagate
            # aborted a whole batch partway through and lost the folders after it.
            sys.stderr.write("  %s -> %d, skipping\n" % (url, e.code))
            return None
        except (urllib.error.URLError, TimeoutError) as e:
            sys.stderr.write("  %s -> %s\n" % (url, e))
            time.sleep(3 * (attempt + 1))
    return None


def slug_of(page_url):
    m = re.search(r"film-grab\.com/\d{4}/\d{2}/\d{2}/([^/]+)/?$", page_url)
    return m.group(1) if m else page_url.rstrip("/").rsplit("/", 1)[-1]


def cmd_index(args):
    rows = []
    for i in range(1, SHARDS + 1):
        sys.stderr.write("sitemap shard %d/%d\n" % (i, SHARDS))
        body = get(SITEMAP % i, args.delay)
        if body is None:
            sys.stderr.write("error: could not fetch shard %d\n" % i)
            return 1
        root = ET.fromstring(body)
        for url in root.findall("sm:url", NS):
            loc = url.find("sm:loc", NS)
            img = url.find("image:image/image:loc", NS)
            if loc is None or img is None:
                continue
            rows.append((slug_of(loc.text), loc.text, img.text))

    os.makedirs(os.path.dirname(os.path.abspath(args.index)), exist_ok=True)
    with open(args.index, "w") as f:
        f.write("# slug\tpage\timage   (regenerable: ./fetch.py index)\n")
        for r in sorted(set(rows)):
            f.write("\t".join(r) + "\n")
    print("indexed %d films -> %s" % (len(rows), args.index))
    return 0


def load_index(path):
    if not os.path.exists(path):
        sys.stderr.write("no index at %s -- run: ./fetch.py index\n" % path)
        return None
    out = {}
    with open(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) == 3:
                out[parts[0]] = (parts[1], parts[2])
    return out


def cmd_find(args):
    idx = load_index(args.index)
    if idx is None:
        return 1
    needle = args.query.lower().replace(" ", "-")
    hits = [s for s in sorted(idx) if needle in s]
    for s in hits[: args.limit]:
        print("%-55s %s" % (s, idx[s][0]))
    print("\n%d match%s%s" % (len(hits), "" if len(hits) == 1 else "es",
                              "" if len(hits) <= args.limit else
                              " (showing %d, use --limit)" % args.limit))
    return 0


def cmd_get(args):
    idx = load_index(args.index)
    if idx is None:
        return 1
    look = os.path.abspath(args.look_dir)
    listing = os.path.join(look, "films.txt")
    if not os.path.exists(listing):
        sys.stderr.write("no %s\n" % listing)
        sys.stderr.write("Create it with one film slug per line (find them with ./fetch.py find).\n")
        return 1

    wanted = []
    with open(listing) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            wanted.append(slug_of(line) if "film-grab.com" in line else line)

    manifest = os.path.join(look, "manifest.tsv")
    have = set()
    if os.path.exists(manifest):
        with open(manifest) as f:
            for line in f:
                if not line.startswith("#") and line.strip():
                    have.add(line.split("\t", 1)[0])

    got = missing = skipped = 0
    with open(manifest, "a") as mf:
        if os.path.getsize(manifest) == 0:
            mf.write("# file\tslug\tpage\timage\tfetched\n")
        for slug in wanted:
            if slug not in idx:
                sys.stderr.write("  not on film-grab: %s\n" % slug)
                missing += 1
                continue
            page, img = idx[slug]
            ext = os.path.splitext(img)[1].lower() or ".jpg"
            name = slug + ext
            dest = os.path.join(look, name)
            if name in have or os.path.exists(dest):
                skipped += 1
                continue
            body = get(img, args.delay)
            if body is None:
                missing += 1
                continue
            with open(dest, "wb") as fh:
                fh.write(body)
            mf.write("%s\t%s\t%s\t%s\t%s\n" % (
                name, slug, page, img, datetime.now(timezone.utc).strftime("%Y-%m-%d")))
            mf.flush()
            got += 1
            print("  %s" % name)

    n = len([f for f in os.listdir(look) if f.lower().endswith((".jpg", ".jpeg", ".png"))])
    print("\n%s: +%d new, %d already had, %d unavailable -> %d stills total"
          % (os.path.basename(look), got, skipped, missing, n))
    # The separability report needs 8 per look per region, and a region only appears in the stills
    # that contain it -- so the count that matters is always larger than the count of files.
    if n < 30:
        print("NOTE: aim for ~30. Separability needs 8 stills per REGION, and any one still only\n"
              "      contributes to the regions it actually contains.")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--index", default=os.path.normpath(DEFAULT_INDEX))
    p.add_argument("--delay", type=float, default=1.5,
                   help="seconds between requests (default 1.5)")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("index", help="build/refresh the local film index")

    f = sub.add_parser("find", help="search the index for a film")
    f.add_argument("query")
    f.add_argument("--limit", type=int, default=40)

    g = sub.add_parser("get", help="download the stills listed in a look's films.txt")
    g.add_argument("look_dir")

    args = p.parse_args()
    return {"index": cmd_index, "find": cmd_find, "get": cmd_get}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
