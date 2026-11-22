#!/usr/bin/env python3
"""ghettomachine.bongcult.com: GhettoMachine's page (docs/how-it-works.html) with the emulator framed in it, and the emulator.

  python3 -I site/build.py site/public   (from the repository's top, after tools/build.py ../../app --web in third_party/fm1-base;
                                         site/publish.sh does this, commits it and pushes)

  python3 -I site/build.py --landing-only site/public   (refresh the page and branding only)

site/public/index.html   the homepage linking the User Guide and browser instrument
site/public/guide/       task-based guide, generated from site/guide HTML fragments
site/public/technical/   the original technical tour, with its embedded emulator
site/public/emu/         the emulator: app/build/emu (the page, app.wasm, the audio worklet, its parts)
site/public/_headers     the headers Cloudflare serves with it (wasm's type, caching)
"""
import html
import json
import re
import shutil
import sys
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = "https://github.com/siddholdenried-del/ghettomachine"
ICON = '<link rel="icon" href="HREF" type="image/svg+xml">\n'
# the emulator's sound on phones: Safari mutes web audio while an iPhone is in silent mode unless the page asks for
# the "playback" audio session (the Audio Session API, where there is one), and a call, Siri or another app leaves the
# context "interrupted" until something resumes it. Before the page's own script, so the session is set before the
# switch-on makes its AudioContext; every AudioContext it makes is kept, and woken by the next touch or key.
AUDIO_FIX = """<script>
(function () {
  try { if (navigator.audioSession) navigator.audioSession.type = "playback"; } catch (e) {}
  var A = window.AudioContext;
  if (!A) return;
  var made = [];
  window.AudioContext = class extends A { constructor(o) { super(o); made.push(this); } };
  function wake() {
    made.forEach(function (c) { if (c.state !== "running" && c.state !== "closed") c.resume().catch(function () {}); });
  }
  ["pointerdown", "touchend", "keydown"].forEach(function (t) { document.addEventListener(t, wake, true); });
  document.addEventListener("visibilitychange", function () { if (!document.hidden) wake(); });
})();
</script>
"""
START = '<div class="start" id="start"><button type="button" id="go">Switch on</button></div>'
START_HINT = ('<div class="start" id="start"><div class="startbox"><button type="button" id="go">Switch on</button>'
              '<p class="hint">Then press PLAY / STOP to hear the demo groove.</p></div></div>')

FAVICON = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect width="64" height="64" rx="14" fill="#5AD282"/>'
           '<text x="32" y="47" font-family="Arial Narrow, Arial, sans-serif" font-weight="700" font-size="44" '
           'text-anchor="middle" fill="#16171B">8</text></svg>\n')


BRANDING = ROOT / "site" / "branding"
RELEASE = json.loads((ROOT / "release/config.json").read_text(encoding="utf-8"))
GUIDE = (
    ("", "Quick start", "Quick start"),
    ("beats", "Drums & steps", "Drums and sequencing"),
    ("sounds", "Sounds & samples", "Synths, samples and acid"),
    ("modulation", "Modulation", "Modulation in depth"),
    ("recording", "Recording", "Recording"),
    ("performance", "Live & song", "Playing live"),
    ("mixer", "Mixer & FX", "Mixer and effects"),
    ("setup", "Save & install", "Saving and installing"),
    ("reference", "Every control", "Every control, page by page"),
)
GUIDE_PARTS = (
    ("Getting started", ("",)),
    ("Making a track", ("beats", "sounds", "modulation", "recording")),
    ("Finishing & performing", ("performance", "mixer")),
    ("Reference", ("setup", "reference")),
)
CHAPTER_SUMMARIES = {
    "": "First beat, panel controls, screen and the browser instrument.",
    "beats": "Drum pads, step pages, track lengths, accents, parameter locks and erasing.",
    "sounds": "Voice types, oscillators, modulation, presets, sampling and the 303.",
    "recording": "Real-time and step entry, recording options, chords and knob motion.",
    "performance": "Clips, scenes, follow actions, performance moves, songs and sync.",
    "mixer": "Levels, sends, effects, the mastering chain and reading the meters.",
    "setup": "Project storage, saving patterns and sounds, MIDI, updates and recovery.",
    "modulation": "LFO 1 and 2, the MOD ENV, the matrix's sources and destinations, macros, locks and recipes.",
    "reference": "Every page, button, key and knob with its range, the presets, the limits and the screen's messages.",
}


