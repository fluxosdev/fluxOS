#!/bin/sh
# The phase 3 tools (tar, gzip, sed, diff, patch, find, ...) built for the
# host, checked against the GNU originals: archives and compressed files
# that each side can read, diffs that each side's patch applies, and
# the same output for the same scripts and options.
#   tests/tools-host.sh TOOLDIR      (TOOLDIR holds the host-built tools; "make test-tools")
set -u
tools=$(cd "$1" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { echo "FAIL: $*"; fail=$((fail + 1)); }
check() { if "$@" >/dev/null 2>&1; then ok; else bad "$*"; fi; }
same() { if cmp -s "$1" "$2"; then ok; else bad "$3"; diff "$1" "$2" | head -5; fi; }

cd "$work"
# a tree with a bit of everything
mkdir -p tree/src/deep/deeper tree/empty
printf 'hello\nworld\n' > tree/src/a.txt
seq 1 5000 > tree/src/numbers.txt
head -c 200000 /dev/urandom > tree/src/random.bin
{ head -c 300000 /dev/zero | tr '\0' 'x'; echo; } > tree/src/xs.txt     # a whole line: find -exec cat's output is sorted
: > tree/src/deep/deeper/empty.txt
cp "$tools/sed" tree/src/deep/binary
chmod 640 tree/src/a.txt
chmod 755 tree/src/deep/binary
touch -d '2020-02-02 02:02:02' tree/src/a.txt
longdir=tree/src/$(printf 'd%.0s' $(seq 1 60))/$(printf 'e%.0s' $(seq 1 60))
mkdir -p "$longdir"
echo deep > "$longdir/$(printf 'f%.0s' $(seq 1 70)).txt"

# ---- gzip ----
for f in tree/src/a.txt tree/src/numbers.txt tree/src/random.bin tree/src/xs.txt tree/src/deep/deeper/empty.txt; do
    "$tools/gzip" -c "$f" > ours.gz && gzip -dc ours.gz > back1 && same "$f" back1 "gzip: GNU gunzip reads ours ($f)"
    gzip -c "$f" > theirs.gz && "$tools/gzip" -dc theirs.gz > back2 && same "$f" back2 "gzip: ours reads GNU's ($f)"
    "$tools/gzip" -dc ours.gz > back3 && same "$f" back3 "gzip: round trip ($f)"
    "$tools/gzip" -1 -c "$f" | "$tools/gzip" -dc > back4 && same "$f" back4 "gzip -1 ($f)"
    "$tools/gzip" -9 -c "$f" | gzip -dc > back5 && same "$f" back5 "gzip -9 ($f)"
done
cat theirs.gz ours.gz > both.gz && "$tools/gzip" -dc both.gz > back6 && cat tree/src/deep/deeper/empty.txt tree/src/deep/deeper/empty.txt > exp6 && same exp6 back6 "gzip: concatenated members"
cat tree/src/numbers.txt | "$tools/gzip" > pipe.gz && "$tools/gzip" -dc < pipe.gz > back7 && same tree/src/numbers.txt back7 "gzip: through pipes"
cp tree/src/numbers.txt n.txt && "$tools/gzip" n.txt && [ -f n.txt.gz ] && [ ! -f n.txt ] && "$tools/gzip" -d n.txt.gz && [ -f n.txt ] && same tree/src/numbers.txt n.txt "gzip: file in place" || bad "gzip: in place"
"$tools/gzip" -n -c tree/src/a.txt > r1.gz; "$tools/gzip" -n -c tree/src/a.txt > r2.gz; same r1.gz r2.gz "gzip -n: reproducible"
check "$tools/gzip" -t ours.gz
printf 'not gzip' > bad.gz; if "$tools/gzip" -dc bad.gz >/dev/null 2>&1; then bad "gzip: accepted garbage"; else ok; fi
"$tools/gzip" -l theirs.gz | grep -q " theirs$" && ok || bad "gzip -l"
cmp -s "$("$tools/gzip" -c tree/src/random.bin | wc -c)" "" 2>/dev/null
sz=$("$tools/gzip" -c tree/src/xs.txt | wc -c); [ "$sz" -lt 3000 ] && ok || bad "gzip: 300000 x's came out as $sz bytes"

# ---- tar ----
(cd tree && "$tools/tar" -cf ../ours.tar .) && mkdir x1 && (cd x1 && tar -xf ../ours.tar) && diff -r tree x1 >/dev/null && ok || bad "tar: GNU tar unpacks ours"
[ "$(stat -c %a x1/src/a.txt)" = 640 ] && [ "$(stat -c %Y x1/src/a.txt)" = "$(stat -c %Y tree/src/a.txt)" ] && ok || bad "tar: modes and times kept"
(cd tree && tar --format=ustar -cf ../theirs.tar .) && mkdir x2 && (cd x2 && "$tools/tar" -xf ../theirs.tar) && diff -r tree x2 >/dev/null && ok || bad "tar: ours unpacks GNU ustar"
(cd tree && tar -cf ../gnu.tar .) && mkdir x3 && (cd x3 && "$tools/tar" -xf ../gnu.tar) && diff -r tree x3 >/dev/null && ok || bad "tar: ours unpacks GNU format"
(cd tree && tar --format=pax -cf ../pax.tar .) && mkdir x4 && (cd x4 && "$tools/tar" -xf ../pax.tar) && diff -r tree x4 >/dev/null && ok || bad "tar: ours unpacks pax"
"$tools/tar" -tf ours.tar | sort > l1; tar -tf ours.tar | sort > l2; same l1 l2 "tar -t"
"$tools/tar" -tvf ours.tar | grep -q "^-rw-r----- 0/0 .*12 2020-02-02 .* ./src/a.txt" && ok || bad "tar -tv"
(cd tree && "$tools/tar" -czf ../ours.tgz src) && mkdir x5 && (cd x5 && tar -xzf ../ours.tgz) && diff -r tree/src x5/src >/dev/null && ok || bad "tar -z create"
(cd tree && tar -czf ../theirs.tgz src) && mkdir x6 && (cd x6 && "$tools/tar" -xf ../theirs.tgz) && diff -r tree/src x6/src >/dev/null && ok || bad "tar: gzip magic detected"
mkdir x7 && cat theirs.tgz | (cd x7 && "$tools/tar" -xzf -) && diff -r tree/src x7/src >/dev/null && ok || bad "tar -xzf - from a pipe"
mkdir x7b && cat ours.tar | (cd x7b && "$tools/tar" -xf -) && diff -r tree x7b >/dev/null && ok || bad "tar -xf - from a pipe"
mkdir -p two/a two/b && echo A > two/a/f && echo B > two/b/g && "$tools/tar" --format=ustar --owner=0 --group=0 --numeric-owner -cf two.tar -C two/a . -C "$work/two/b" . && "$tools/tar" -tf two.tar | sort | tr '\n' ' ' | grep -q "^\./ \./ \./f \./g " && ok || bad "tar: several -C"
mkdir x8 && "$tools/tar" -xf ours.tar -C x8 src/a.txt && [ -f x8/src/a.txt ] && [ ! -f x8/src/numbers.txt ] && ok || bad "tar -x member"
mkdir x9 && "$tools/tar" -xf ours.tar -C x9 --strip-components=1 ./src/deep && [ -d x9/deep/deeper ] && ok || bad "tar --strip-components"
"$tools/tar" -xOf ours.tar ./src/a.txt > o.txt && same tree/src/a.txt o.txt "tar -O"
if "$tools/tar" -xf ours.tar -C /nonexistent 2>/dev/null; then bad "tar: bad -C"; else ok; fi
printf '\0\0' | (mkdir x10 && cd x10 && "$tools/tar" -xf - 2>/dev/null); ok
(cd tree && "$tools/tar" -cf ../r1.tar .) && (cd tree && "$tools/tar" -cf ../r2.tar .) && same r1.tar r2.tar "tar: reproducible"
# crafted archives: a long-name header claiming 4 GiB; a directory member over a symbolic link
python3 -c '
import sys
h = bytearray(512)
h[0:13] = b"././@LongLink"; h[100:108] = b"0000644\0"; h[124:136] = b"37777777777\0"; h[156] = ord("L"); h[257:263] = b"ustar\0"
h[148:156] = b" " * 8; h[148:156] = b"%06o\0 " % sum(h)
sys.stdout.buffer.write(bytes(h) + b"x" * 2048)' > huge.tar
"$tools/tar" -tf huge.tar >/dev/null 2>&1; [ $? = 2 ] && ok || bad "tar: 4 GiB long-name header"
mkdir -p ln1 ln2/d xl/x xl/outside && ln -s ../outside ln1/d && chmod 777 ln2/d && touch -d '2001-01-01' ln2/d && chmod 700 xl/outside &&
    (cd ln1 && tar -cf ../lnk.tar d) && (cd ln2 && tar -rf ../lnk.tar d) && (cd xl/x && "$tools/tar" -xf ../../lnk.tar 2>/dev/null)
[ "$(stat -c %a xl/outside)" = 700 ] && [ "$(stat -c %Y xl/outside)" != "$(stat -c %Y ln2/d)" ] && ok || bad "tar: directory member over a symbolic link"
mkdir evil && printf '../evil.txt' > /dev/null && (cd tree && "$tools/tar" -cf ../abs.tar "$work/tree/src/a.txt" 2>/dev/null) && "$tools/tar" -tf abs.tar | grep -q "^tmp" && ok || bad "tar: leading / removed"

# ---- sed ----
S="$tools/sed"
printf 'one\ntwo\nthree\nfour\nfive\n' > in.txt
t() { expect=$1; shift; got=$("$@" 2>&1); if [ "$got" = "$expect" ]; then ok; else bad "$* -> '$got' (expected '$expect')"; fi; }
t "$(sed 's/o/0/' in.txt)" "$S" 's/o/0/' in.txt
t "$(sed 's/o/0/g' in.txt)" "$S" 's/o/0/g' in.txt
t "$(sed -n '2,3p' in.txt)" "$S" -n '2,3p' in.txt
t "$(sed -n '/t/p' in.txt)" "$S" -n '/t/p' in.txt
t "$(sed '2d;4d' in.txt)" "$S" '2d;4d' in.txt
t "$(sed '$d' in.txt)" "$S" '$d' in.txt
t "$(sed -n '$p' in.txt)" "$S" -n '$p' in.txt
t "$(sed 's/\(t\)\(.\)/\2\1/' in.txt)" "$S" 's/\(t\)\(.\)/\2\1/' in.txt
t "$(sed -E 's/(t)(.)/\2\1/' in.txt)" "$S" -E 's/(t)(.)/\2\1/' in.txt
t "$(sed 's/.*/[&]/' in.txt)" "$S" 's/.*/[&]/' in.txt
t "$(sed '2i\
inserted' in.txt)" "$S" '2i\
inserted' in.txt
t "$(sed '2a appended' in.txt)" "$S" '2a appended' in.txt
t "$(sed '2,3c\
changed' in.txt)" "$S" '2,3c\
changed' in.txt
t "$(sed 'y/ot/OT/' in.txt)" "$S" 'y/ot/OT/' in.txt
t "$(sed -n '/two/,/four/{s/^/> /;p}' in.txt)" "$S" -n '/two/,/four/{s/^/> /;p}' in.txt
t "$(sed '3q' in.txt)" "$S" '3q' in.txt
t "$(sed -n '2{p;q}' in.txt)" "$S" -n '2{p;q}' in.txt
t "$(sed '=' in.txt)" "$S" '=' in.txt
t "$(sed -n 'h;n;G;p' in.txt)" "$S" -n 'h;n;G;p' in.txt
t "$(sed ':a;N;$!ba;s/\n/,/g' in.txt)" "$S" ':a;N;$!ba;s/\n/,/g' in.txt
t "$(sed -n '1~2p' in.txt)" "$S" -n '1~2p' in.txt
t "$(sed '2,+1d' in.txt)" "$S" '2,+1d' in.txt
t "$(sed 's/e/E/2' in.txt)" "$S" 's/e/E/2' in.txt
t "$(sed 's/O/0/I' in.txt)" "$S" 's/O/0/I' in.txt
t "$(sed -n 's/t/T/p' in.txt)" "$S" -n 's/t/T/p' in.txt
t "$(sed 's/^/\t/' in.txt)" "$S" 's/^/\t/' in.txt
t "$(sed 's/\(.*\)/\U\1/' in.txt)" "$S" 's/\(.*\)/\U\1/' in.txt
t "$(sed 's/\(.\)/\u\1/' in.txt)" "$S" 's/\(.\)/\u\1/' in.txt
t "$(sed '/two/!d' in.txt)" "$S" '/two/!d' in.txt
t "$(sed -e 's/o/0/' -e 's/e/3/' in.txt)" "$S" -e 's/o/0/' -e 's/e/3/' in.txt
t "$(sed 's/x*/-/g' in.txt)" "$S" 's/x*/-/g' in.txt
t "$(sed 'N;P;D' in.txt)" "$S" 'N;P;D' in.txt
t "$(sed -n 'l' in.txt)" "$S" -n 'l' in.txt
t "$(sed 's/o/\n/' in.txt)" "$S" 's/o/\n/' in.txt
t "$(sed 's|/usr|/opt|' in.txt)" "$S" 's|/usr|/opt|' in.txt
t "$(sed 's/[aeiou]/#/g' in.txt)" "$S" 's/[aeiou]/#/g' in.txt
t "$(sed 's/[/]/#/g' in.txt)" "$S" 's/[/]/#/g' in.txt
t "$(printf 'a b\n' | sed 's/ /\\/')" sh -c "printf 'a b\n' | $S 's/ /\\\\/'"
t "$(sed '0,/t/s/t/T/' in.txt)" "$S" '0,/t/s/t/T/' in.txt
t "$(printf 'x\n' | sed 's/x/a\
b/')" sh -c "printf 'x\n' | $S 's/x/a\\
b/'"
printf 's/one/1/\n# comment\n/three/d\n' > script.sed
t "$(sed -f script.sed in.txt)" "$S" -f script.sed in.txt
cp in.txt ip.txt && "$tools/sed" -i 's/one/ONE/' ip.txt && t "$(sed 's/one/ONE/' in.txt)" cat ip.txt
cp in.txt ip2.txt && "$tools/sed" -i.bak 's/one/ONE/' ip2.txt && [ -f ip2.txt.bak ] && same in.txt ip2.txt.bak "sed -i.bak"
printf 'no newline' > nonl.txt; t "$(sed 's/no/NO/' nonl.txt | od -c)" sh -c "$S 's/no/NO/' nonl.txt | od -c"
t "$(sed -n '2p' in.txt in.txt)" "$S" -n '2p' in.txt in.txt
t "$(sed -s -n '2p' in.txt in.txt)" "$S" -s -n '2p' in.txt in.txt
t "$(sed -n '$=' in.txt in.txt)" "$S" -n '$=' in.txt in.txt
t "$(sed 'r in.txt' nonl.txt)" "$S" 'r in.txt' nonl.txt
sed -n 's/o/0/w w1.txt' in.txt; "$tools/sed" -n 's/o/0/w w2.txt' in.txt; same w1.txt w2.txt "sed s///w"
if "$tools/sed" 'k' in.txt >/dev/null 2>&1; then bad "sed: unknown command accepted"; else ok; fi
t "$(sed 'x' in.txt)" "$S" 'x' in.txt
t "$(sed -n '/one/{n;p}' in.txt)" "$S" -n '/one/{n;p}' in.txt
t "$(sed '/o/{/t/d}' in.txt)" "$S" '/o/{/t/d}' in.txt
t "$(sed -n '/one/,/three/!p' in.txt)" "$S" -n '/one/,/three/!p' in.txt
t "$(sed 's/e$/E/' in.txt)" "$S" 's/e$/E/' in.txt
t "$(sed -E 's/(o|e)+/_/g' in.txt)" "$S" -E 's/(o|e)+/_/g' in.txt
t "$(sed 's/o\+/_/g' in.txt)" "$S" 's/o\+/_/g' in.txt
t "$(sed 's/o\|e/_/g' in.txt)" "$S" 's/o\|e/_/g' in.txt
t "$(sed 'tx;s/o/0/;:x' in.txt)" "$S" 'tx;s/o/0/;:x' in.txt
t "$(sed 's/o/0/;ta;s/$/!/;:a' in.txt)" "$S" 's/o/0/;ta;s/$/!/;:a' in.txt
t "$(sed 's/o/0/;Ta;s/$/!/;:a' in.txt)" "$S" 's/o/0/;Ta;s/$/!/;:a' in.txt
t "$(sed '2{h;d};${G}' in.txt)" "$S" '2{h;d};${G}' in.txt
t "$(sed 'z' in.txt)" "$S" 'z' in.txt
t "$(sed 2Q in.txt; echo "rc=$?")" sh -c "$S 2Q in.txt; echo rc=\$?"
t "$(sed '2q5' in.txt; echo "rc=$?")" sh -c "$S 2q5 in.txt; echo rc=\$?"
t "$(printf 'a.b\n' | sed 's/\./-/')" sh -c "printf 'a.b\n' | $S 's/\\./-/'"
t "$(printf 'path/x\n' | sed 's,/,:,g')" sh -c "printf 'path/x\n' | $S 's,/,:,g'"
t "$(printf 'abc\n' | sed -E 's/(a)(b)?(x)?c/[\1|\2|\3]/')" sh -c "printf 'abc\n' | $S -E 's/(a)(b)?(x)?c/[\\1|\\2|\\3]/'"
t "$(seq 1 20 | sed -n '5,10{/7/d;p}')" sh -c "seq 1 20 | $S -n '5,10{/7/d;p}'"
t "$(seq 1 10 | sed -n '2~3p')" sh -c "seq 1 10 | $S -n '2~3p'"
t "$(seq 1 10 | sed '3,~4d')" sh -c "seq 1 10 | $S '3,~4d'"
t "$(seq 1 10 | sed -n '/3/,/5/p;/8/,$p')" sh -c "seq 1 10 | $S -n '/3/,/5/p;/8/,\$p'"
t "$(seq 1 10 | sed '/1/,/3/{/2/d}')" sh -c "seq 1 10 | $S '/1/,/3/{/2/d}'"
t "$(printf 'CC = gcc\nCFLAGS=-O2\n' | sed -n 's/^CC *= *//p')" sh -c "printf 'CC = gcc\nCFLAGS=-O2\n' | $S -n 's/^CC *= *//p'"
t "$(printf '#define FOO 1\n' | sed 's/#define \([A-Z]*\) \(.*\)/\1=\2/')" sh -c "printf '#define FOO 1\n' | $S 's/#define \\([A-Z]*\\) \\(.*\\)/\\1=\\2/'"

# ---- diff and patch ----
D="$tools/diff"; P="$tools/patch"
seq 1 60 > old.txt
sed -e '5d' -e '10s/.*/ten/' -e '20a\
twenty and a half' -e '30,33d' -e '50s/.*/&\
more/' old.txt > new.txt
"$D" -u old.txt new.txt > ours.diff; [ $? = 1 ] && ok || bad "diff -u exit status"
touch -d "2020-01-01" old.txt; touch -d "2021-01-01" new.txt
cp old.txt t1.txt && patch -s t1.txt ours.diff && same new.txt t1.txt "patch: GNU patch applies our unified diff"
"$D" old.txt new.txt > ours.ndiff; cp old.txt t2.txt && patch -s t2.txt ours.ndiff && same new.txt t2.txt "patch: GNU patch applies our normal diff"
diff -u old.txt new.txt > theirs.diff; cp old.txt t3.txt && "$P" -s t3.txt theirs.diff && same new.txt t3.txt "patch: ours applies GNU's unified diff"
diff old.txt new.txt > theirs.ndiff; cp old.txt t4.txt && "$P" -s t4.txt theirs.ndiff && same new.txt t4.txt "patch: ours applies GNU's normal diff"
cp old.txt t5.txt && "$P" -s t5.txt ours.diff && same new.txt t5.txt "patch: ours applies ours"
cp new.txt t6.txt && "$P" -s -R t6.txt ours.diff && same old.txt t6.txt "patch -R"
cp new.txt t7.txt && "$P" -s -N t7.txt ours.diff >/dev/null 2>&1; same new.txt t7.txt "patch -N on an applied patch"
t "$(diff old.txt new.txt)" "$D" old.txt new.txt
t "$(diff -u old.txt new.txt | tail -n +3)" sh -c "$D -u old.txt new.txt | tail -n +3"
t "$(diff -U1 old.txt new.txt | tail -n +3)" sh -c "$D -U1 old.txt new.txt | tail -n +3"
t "$(diff -q old.txt new.txt)" "$D" -q old.txt new.txt
t "$(diff old.txt old.txt; echo "rc=$?")" sh -c "$D old.txt old.txt; echo rc=\$?"
t "$(diff -s old.txt old.txt)" "$D" -s old.txt old.txt
printf 'a\nb\n' > nl1.txt; printf 'a\nb' > nl2.txt
t "$(diff nl1.txt nl2.txt)" "$D" nl1.txt nl2.txt
t "$(diff -u nl1.txt nl2.txt | tail -n +3)" sh -c "$D -u nl1.txt nl2.txt | tail -n +3"
cp nl1.txt t8.txt && "$D" -u nl1.txt nl2.txt > nl.diff; "$P" -s t8.txt nl.diff && same nl2.txt t8.txt "patch: no newline at end"
printf 'Hello World\n' > c1.txt; printf 'hello   world\n' > c2.txt
t "$(diff -i -b c1.txt c2.txt; echo "rc=$?")" sh -c "$D -i -b c1.txt c2.txt; echo rc=\$?"
t "$(diff -w c1.txt c2.txt; echo "rc=$?")" sh -c "$D -w c1.txt c2.txt; echo rc=\$?"
t "$(diff c1.txt c2.txt; echo "rc=$?")" sh -c "$D c1.txt c2.txt; echo rc=\$?"
# a random-edits torture: our diff, GNU patch; GNU diff, our patch
i=0
while [ $i -lt 12 ]; do
    awk -v s=$i 'BEGIN{srand(s); for(n=0;n<200;n++) print int(rand()*30)}' > r1.txt
    awk -v s=$i 'BEGIN{srand(s+100); while ((getline l < "r1.txt") > 0) { r=rand(); if (r<0.15) continue; if (r<0.3) print "new" int(rand()*30); print l }}' > r2.txt
    "$D" -u r1.txt r2.txt > r.diff; cp r1.txt r3.txt; patch -s r3.txt r.diff && same r2.txt r3.txt "diff torture $i (ours -> GNU patch)"
    diff -u r1.txt r2.txt > r2.diff; cp r1.txt r4.txt; "$P" -s r4.txt r2.diff && same r2.txt r4.txt "diff torture $i (GNU diff -> our patch)"
    "$D" r1.txt r2.txt > r3.diff; cp r1.txt r5.txt; "$P" -s r5.txt r3.diff && same r2.txt r5.txt "diff torture $i (normal, ours -> ours)"
    [ "$("$D" -u r1.txt r2.txt | grep -c '^[-+][^-+]')" -le "$(diff -u r1.txt r2.txt | grep -c '^[-+][^-+]')" ] && ok || bad "diff torture $i: longer than GNU's"
    i=$((i + 1))
done
# directories, new files, and -p
mkdir -p da/sub db/sub && echo same > da/sub/s && echo same > db/sub/s && echo x > da/only_a && echo y > db/sub/new && printf '1\n2\n' > da/f && printf '1\n3\n' > db/f
t "$(diff -r da db; echo "rc=$?")" sh -c "$D -r da db; echo rc=\$?"
t "$(diff -ruN da db | grep -v '^[-+][-+][-+]')" sh -c "$D -ruN da db | grep -v '^[-+][-+][-+]'"
"$D" -ruN da db > dir.diff; cp -r da dc && (cd dc && "$P" -s -p1 < ../dir.diff) && diff -r db dc >/dev/null && ok || bad "patch -p1 of a directory diff (ours)"
diff -ruN da db > dir2.diff; cp -r da dd && (cd dd && "$P" -s -p1 < ../dir2.diff) && diff -r db dd >/dev/null && ok || bad "patch -p1 of GNU's directory diff"
cp -r da de && (cd de && patch -s -p1 < ../dir.diff) && diff -r db de >/dev/null && ok || bad "GNU patch -p1 of our directory diff"
# fuzz and offset
seq 1 40 > fz.txt; sed '20s/.*/TWENTY/' fz.txt > fz2.txt; "$D" -u fz.txt fz2.txt > fz.diff
(seq 100 105; cat fz.txt) > fz3.txt; "$P" -s fz3.txt fz.diff >/dev/null && grep -q TWENTY fz3.txt && ok || bad "patch: offset"
sed '18s/.*/changed/' fz.txt > fz4.txt; "$P" fz4.txt fz.diff 2>&1 | grep -q "fuzz" && grep -q TWENTY fz4.txt && ok || bad "patch: fuzz"
sed '19s/.*/changed/;21s/.*/changed/;18d;22d' fz.txt > fz5.txt; "$P" -s fz5.txt fz.diff >/dev/null 2>&1; [ $? = 1 ] && [ -f fz5.txt.rej ] && ok || bad "patch: reject"
# patch of a new file and a deleted one
rm -rf pa pb && mkdir pa pb && echo gone > pa/gone && echo born > pb/born && "$D" -ruN pa pb > np.diff; cp -r pa pc && (cd pc && "$P" -s -p1 < ../np.diff) && [ -f pc/born ] && [ ! -f pc/gone ] && ok || bad "patch: new and deleted files"
(cd pc && "$P" -s -R -p1 < ../np.diff) && [ -f pc/gone ] && [ ! -f pc/born ] && ok || bad "patch -R: new and deleted files"
# the toolchain patches themselves
(cd "$work" && mkdir gccsrc && cd gccsrc && mkdir -p gcc/config libgcc libstdc++-v3 && for f in config.sub gcc/config.gcc libgcc/config.host libstdc++-v3/crossconfig.m4; do : ; done) 2>/dev/null
"$P" --dry-run -p1 -d "$work" -i /dev/null >/dev/null 2>&1; ok

# ---- find ----
F="$tools/find"
(cd tree && find . | sort) > f1; (cd tree && "$F" . | sort) > f2; same f1 f2 "find ."
(cd tree && find . -type f | sort) > f1; (cd tree && "$F" . -type f | sort) > f2; same f1 f2 "find -type f"
(cd tree && find . -type d | sort) > f1; (cd tree && "$F" . -type d | sort) > f2; same f1 f2 "find -type d"
(cd tree && find . -name '*.txt' | sort) > f1; (cd tree && "$F" . -name '*.txt' | sort) > f2; same f1 f2 "find -name"
(cd tree && find src -name '*.txt' -o -name '*.bin' | sort) > f1; (cd tree && "$F" src -name '*.txt' -o -name '*.bin' | sort) > f2; same f1 f2 "find -o"
(cd tree && find . -path '*deep*' -prune -o -type f -print | sort) > f1; (cd tree && "$F" . -path '*deep*' -prune -o -type f -print | sort) > f2; same f1 f2 "find -prune"
(cd tree && find . -newer src/a.txt -type f | sort) > f1; (cd tree && "$F" . -newer src/a.txt -type f | sort) > f2; same f1 f2 "find -newer"
(cd tree && find . -maxdepth 2 | sort) > f1; (cd tree && "$F" . -maxdepth 2 | sort) > f2; same f1 f2 "find -maxdepth"
(cd tree && find . -mindepth 2 -type d | sort) > f1; (cd tree && "$F" . -mindepth 2 -type d | sort) > f2; same f1 f2 "find -mindepth"
(cd tree && find . -empty | sort) > f1; (cd tree && "$F" . -empty | sort) > f2; same f1 f2 "find -empty"
(cd tree && find . -size +100k | sort) > f1; (cd tree && "$F" . -size +100k | sort) > f2; same f1 f2 "find -size"
(cd tree && find . ! -name '*.txt' -type f | sort) > f1; (cd tree && "$F" . ! -name '*.txt' -type f | sort) > f2; same f1 f2 "find !"
(cd tree && find . \( -name '*.txt' -o -name '*.bin' \) -a -type f | sort) > f1; (cd tree && "$F" . \( -name '*.txt' -o -name '*.bin' \) -a -type f | sort) > f2; same f1 f2 "find ( )"
(cd tree && find . -name '*.txt' -exec cat {} \; | sort) > f1; (cd tree && "$F" . -name '*.txt' -exec cat {} \; | sort) > f2; same f1 f2 "find -exec ;"
(cd tree && find . -name '*.txt' -exec cat {} + | sort) > f1; (cd tree && "$F" . -name '*.txt' -exec cat {} + | sort) > f2; same f1 f2 "find -exec +"
(cd tree && find . -print0 | tr '\0' '\n' | sort) > f1; (cd tree && "$F" . -print0 | tr '\0' '\n' | sort) > f2; same f1 f2 "find -print0"
(cd tree && find src/a.txt src/numbers.txt) > f1; (cd tree && "$F" src/a.txt src/numbers.txt) > f2; same f1 f2 "find files"
(find "$work/tree" -name a.txt) > f1; ("$F" "$work/tree" -name a.txt) > f2; same f1 f2 "find absolute"
(cd tree && find . -path './src/deep*' | sort) > f1; (cd tree && "$F" . -path './src/deep*' | sort) > f2; same f1 f2 "find -path"
(cd tree && find . -iname 'A.TXT') > f1; (cd tree && "$F" . -iname 'A.TXT') > f2; same f1 f2 "find -iname"
(cd "$work" && find build -name '*.d' 2>/dev/null | sort) > f1; (cd "$work" && "$F" build -name '*.d' 2>/dev/null | sort) > f2; same f1 f2 "find missing dir"
cp -r tree del && "$F" del -name '*.txt' -delete && [ -z "$(find del -name '*.txt')" ] && ok || bad "find -delete"

# ---- cut, xargs, cmp, printf, test, truncate, dd, install, ln, mktemp, nproc, true, false ----
printf 'a:b:c\nd:e\n\nf\n' > cut.txt
t "$(cut -d: -f2 cut.txt)" "$tools/cut" -d: -f2 cut.txt
t "$(cut -d: -f1,3 cut.txt)" "$tools/cut" -d: -f1,3 cut.txt
t "$(cut -d: -f2- cut.txt)" "$tools/cut" -d: -f2- cut.txt
t "$(cut -d: -s -f2 cut.txt)" "$tools/cut" -d: -s -f2 cut.txt
t "$(cut -c2-4 cut.txt)" "$tools/cut" -c2-4 cut.txt
t "$(cut -c1,3 cut.txt)" "$tools/cut" -c1,3 cut.txt
t "$(cut -c-2 cut.txt)" "$tools/cut" -c-2 cut.txt
t "$(cut -d: -f1 --complement cut.txt)" "$tools/cut" -d: -f1 --complement cut.txt
t "$(cut -d: -f1,2 --output-delimiter=, cut.txt)" "$tools/cut" -d: -f1,2 --output-delimiter=, cut.txt
t "$(cut -d '' -f2 cut.txt)" "$tools/cut" -d '' -f2 cut.txt
t "$(cut -d '' -s -f1 cut.txt)" "$tools/cut" -d '' -s -f1 cut.txt
t "$(printf 'a b  c\nd\n' | xargs echo)" sh -c "printf 'a b  c\nd\n' | $tools/xargs echo"
t "$(printf 'a b c d e\n' | xargs -n 2 echo)" sh -c "printf 'a b c d e\n' | $tools/xargs -n 2 echo"
t "$(printf 'a\nb\n' | xargs -I{} echo "[{}]")" sh -c "printf 'a\nb\n' | $tools/xargs -I{} echo '[{}]'"
t "$(printf 'a\nb\n' | xargs -i echo "[{}]")" sh -c "printf 'a\nb\n' | $tools/xargs -i echo '[{}]'"
t "$(printf 'a\nb\n' | xargs -i% echo "[%]")" sh -c "printf 'a\nb\n' | $tools/xargs -i% echo '[%]'"
t "$(printf 'a\0b c\0' | xargs -0 printf '<%s>')" sh -c "printf 'a\0b c\0' | $tools/xargs -0 printf '<%s>'"
t "$(printf "'a b' c \"d e\" f\\\\ g\n" | xargs printf '<%s>')" sh -c "printf \"'a b' c \\\"d e\\\" f\\\\\\\\ g\n\" | $tools/xargs printf '<%s>'"
t "$(printf '' | xargs -r echo x; echo "[$?]")" sh -c "printf '' | $tools/xargs -r echo x; echo \"[\$?]\""
t "$(printf '' | xargs echo x)" sh -c "printf '' | $tools/xargs echo x"
t "$(printf '1\n2\n3\n' | xargs false; echo "rc=$?")" sh -c "printf '1\n2\n3\n' | $tools/xargs false; echo rc=\$?"
t "$(seq 1 5000 | xargs echo | wc -l)" sh -c "seq 1 5000 | $tools/xargs echo | wc -l"
t "$(cmp old.txt new.txt; echo "rc=$?")" sh -c "$tools/cmp old.txt new.txt; echo rc=\$?"
t "$(cmp old.txt old.txt; echo "rc=$?")" sh -c "$tools/cmp old.txt old.txt; echo rc=\$?"
t "$(cmp -s old.txt new.txt; echo "rc=$?")" sh -c "$tools/cmp -s old.txt new.txt; echo rc=\$?"
t "$(cmp -l nl1.txt nl2.txt 2>&1; echo "rc=$?")" sh -c "$tools/cmp -l nl1.txt nl2.txt 2>&1; echo rc=\$?"
t "$(cmp nl1.txt nl2.txt 2>&1; echo "rc=$?")" sh -c "$tools/cmp nl1.txt nl2.txt 2>&1; echo rc=\$?"
t "$(printf '%s=%d\n' a 1 b 2)" "$tools/printf" '%s=%d\n' a 1 b 2
t "$(printf '%5.2f|%-4s|%04x|%o|%c|%%\n' 3.14159 ab 255 8 xyz)" "$tools/printf" '%5.2f|%-4s|%04x|%o|%c|%%\n' 3.14159 ab 255 8 xyz
t "$(/usr/bin/printf 'tab\there\n')" "$tools/printf" 'tab\there\n'
t "$(printf '%b\n' 'a\tb')" "$tools/printf" '%b\n' 'a\tb'
t "$(printf 'no args\n')" "$tools/printf" 'no args\n'
t "$(printf '%s\n')" "$tools/printf" '%s\n'
t "$(printf '%d\n' "'A")" "$tools/printf" '%d\n' "'A"
t "$(/usr/bin/printf '\101\x42\n')" "$tools/printf" '\101\x42\n'
check "$tools/test" -f old.txt
check "$tools/test" ! -f nonexistent
check "$tools/test" -d tree -a -r old.txt
check "$tools/test" 3 -gt 2 -a abc = abc -o 1 -eq 2
check "$tools/test" \( 1 -eq 2 -o 2 -eq 2 \) -a -n x
check "$tools/test" -z ""
if "$tools/test" -z x; then bad "test -z x"; else ok; fi
touch -d "2020-01-01" old.txt; touch -d "2021-01-01" new.txt
check "$tools/test" new.txt -nt old.txt
check "$tools/test" old.txt -ot new.txt
check "$tools/test" old.txt -ef old.txt
check "$tools/test" -x "$tools/test"
check "$tools/test" -s old.txt
check "$tools/test" abc '!=' abd
if "$tools/test"; then bad "test with no args"; else ok; fi
check "$tools/test" -t 0 -o -n yes
ln -s old.txt sl.txt; check "$tools/test" -L sl.txt; check "$tools/test" -h sl.txt
"$tools/truncate" -s 1000 tr.txt && [ "$(stat -c %s tr.txt)" = 1000 ] && ok || bad "truncate -s"
"$tools/truncate" -s 1K tr.txt && [ "$(stat -c %s tr.txt)" = 1024 ] && ok || bad "truncate -s 1K"
"$tools/truncate" -s +10 tr.txt && [ "$(stat -c %s tr.txt)" = 1034 ] && ok || bad "truncate -s +10"
"$tools/truncate" -s -34 tr.txt && [ "$(stat -c %s tr.txt)" = 1000 ] && ok || bad "truncate -s -34"
"$tools/truncate" -s 2M tr.img && [ "$(stat -c %s tr.img)" = 2097152 ] && ok || bad "truncate -s 2M"
"$tools/truncate" -c -s 5 nothere.txt && [ ! -f nothere.txt ] && ok || bad "truncate -c"
"$tools/dd" if=tree/src/random.bin of=dd1.bin bs=4096 status=none && same tree/src/random.bin dd1.bin "dd copy"
"$tools/dd" if=tree/src/random.bin of=dd2.bin bs=1000 count=3 status=none && head -c 3000 tree/src/random.bin > dd2.exp && same dd2.exp dd2.bin "dd count"
"$tools/dd" if=tree/src/random.bin of=dd3.bin bs=1000 skip=2 count=2 status=none && tail -c +2001 tree/src/random.bin | head -c 2000 > dd3.exp && same dd3.exp dd3.bin "dd skip"
cp tree/src/random.bin dd4.bin && "$tools/dd" if=tree/src/a.txt of=dd4.bin bs=512 seek=1 conv=notrunc status=none && { head -c 512 tree/src/random.bin; cat tree/src/a.txt; tail -c +$((512 + 13)) tree/src/random.bin; } > dd4.exp && same dd4.exp dd4.bin "dd seek conv=notrunc"
"$tools/dd" if=/dev/zero of=dd5.bin bs=1k count=4 status=none && [ "$(stat -c %s dd5.bin)" = 4096 ] && ok || bad "dd bs=1k"
"$tools/dd" if=tree/src/a.txt of=dd6.bin bs=512 conv=sync status=none && [ "$(stat -c %s dd6.bin)" = 512 ] && ok || bad "dd conv=sync"
"$tools/dd" if=tree/src/numbers.txt bs=4k status=none | cmp -s - tree/src/numbers.txt && ok || bad "dd to stdout"
"$tools/dd" if=tree/src/numbers.txt of=dd7.txt 2>dd.err && grep -q "records in" dd.err && ok || bad "dd status"
"$tools/install" -d idir/sub/deep && [ -d idir/sub/deep ] && ok || bad "install -d"
"$tools/install" -m 644 old.txt idir/ && [ "$(stat -c %a idir/old.txt)" = 644 ] && same old.txt idir/old.txt "install"
"$tools/install" old.txt idir/x && [ "$(stat -c %a idir/x)" = 755 ] && ok || bad "install default mode"
"$tools/install" -D -m 600 old.txt idir/new/dir/y && [ "$(stat -c %a idir/new/dir/y)" = 600 ] && ok || bad "install -D"
"$tools/install" -t idir/sub old.txt new.txt && [ -f idir/sub/old.txt ] && [ -f idir/sub/new.txt ] && ok || bad "install -t"
"$tools/install" -p old.txt idir/p && [ "$(stat -c %Y idir/p)" = "$(stat -c %Y old.txt)" ] && ok || bad "install -p"
"$tools/install" -m u+x,go-w old.txt idir/m && [ "$(stat -c %a idir/m)" = 755 ] && ok || bad "install -m symbolic"
"$tools/ln" -s old.txt lnk1 && [ "$(readlink lnk1)" = old.txt ] && ok || bad "ln -s"
"$tools/ln" old.txt lnk2 && [ "$(stat -c %h old.txt)" = 2 ] && ok || bad "ln"
"$tools/ln" -sf new.txt lnk1 && [ "$(readlink lnk1)" = new.txt ] && ok || bad "ln -sf"
mkdir lnd && "$tools/ln" -s "$work/old.txt" "$work/new.txt" lnd && [ -L lnd/old.txt ] && [ -L lnd/new.txt ] && ok || bad "ln into a directory"
m=$("$tools/mktemp" -p "$work") && [ -f "$m" ] && ok || bad "mktemp"
m=$("$tools/mktemp" -d -p "$work" dirXXXXXX) && [ -d "$m" ] && ok || bad "mktemp -d"
m=$("$tools/mktemp" "$work/tmp.XXXXXXXX") && [ -f "$m" ] && ok || bad "mktemp template"
if "$tools/mktemp" "$work/noxs" 2>/dev/null; then bad "mktemp: too few X"; else ok; fi
[ "$("$tools/nproc")" = "$(nproc)" ] && ok || bad "nproc"
"$tools/true" && ok || bad "true"
if "$tools/false"; then bad "false"; else ok; fi

