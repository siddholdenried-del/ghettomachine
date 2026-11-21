#!/bin/sh
# The existing GitHub main branch deploys the generated site/public.
# The ordinary/default command is read-only. --publish remains explicit.
set -eu
case "${1:---check}" in
    --check) python3 release/verify_release.py verify site/public; exit ;;
    --publish) ;;
    *) echo "usage: sh site/publish.sh [--check | --publish]" >&2; exit 2 ;;
esac
[ "$(git branch --show-current)" = main ] || { echo "publish: only the reviewed local main branch can publish" >&2; exit 1; }
[ -z "$(git status --porcelain -- .)" ] || { echo "publish: commit all reviewed source changes first" >&2; exit 1; }
SRC=$(git rev-parse --short HEAD)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT HUP INT TERM
python3 -I site/build.py "$STAGE/public"
python3 release/verify_release.py seal "$STAGE/public"
python3 release/verify_release.py verify "$STAGE/public"
# Don't alter the checkout until the isolated output has passed all policy checks.
rsync -a --delete "$STAGE/public/" site/public/
git add -A -- site/public
if git diff --cached --quiet -- site/public; then
    echo "publish: site/public already matches approved release hashes"
else
    git commit -q -m "The verified site, built from $SRC"
fi
git push origin main
echo "publish: main pushed; verify the served release separately after Cloudflare Workers Builds finishes"
