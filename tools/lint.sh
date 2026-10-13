#!/usr/bin/env bash
# The house lint: every shipped string and doc is ASCII, has no em or en
# dashes, no emoji, no bold-for-emphasis in prose, and glyph rows are exactly
# 16 characters. Runs over content.h, parts.h, glyphs.h, HANDOFF.md and
# README.md. Exit 1 on any finding.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
fail=0
files=()
for f in src/content.h src/parts.h src/glyphs.h HANDOFF.md README.md; do [ -f "$f" ] && files+=("$f"); done

for f in "${files[@]}"; do
  # em dash, en dash, any non-ASCII byte (covers emoji)
  if LC_ALL=C grep -nP '[\x80-\xFF]' "$f" >/dev/null 2>&1; then
    echo "$f: non-ASCII bytes (dashes or emoji):"; LC_ALL=C grep -nP '[\x80-\xFF]' "$f" | head -20; fail=1
  fi
  case "$f" in
    src/*.h)
      # lowercase letters inside a quoted string (game strings are ASCII caps); format specifiers are allowed
      if grep -nE '"[^"]*"' "$f" | sed -E 's/%[0-9.]*[a-z]//g' | grep -nE '"[^"]*[a-z][^"]*"' | grep -v 'include' >/dev/null; then
        echo "$f: lowercase inside a game string:"; grep -nE '"[^"]*"' "$f" | sed -E 's/%[0-9.]*[a-z]//g' | grep -E '"[^"]*[a-z][^"]*"' | grep -v include | head -20; fail=1
      fi
      ;;
    *.md)
      if grep -nE '\*\*[^*]+\*\*' "$f" >/dev/null; then
        echo "$f: bold for emphasis:"; grep -nE '\*\*[^*]+\*\*' "$f" | head -20; fail=1
      fi
      ;;
  esac
done

# glyph rows: every quoted row in glyphs.h is exactly 16 characters
if [ -f src/glyphs.h ]; then
  bad="$(grep -oE '"[.#]+"' src/glyphs.h | awk 'length($0) != 18' | head -5)"
  if [ -n "$bad" ]; then echo "src/glyphs.h: glyph rows that are not 16 wide: $bad"; fail=1; fi
fi

# string buffers: stingers under 24, hooks under 24, blurbs under 32, hints under 48
if [ -f src/content.h ]; then
  while IFS= read -r line; do
    case "$line" in
      *"char *ST_"*)      lim=24 ;;
      *"ARENA_BLURB"*) lim=24 ;;
      *"MODE_BLURB"*|*"OPS_LINES"*) lim=32 ;;
      *) continue ;;
    esac
    echo "$line" | grep -oE '"[^"]*"' | while IFS= read -r s; do
      n=$(( ${#s} - 2 ))
      if [ "$n" -ge "$lim" ]; then echo "src/content.h: $s is $n chars, limit $lim"; exit 3; fi
    done || fail=1
  done < src/content.h
fi

if [ $fail = 0 ]; then echo "lint: zero findings over ${files[*]}"; fi
exit $fail