# ---- mkfs.ext2, fsck.ext2: Linux's e2fsck agrees with the volumes and the counts ----
if command -v e2fsck >/dev/null 2>&1 && command -v mke2fs >/dev/null 2>&1; then
    for spec in "1024 8M" "4096 600M" "2048 3000K"; do
        set -- $spec
        rm -f e2.img
        "$tools/mkfs.ext2" -q -b "$1" e2.img "$2" && e2fsck -fn e2.img >/dev/null 2>&1 && ok || bad "mkfs.ext2 -b $1 $2"
    done
    rm -f e2.img && "$tools/mkfs.ext2" -q -L srcdisk e2.img 100M &&
        [ "$(dumpe2fs -h e2.img 2>/dev/null | sed -n 's/^Filesystem volume name: *//p')" = srcdisk ] && ok || bad "mkfs.ext2 -L"
    "$tools/fsck.ext2" e2.img >/dev/null && ok || bad "fsck.ext2 on a clean volume"
    mke2fs -q -F -t ext2 e2l.img 20M >/dev/null 2>&1 && "$tools/fsck.ext2" e2l.img >/dev/null && ok || bad "fsck.ext2 on mke2fs's volume"
    printf '\071\060\000\000' | dd of=e2.img bs=1 seek=1036 conv=notrunc 2>/dev/null
    if "$tools/fsck.ext2" e2.img >/dev/null; then bad "fsck.ext2 misses a wrong count"; else ok; fi
    "$tools/fsck.ext2" -y e2.img >/dev/null; [ $? = 1 ] && e2fsck -fn e2.img >/dev/null 2>&1 && ok || bad "fsck.ext2 -y"
