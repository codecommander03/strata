"""Bundles web/ into a single self-contained page for hosts that want one file.

index.html references style.css and app.js as siblings, which is the right shape
for a static deploy. Some hosts — and Claude's artifact runtime — prefer the
page to carry its own CSS and JS inline, with only the WebAssembly kept
separate because it cannot be inlined usefully at 250 KB.

    python scripts/build_artifact_page.py            -> web/bundled.html

The output has no <!doctype>, <html> or <body> wrapper: it is page *content*,
because the artifact runtime supplies that skeleton itself.
"""

import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")


def read(name):
    with io.open(os.path.join(WEB, name), encoding="utf-8") as f:
        return f.read()


def main():
    html = read("index.html")
    css = read("style.css")
    js = read("app.js")

    # Keep only what lives between <body> and </body>.
    match = re.search(r"<body>(.*)</body>", html, re.S)
    if not match:
        print("could not find <body> in index.html", file=sys.stderr)
        return 1
    body = match.group(1)

    # Drop both script tags from the body: app.js gets inlined below, and
    # strata.js is re-emitted in the right order (it must load before app.js
    # runs, because app.js calls createStrataModule).
    body = body.replace('<script src="app.js"></script>', "")
    body = body.replace('<script src="strata.js"></script>', "")
    body = body.strip()

    out = []
    out.append("<title>strata SQL playground</title>")
    out.append("<style>\n" + css.strip() + "\n</style>")
    out.append(body)
    out.append('<script src="strata.js"></script>')
    out.append("<script>\n" + js.strip() + "\n</script>")

    target = os.path.join(WEB, "bundled.html")
    with io.open(target, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n\n".join(out) + "\n")

    size = os.path.getsize(target)
    print("wrote %s (%d bytes)" % (target, size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