class ChapterHeadings(HTMLParser):
    """Use real chapter H2s and their explicit IDs as the one source for the TOC."""

    def __init__(self):
        super().__init__()
        self.headings = []
        self.identifiers = set()
        self.text = None
        self.heading_id = None

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        marker = attrs.get("id")
        if marker:
            if marker in self.identifiers or marker == "gm-main":
                raise ValueError(f"duplicate or reserved ID {marker!r}")
            self.identifiers.add(marker)
        if tag == "h1":
            raise ValueError("chapter fragments may not contain an H1")
        if tag == "h2":
            if not marker or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", marker):
                raise ValueError("each chapter H2 needs an explicit lowercase hyphenated id")
            self.heading_id = marker
            self.text = []

    def handle_data(self, data):
        if self.text is not None:
            self.text.append(data)

    def handle_endtag(self, tag):
        if tag == "h2" and self.text is not None:
            label = " ".join("".join(self.text).split())
            if not label:
                raise ValueError(f"chapter H2 is empty: {self.heading_id}")
            self.headings.append((self.heading_id, label))
            self.text = None
            self.heading_id = None


def guide_table(match):
    table = match.group()
    caption = re.search(r"<caption>(.*?)</caption>", table, re.S)
    name = html.unescape(re.sub(r"<[^>]*>", "", caption.group(1))).strip() if caption else "Reference table"
    return f'<div class="table-wrap gm-guide-table" tabindex="0" role="region" aria-label="{html.escape(name)}">{table}</div>'


def guide_navigation(current_slug, topics, mobile=False):
    chapter_labels = {slug: (i + 1, label) for i, (slug, label, _) in enumerate(GUIDE)}
    markup = []
    for part, members in GUIDE_PARTS:
        chapters = []
        for slug in members:
            number, label = chapter_labels[slug]
            active = ' aria-current="page"' if slug == current_slug else ''
            subtopics = ''
            if slug == current_slug:
                subtopics = '<ul class="gm-toc-topics">' + ''.join(
                    f'<li><a href="#{anchor}" data-guide-topic>{html.escape(text)}</a></li>'
                    for anchor, text in topics
                ) + '</ul>'
            link = (
                f'<a class="gm-toc-chapter" href="{guide_path(slug)}"{active}>'
                f'<span class="gm-chapter-number" aria-hidden="true">{number:02d}</span>'
                f'<span>{html.escape(label)}</span></a>'
            )
            chapters.append(f'<li>{link}{subtopics}</li>')
        markup.append(
            f'<li><span class="gm-toc-part">{html.escape(part)}</span>'
            f'<ol class="gm-toc-chapters">{"".join(chapters)}</ol></li>'
        )
    label = "Guide table of contents (mobile)" if mobile else "Guide table of contents"
    return f'<nav class="gm-toc" aria-label="{label}"><ul class="gm-toc-parts">{"".join(markup)}</ul></nav>'


def guide_catalog():
    labels = {slug: (i + 1, label) for i, (slug, label, _) in enumerate(GUIDE)}
    parts = []
    chapter_index = "gm-chapter-index"
    for part, members in GUIDE_PARTS:
        entries = []
        for slug in members:
            number, label = labels[slug]
            entries.append(
                f'<li><a href="{guide_path(slug)}"><span aria-hidden="true">{number:02d}. </span>'
                f'{html.escape(label)}</a><p>{html.escape(CHAPTER_SUMMARIES[slug])}</p></li>'
            )
        parts.append(f'<li><h3>{html.escape(part)}</h3><ol>{"".join(entries)}</ol></li>')
    return (
        f'<div class="gm-book-index" aria-labelledby="{chapter_index}">'
        f'<h2 id="{chapter_index}" tabindex="-1">Chapter index</h2>'
        f'<ul>{"".join(parts)}</ul></div>'
    )


TOC_SCRIPT = """'use strict';
(() => {
  const topics = Array.from(document.querySelectorAll('a[data-guide-topic]'));
  const drawer = document.querySelector('.gm-toc-mobile');
  const mark = () => {
    for (const link of topics) {
      if (link.hash === location.hash) link.setAttribute('aria-current', 'location');
      else link.removeAttribute('aria-current');
    }
  };
  mark();
  window.addEventListener('hashchange', mark);
  if (drawer) drawer.addEventListener('keydown', (event) => {
    if (event.key !== 'Escape' || !drawer.open) return;
    event.preventDefault();
    drawer.open = false;
    drawer.querySelector('summary').focus({preventScroll: true});
  });
  for (const link of topics) {
    link.addEventListener('click', (event) => {
      if (event.metaKey || event.ctrlKey || event.shiftKey || event.altKey || event.button !== 0) return;
      const target = document.getElementById(link.hash.slice(1));
      if (drawer && drawer.contains(link)) drawer.open = false;
      if (target) requestAnimationFrame(() => target.focus({preventScroll: true}));
    });
  }
})();
"""