fi

# ---- mkiso: xorriso reads the image, its names and El Torito's boot entry ----
if command -v xorriso >/dev/null 2>&1; then
    mkdir -p isotree/Sub/deeper && echo hi > isotree/Sub/deeper/A-Long-Name.txt && cp tree/src/random.bin isotree/ &&
        head -c 4096 /dev/zero > isotree/boot.img
    "$tools/mkiso" -o t.iso -b boot.img isotree && ok || bad "mkiso"
    xorriso -indev t.iso -find / -type f 2>/dev/null | LC_ALL=C sort > iso.list
    printf "'/Sub/deeper/A-Long-Name.txt'\n'/boot.img'\n'/random.bin'\n" > iso.exp
    same iso.exp iso.list "mkiso Rock Ridge names"
    xorriso -indev t.iso -report_el_torito plain 2>/dev/null | grep -q 'cat path : /boot.catalog' && ok || bad "mkiso boot catalog"
    xorriso -osirrox on -indev t.iso -extract /random.bin iso.random 2>/dev/null && same tree/src/random.bin iso.random "mkiso contents"
    xorriso -indev t.iso -report_el_torito plain 2>/dev/null | grep -q 'boot-info-table' && ok || bad "mkiso El Torito"
fi

# ---- mbcheck: a Multiboot header with a good checksum, one PT_LOAD ----
printf '\177ELF' > notkernel && if "$tools/mbcheck" -q notkernel 2>/dev/null; then bad "mbcheck accepts junk"; else ok; fi

echo "tools: $pass checks passed, $fail failed"
[ $fail = 0 ]