FRAME_STYLESHEET = '<link rel="stylesheet" href="/bongcult-frame/frame.css">\n'
FRAME_TOP = '<div class="bc-frame bc-frame-top" aria-hidden="true"><span></span></div>\n'
FRAME_BOTTOM = '<div class="bc-frame bc-frame-bottom" aria-hidden="true"><i></i><span></span><i></i></div>\n'


EDITORIAL_BASE = "/bongcult-frame/editorial"
PRINT_SOURCES = json.loads((BRANDING / "editorial-print-sources.json").read_text(encoding="utf-8"))["routes"]
FAMILY_STYLESHEET = f'<link rel="stylesheet" href="{EDITORIAL_BASE}/mast.css">\n'
FAMILY_MAST = """<header class="gm-family">
  <a class="gm-family-brand" href="https://bongcult.com/"><img src="/bong-cult-logo.svg" alt="Bong Cult home" width="866" height="320"></a>
  <nav class="gm-family-links" aria-label="Bong Cult">
    <a href="https://bongcult.com/">Journal</a>
    <a href="https://tools.bongcult.com/">Audio Tools</a>
    <a href="https://bongcult.com/guestbook/">Guestbook</a>
    <a href="https://bongcult.com/about/">About</a>
  </nav>
</header>
"""
MASTHEAD = '<a class="bc-skip" href="#gm-main">Skip to content</a>\n' + FAMILY_MAST


def frontispiece(slug):
    description = html.escape(PRINT_SOURCES[slug]["instrument"]["title"], quote=True)
    return (
        f'<figure class="gm-plate"><img alt="{description}" '
        f'src="{EDITORIAL_BASE}/{slug}-instrument.webp" decoding="async"></figure>'
    )


LANDING = """<div class="hero gm-home">
  <div class="screen">
    <div class="gm-hero-copy">
      <div class="top"><span class="label">Custom firmware for the M-VAVE FM-1</span><span class="ver">RELEASE_VERSION_LABEL</span></div>
      <h1><span class="logo">GhettoMachine</span></h1>
      <p class="sub">Four-track groovebox firmware. 909 and 808 kits, three synth or sampler tracks, and a 128-step sequencer.</p>
    </div>
    <img class="gm-four-registers" src="/bongcult-frame/windchest/four-registers.webp"
      srcset="/bongcult-frame/windchest/four-registers-540.webp 540w, /bongcult-frame/windchest/four-registers.webp 1080w"
      sizes="(max-width: 620px) calc(84vw - 3rem), (min-width: 1547px) 39rem, 39vw" width="1080" height="800"
      alt="A hand reaches across four organ registers, one printed in hot pink, above a row of keys." fetchpriority="high">
  </div>
  <div class="gm-home-entry"><p class="lede">GhettoMachine is BongSoft custom firmware engineered by BongTech, part of <a href="https://bongcult.com/">BongCult</a>. Make a beat in the browser or look up the controls for the FM-1.</p>
  <div class="gm-actions" id="play">
    <a class="gm-button gm-primary" href="/guide/">Open the User Guide</a>
    <a class="gm-button" href="/emu/">Play in browser</a>
  </div>
  <p class="gm-home-links"><a href="/technical/">How it works</a> · <a href="REPO">Source on GitHub</a></p>
  <details class="gm-technical-topics">
    <summary>Technical topics</summary>
    TECHNICAL_TOPIC_LINKS
  </details></div>
</div>
"""


def guide_path(slug):
    return f"/guide/{slug}/" if slug else "/guide/"


def nav_link(href, label, current):
    selected = ' aria-current="page"' if href == current else ''
    return f'<a href="{href}"{selected}>{html.escape(label)}</a>'


def render_document(head, title, path, content, guide_slug=None, topics=()):
    is_guide = guide_slug is not None
    play_label = "Play in browser" if is_guide else "Play"
    nav = [("/", "Home"), ("/guide/", "User Guide"), ("/emu/", play_label), ("/technical/", "How it works")]
    site_nav = "".join(nav_link(href, label, path) for href, label in nav)
    chapters = ""
    if is_guide:
        active_label = next(label for slug, label, _ in GUIDE if slug == guide_slug)
        chapters = (
            '<div class="gm-toc-desktop"><p class="gm-toc-title">Table of contents</p>'
            + guide_navigation(guide_slug, topics) + '</div>'
            '<details class="gm-toc-mobile">'
            '<summary aria-label="Table of contents">Contents'
            f'<span>{html.escape(active_label)}</span></summary>'
            '<div class="gm-toc-mobile-panel">'
            + guide_navigation(guide_slug, topics, mobile=True) + '</div></details>'
        )
    section = (guide_slug or "quick") if is_guide else ("technical" if path == "/technical/" else "home")
    css_class = ' gm-guide-page' if is_guide else ''
    aside = (f'<aside class="bc-sidebar"><div class="gm-nav-stack">{chapters}</div></aside>\n'
             if is_guide else '')
    js = ('<script src="/chapter-redirect.js"></script>\n' if path == '/' else
          '<script src="/guide-toc.js" defer></script>\n' if is_guide else '')
    return (
        '<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n'
        '<meta name="theme-color" content="#10100f">\n'
        '<meta name="description" content="GhettoMachine is BongSoft custom firmware by BongTech: a four-track groovebox for the M-VAVE FM-1, playable in the browser.">\n'
        f'<link rel="canonical" href="https://ghettomachine.bongcult.com{path}">\n'
        + re.sub(r'<title>[^<]*</title>', f'<title>{html.escape(title)}</title>', head, count=1) + '\n'
        + ICON.replace("HREF", "/favicon.svg")
        + FRAME_STYLESHEET + '<link rel="stylesheet" href="/bong-cult-shell.css">\n'
        + FAMILY_STYLESHEET + f'<link rel="stylesheet" href="{EDITORIAL_BASE}/reader.css">\n' + js + '</head>\n'
        + f'<body class="gm-bong-cult{css_class}" data-edition="{section}">\n' + MASTHEAD
        + '<div class="gm-project"><p>BongSoft · BongTech</p>'
        + f'<nav aria-label="GhettoMachine">{site_nav}</nav></div>\n'
        + '<div class="gm-page-shell">\n' + aside
        + f'<main class="wrap" id="gm-main" tabindex="-1">\n{content}\n</main>\n</div>\n' + FRAME_BOTTOM + '</body>\n</html>\n'
    )


def build_landing(out):
    out.mkdir(parents=True, exist_ok=True)
    page = (ROOT / "docs" / "how-it-works.html").read_text()
    frame = (
        '<section id="play" style="--c: var(--t2)">\n'
        '    <div class="band"><h2>Play it</h2><span class="tag">IN THE BROWSER</span></div>\n'
        "    <p>This is GhettoMachine itself, compiled to WebAssembly: the same sequencer, drums, synth, effects and screen as on "
        "the FM-1, drawn as its panel. Press <strong>Switch on</strong>, then <strong>PLAY / STOP</strong> to hear its demo "
        "groove, and play it with the mouse or your computer's keyboard. Your work is kept in this browser. If an iPhone "
        "stays silent, turn up its volume, and turn off silent mode: older iOS versions mute web audio in it.</p>\n"
        '    <div class="emu-frame"><iframe src="/emu/" title="GhettoMachine, running in the browser" allow="midi; autoplay" '
        'loading="lazy"></iframe></div>\n'
        f'    <p><a href="/emu/">Open it full size</a> · <a href="{REPO}">The source</a> · '
        f'<a href="{REPO}/blob/main/MANUAL.md">The manual</a></p>\n'
        "  </section>")
    page, n = re.subn(r"<!-- emu:start -->.*?<!-- emu:end -->", frame, page, flags=re.S)
    if n != 1:
        raise SystemExit("site: the page has no <!-- emu:start --> ... <!-- emu:end --> block")
    page = page.replace("It's about 430 KB of code, and the same code runs in your browser, right below.",
                        "It's about 430 KB of code, and the same code runs in your browser, right here.")
    page = re.sub(r'<meta name="claude:[^>]*>\n', "", page)
    head_end = page.index("</style>") + len("</style>")
    head = page[:head_end]
    tag = '<div class="wrap">'
    contents = page[head_end:].strip()
    if not contents.startswith(tag) or not contents.endswith("</div>"):
        raise SystemExit("site: could not find the landing page wrapper")
    contents = contents[len(tag):-len("</div>")].strip()
    # Keep previously shared /#chapter addresses working. The labeled links
    # on the home page cover the same destinations even with JS disabled.
    links = re.findall(r'<li><a href="#([a-z]+)"><i[^>]*></i>([^<]+)</a></li>', contents)
    if not links or len(links) != len(set(slug for slug, _ in links)):
        raise SystemExit("site: could not identify original technical anchors")
    fallback = "<ul>" + "".join(f'<li><a href="/technical/#{slug}">{label}</a></li>' for slug, label in links) + "</ul>"
    (out / "chapter-redirect.js").write_text(
        "'use strict';\n"
        "(() => {\n"
        f"  const chapters = new Set({json.dumps([slug for slug, _ in links])});\n"
        "  const forward = () => {\n"
        "    const chapter = location.hash.slice(1);\n"
        "    if (chapters.has(chapter)) location.replace('/technical/' + location.search + location.hash);\n"
        "  };\n"
        "  forward();\n"
        "  addEventListener('hashchange', forward);\n"
        "})();\n"
    )
    landing = LANDING.replace("RELEASE_VERSION_LABEL", html.escape(RELEASE["targets"]["web"]["landing_label"])).replace("REPO", REPO).replace("TECHNICAL_TOPIC_LINKS", fallback)
    (out / "index.html").write_text(render_document(head, "GhettoMachine · BongSoft", "/", landing))
    tech_dir = out / "technical"
    tech_dir.mkdir(exist_ok=True)
    contents = contents.replace('<div class="screen">', '<div class="screen gm-chapter-head"><div class="gm-title-copy">', 1)
    contents = contents.replace('  <p class="lede">', frontispiece("technical") + '</div>\n  <p class="lede">', 1)
    (tech_dir / "index.html").write_text(render_document(head, "Inside GhettoMachine · Technical tour", "/technical/", contents))
    (out / "guide-toc.js").write_text(TOC_SCRIPT)
    slugs = [slug for slug, _, _ in GUIDE]
    if slugs != [slug for _, members in GUIDE_PARTS for slug in members] or set(CHAPTER_SUMMARIES) != set(slugs):
        raise SystemExit("site: guide manifest and TOC parts must contain the same chapters in order")
    for i, (slug, label, title) in enumerate(GUIDE):
        path = guide_path(slug)
        dest = out / path.strip("/")
        dest.mkdir(parents=True, exist_ok=True)
        chapter = (ROOT / "site" / "guide" / f"{slug or 'index'}.html").read_text().strip()
        parser = ChapterHeadings()
        try:
            parser.feed(chapter)
            parser.close()
            if parser.text is not None or not parser.headings:
                raise ValueError("missing or unclosed H2")
            if not slug and "gm-chapter-index" in parser.identifiers:
                raise ValueError("chapter index ID reserved for generated content")
        except ValueError as error:
            raise SystemExit(f"site: bad guide chapter {slug or 'index'}: {error}") from error
        chapter = re.sub(r'(<h2\b[^>]*)(>)', r'\1 tabindex="-1"\2', chapter)
        chapter = re.sub(r"<table\b[^>]*>.*?</table>", guide_table, chapter, flags=re.S)

        topics = parser.headings
        if not slug:
            chapter += guide_catalog()
            topics = topics + [("gm-chapter-index", "Chapter index")]

        nav = []
        if i:
            prev_slug, prev_label, _ = GUIDE[i - 1]
            nav.append(f'<a rel="prev" href="{guide_path(prev_slug)}">← {html.escape(prev_label)}</a>')
        if i < len(GUIDE) - 1:
            next_slug, next_label, _ = GUIDE[i + 1]
            nav.append(f'<a rel="next" href="{guide_path(next_slug)}">{html.escape(next_label)} →</a>')
        body = (
            '<article class="gm-guide"><header class="gm-chapter-head"><div class="gm-title-copy">'
            f'<p class="gm-crumb">User Guide · {i + 1} of {len(GUIDE)}</p><h1>{html.escape(title)}</h1></div>'
            + frontispiece(slug or "quick") + '</header>\n'
            + chapter + '<nav class="gm-guide-pager" aria-label="Adjacent chapters">'
            + "".join(nav) + '</nav></article>'
        )
        (dest / "index.html").write_text(render_document(head, f"{title} · GhettoMachine User Guide", path, body, slug, topics))
    for file in ("bong-cult-logo.svg", "bong-cult-shell.css", "dungeon-bbs-tokens.css"):
        shutil.copyfile(BRANDING / file, out / file)
    shutil.copytree(BRANDING / "bongcult-frame", out / "bongcult-frame", dirs_exist_ok=True)


def emulator_document(page):
    page, count = re.subn(r"<title>[^<]*</title>", "<title>GhettoMachine in the browser · BongSoft</title>", page, count=1)
    if count != 1:
        raise SystemExit("site: the emulator's page has no title")
    # --landing-only can read a previously generated emulator. Leave its
    # single frame in place on repeated builds.
    if FRAME_STYLESHEET not in page:
        if any(page.count(tag) != 1 for tag in ("</head>", "<body>", "</body>")):
            raise SystemExit("site: unexpected emulator shell")
        frame_layout = ("<style>body > .bc-frame { max-width: 1040px; margin: .8rem auto; "
                        "padding: .35rem .65rem; background: #10100f; } "
                        ".framed body > .bc-frame { display: none; }</style>\n")
        page = page.replace("</head>", FRAME_STYLESHEET + frame_layout + "</head>")
        page = page.replace("<body>", "<body>\n" + FRAME_TOP)
        page = page.replace("</body>", FRAME_BOTTOM + "</body>")
    if FAMILY_STYLESHEET not in page:
        page = page.replace("</head>", FAMILY_STYLESHEET + "</head>")
        page = page.replace("<body>", "<body>\n" + FAMILY_MAST)
    return page


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "--landing-only":
        out = Path(args[1]).resolve()
        build_landing(out)
        page = out / "emu" / "index.html"
        if page.is_file():
            page.write_text(emulator_document(page.read_text()))
        return
    if len(args) != 1 or args[0].startswith("-"):
        raise SystemExit(__doc__)
    out = Path(args[0]).resolve()
    emu_src = ROOT / "app" / "build" / "emu"
    if not (emu_src / "app.wasm").exists():
        raise SystemExit("site: no app/build/emu/app.wasm (tools/build.py ../../app --web first)")
    if out.exists():
        shutil.rmtree(out)
    (out / "emu").mkdir(parents=True)
    build_landing(out)

    for f in ("index.html", "app.js", "app.wasm", "app_worklet.js", "worklet.js"):
        shutil.copy(emu_src / f, out / "emu" / f)
    shutil.copytree(emu_src / "parts", out / "emu" / "parts")
    emu_page = (out / "emu" / "index.html").read_text()
    emu_page = emulator_document(emu_page)
    emu_page, n2 = re.subn(r"<h1>[^<]*</h1>", '<h1>GhettoMachine</h1>\n  <p><a href="../">How it works</a></p>', emu_page, count=1)
    if n2 != 1 or emu_page.count(START) != 1:
        raise SystemExit("site: the emulator's page has no <title>, <h1> or switch-on block to change")
    emu_page = emu_page.replace(START, START_HINT)
    # framed in the page: no title of its own (the page has it), less room around it
    emu_page = emu_page.replace("</head>", ICON.replace("HREF", "../favicon.svg") + AUDIO_FIX +
                                '<script>if (self !== top) document.documentElement.classList.add("framed");</script>\n'
                                "<style>.framed body > header { display: none; } "
                                ".framed body { padding-block: 14px 14px; }\n"
                                # the name in the logo's face (the boot screen's, Sedgwick Ave Display, SIL OFL 1.1)
                                'body > header h1 { font: 400 2.4rem/1.05 "Sedgwick Ave Display", "Marker Felt", '
                                'cursive; }\n'
                                '.startbox { display: grid; justify-items: center; gap: .7rem; padding: 0 1rem; } '
                                '.start .hint { margin: 0; text-align: center; color: #fff; '
                                'font: 600 1.05rem/1.3 "Barlow Semi Condensed", "Barlow", sans-serif; '
                                'text-shadow: 0 1px 3px rgba(0, 0, 0, .8); }</style>\n'
                                '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?'
                                'family=Sedgwick+Ave+Display&display=swap">\n</head>', 1)
    (out / "emu" / "index.html").write_text(emu_page)
    (out / "favicon.svg").write_text(FAVICON)

    (out / "_headers").write_text("/emu/*.wasm\n  Content-Type: application/wasm\n  Cache-Control: public, max-age=3600\n")
    total = sum(f.stat().st_size for f in out.rglob("*") if f.is_file())
    print(f"site: {out} ({total // 1024} KB)")


if __name__ == "__main__":
    main()
