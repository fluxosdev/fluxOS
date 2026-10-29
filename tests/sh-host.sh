#!/bin/sh
# fsh as /bin/sh, built for the host, against the host's own /bin/sh:
# the same scripts must give the same output and status.
#   tests/sh-host.sh BUILT_SH          ("make test-sh")
set -u
ours=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
ref=/bin/sh
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
pass=0
fail=0
# t NAME SCRIPT [ARGS...]: run the script with both shells, compare stdout and the status
t() {
    name=$1; script=$2; shift 2
    exp=$($ref -c "$script" sh "$@" 2>/dev/null; echo "[$?]")
    got=$("$ours" -c "$script" sh "$@" 2>/dev/null; echo "[$?]")
    if [ "$exp" = "$got" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); printf 'FAIL: %s\n  expected: %s\n  got:      %s\n' "$name" "$(printf '%s' "$exp" | tr '\n' '|')" "$(printf '%s' "$got" | tr '\n' '|')"; fi
}
# te NAME EXPECTED SCRIPT: compare with a fixed expectation
te() {
    name=$1; exp=$2; script=$3
    got=$("$ours" -c "$script" sh 2>/dev/null; echo "[$?]")
    if [ "$exp" = "$got" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); printf 'FAIL: %s\n  expected: %s\n  got:      %s\n' "$name" "$(printf '%s' "$exp" | tr '\n' '|')" "$(printf '%s' "$got" | tr '\n' '|')"; fi
}

printf 'alpha\nbeta\ngamma\n' > lines.txt
mkdir -p d && : > d/a.c && : > d/b.c && : > d/c.h

t "echo" 'echo hello world'
t "status" 'false; echo $?; true; echo $?'
t "exit code" 'exit 3'
t "args" 'echo $# "$1" "$2" "$@"' one "two words"
t "args star" 'for a in "$*"; do echo "[$a]"; done' x y
t "args at" 'for a in "$@"; do echo "[$a]"; done' x "y z"
t "empty at" 'set --; for a in "$@"; do echo "[$a]"; done; echo done'
t "shift" 'shift; echo "$1"; shift 2; echo "$#"' a b c d
t "set --" 'set -- p q r; echo "$2 $#"'
t "split" 'x="a b  c"; for w in $x; do echo "[$w]"; done'
t "nosplit quoted" 'x="a b  c"; for w in "$x"; do echo "[$w]"; done'
t "IFS" 'IFS=:; x="a:b::c"; for w in $x; do echo "[$w]"; done'
t "IFS newline" 'x="a
b"; for w in $x; do echo "[$w]"; done'
t "cmdsubst split" 'for w in $(echo "1 2 3"); do echo "[$w]"; done'
t "backquote" 'x=`echo hi`; echo "$x"'
t "nested subst" 'echo $(echo $(echo deep))'
t "glob" 'echo d/*.c'
t "glob nomatch" 'echo d/*.z'
t "glob quoted" 'echo "d/*.c"'
t "noglob" 'set -f; echo d/*.c'
t "brace literal" 'echo a{b,c}'
t "tilde" 'HOME=/hh; echo ~ ~/x'
t "default" 'unset x; echo ${x:-dflt} ${x-dflt2} "${y:=set}" "$y"'
t "alt" 'x=1; echo ${x:+alt} ${z:+alt}'
t "length" 'x=hello; echo ${#x} ${#}' a b c
t "hash" 'x=/usr/local/bin/gcc; echo ${x#*/} ${x##*/} ${x%/*} ${x%%/*}'
t "hash pattern" 'x=file.tar.gz; echo ${x%.*} ${x%%.*} ${x#*.}'
t "hash quoted" 'x="a*b"; echo "${x#a}" "${x#"a*"}"'
t "special brace" 'false; echo ${?} ${#} ${1}' arg
t "arith" 'x=5; echo $((x * 2 + 1)) $((x % 3)) $(( (1+2)*3 ))'
t "case" 'for x in apple banana cherry; do case $x in a*) echo A;; b*|c*) echo BC;; esac; done'
t "case default" 'case foo in bar) echo no;; *) echo yes;; esac'
t "case quoted" 'case "a*" in "a*") echo lit;; *) echo glob;; esac'
t "case paren" 'case x in (x) echo px;; esac'
t "case empty" 'case "" in "") echo empty;; esac'
t "case newline" 'case 1 in
1)
  echo one
  ;;
esac'
t "case multiline body" 'case x in x) echo a
echo b;; esac'
t "case host" 'host=i686-pc-fluxos; case $host in *-*-linux*) echo linux;; *-fluxos*) echo flux;; esac'
t "case class" 'case 7 in [0-9]) echo digit;; esac'
t "heredoc" 'cat <<EOF
hello $USER
EOF'
t "heredoc quoted" 'x=1; cat <<"EOF"
no $x expansion
EOF'
t "heredoc strip" 'cat <<-EOF
		indented
	EOF'
t "heredoc subst" 'cat <<EOF
$(echo sub) `echo bq` \$esc \\
EOF'
t "heredoc two" 'cat <<A; cat <<B
first
A
second
B'
t "heredoc in func" 'f() { cat <<EOF
in func $1
EOF
}; f arg'
t "if" 'if true; then echo yes; else echo no; fi; if false; then echo yes; elif true; then echo elif; fi'
t "while" 'i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done'
t "until" 'i=0; until [ $i -ge 2 ]; do echo $i; i=$((i+1)); done'
t "for" 'for i in 1 2 3; do echo $i; done'
t "for noin" 'for i; do echo $i; done' x y
t "break continue" 'for i in 1 2 3 4; do [ $i = 2 ] && continue; [ $i = 4 ] && break; echo $i; done'
t "func" 'f() { echo "f $1 $#"; return 7; }; f a b; echo $?'
t "func local" 'x=global; f() { local x=local; echo $x; }; f; echo $x'
t "func local unset" 'f() { local y; y=1; echo $y; }; f; echo "[${y:-}]"'
t "func recursion" 'f() { [ $1 -le 0 ] && return; echo $1; f $(($1-1)); }; f 3'
t "test" '[ -f lines.txt ] && echo f; [ -d d ] && echo d; [ ! -e nope ] && echo ne; [ 3 -gt 2 ] && echo gt; test a = a && echo eq'
t "test strings" '[ -z "" ] && echo z; [ -n x ] && echo n; [ "a" != "b" ] && echo ne'
t "test combined" '[ 1 -eq 1 -a 2 -eq 2 ] && echo and; [ 1 -eq 2 -o 2 -eq 2 ] && echo or'
t "set -e" 'set -e; echo a; false; echo b'
t "set -e cond" 'set -e; if false; then :; fi; false || echo ok; ! false; echo end'
t "set -e and" 'set -e; false && echo no; echo still'
t "set -e func" 'set -e; f() { false; echo in; }; if f; then echo t; fi; echo after'
t "set -e subshell" 'set -e; (false); echo nope'
t "set -e pipe" 'set -e; false | true; echo ok'
t "set -u" 'set -u; echo ${x:-d}; echo $y; echo after'
t "set -x" 'set -x; echo hi 2>/dev/null'
t "set +e" 'set -e; set +e; false; echo ok'
t "set -o" 'set -o errexit; false; echo no'
t "trap exit" 'trap "echo bye" EXIT; echo main'
t "trap exit code" 'trap "echo t" EXIT; exit 4'
t "trap int" 'trap "echo got" INT; kill -INT $$; echo after'
t "trap reset" 'trap "echo x" EXIT; trap - EXIT; echo none'
t "trap ignore" 'trap "" INT; kill -INT $$; echo alive'
t "trap list" 'trap "echo a" EXIT; trap'
t "getopts" 'while getopts ab:c opt; do echo "$opt:${OPTARG:-}"; done; shift $((OPTIND-1)); echo "rest $*"' -a -b val -c x y
t "getopts bundle" 'while getopts abc opt; do echo $opt; done' -abc
t "getopts silent" 'while getopts :a opt; do echo "$opt ${OPTARG:-}"; done' -z
t "getopts attached" 'while getopts o: opt; do echo "$opt=$OPTARG"; done' -ofile
t "command -v" 'command -v echo; command -v cat >/dev/null && echo cat; command -v nonexistent_zzz || echo none'
t "command" 'echo() { builtin_was_not_called=1; }; command echo direct'
t "redirect" 'echo out > f.txt; cat f.txt; echo app >> f.txt; wc -l < f.txt'
t "redirect fd" 'echo err 1>&2 2>/dev/null; echo visible'
t "redirect both" 'sh -c "echo o; echo e >&2" > both.txt 2>&1; cat both.txt'
t "redirect in block" '{ echo a; echo b; } > blk.txt; cat blk.txt'
t "redirect loop" 'for i in 1 2; do echo $i; done > loop.txt; cat loop.txt'
t "pipe" 'printf "b\na\nc\n" | sort | head -2'
t "pipe status" 'true | false; echo $?; false | true; echo $?'
t "subshell" '(cd d && echo "$(basename "$PWD")"); echo "$(basename "$PWD")" | sed "s/^tmp.*/tmp/"'
t "subshell var" 'x=1; (x=2); echo $x'
t "and or" 'true && echo a || echo b; false && echo c || echo d'
t "not" '! false && echo ok; ! true || echo ok2'
t "eval" 'eval "echo ev; x=5"; echo $x'
t "source" 'echo "y=42" > s.sh; . ./s.sh; echo $y'
t "read" 'printf "a b c\n" | { read x y; echo "[$x][$y]"; }'
t "read lines" 'while read l; do echo "<$l>"; done < lines.txt'
t "printf" 'printf "%s-%d-%05.1f\n" a 7 3.14159; printf "%s\n" x y'
t "printf b" 'printf "%b" "a\tb\n"'
t "echo -n" 'echo -n a; echo b'
t "export" 'export X=1; sh -c "echo \$X"'
t "env assign" 'X=2 sh -c "echo \$X"; echo "[${X:-}]"'
t "unset" 'x=1; unset x; echo "[${x:-}]"'
t "unset func" 'f() { echo f; }; unset -f f; f 2>/dev/null || echo gone'
t "umask" 'umask 022; umask'
t "pwd" 'cd d; pwd | sed "s|.*/||"'
t "dollar0" 'echo "$0"'
t "exit in func" 'f() { exit 5; }; f; echo no'
t "colon" ': ignored; echo ok'
t "comment" 'echo a # comment
echo b'
t "semicolon" 'echo a; echo b;'
t "line continuation" 'echo a \
b'
t "dollar in single" 'echo '"'"'$x'"'"
t "backslash" 'echo a\ b \$x \\'
t "empty var removed" 'x=; set -- a $x b; echo $#'
t "quoted empty kept" 'x=; set -- a "$x" b; echo $#'
t "at nothing" 'set --; set -- "$@"; echo $#'
t "configure-ish" 'ac_x=yes; test "${ac_x+set}" = set && echo set; test -z "${ac_y:-}" && echo empty; for ac_arg in "$@"; do case $ac_arg in --prefix=*) prefix=${ac_arg#--prefix=};; esac; done; echo "$prefix"' --prefix=/usr/local
t "make-ish" 'cd d && for f in *.c; do o=${f%.c}.o; echo "cc -c $f -o $o"; done'
t "make exit" 'test -d nope || mkdir -p nope; test -d nope && echo made'
t "mktemp pattern" 'x=tmp.abc; echo ${x##*.}'
t "nested case func" 'f() { case $1 in a) echo A;; *) echo other;; esac; }; f a; f b'
t "dsemi spaced" 'case x in x) echo ok ;; esac'
t "case fallthrough none" 'case z in a) echo a;; esac; echo $?'
t "while read split" 'printf "k=v\n" | while IFS== read k v; do echo "$k|$v"; done'
t "exec fd" 'exec 3>fd3.txt; echo three >&3; exec 3>&-; cat fd3.txt'
t "rdwr" 'echo data > rw.txt; exec 4<>rw.txt; read x <&4; echo $x; exec 4<&-'
t "set -e in while cond" 'set -e; i=0; while [ $i -lt 2 ]; do i=$((i+1)); done; echo $i'
t "errexit last" 'set -e; echo a; exit 0'
t "hash builtin" 'hash -r; echo ok'
t "if pipeline" 'if echo x | grep -q x; then echo found; fi'
t "quoted heredoc delim backslash" 'cat <<\EOF
$notexpanded
EOF'
t "heredoc then command" 'cat <<EOF | tr a-z A-Z
lower
EOF
echo after'
t "ifs restore" 'IFS=:; set -- a:b; for w in $1; do echo $w; done; unset IFS; set -- "a b"; for w in $1; do echo $w; done'
te "sh name" 'sh[]
[0]' 'printf "%s[%s]\n" "$(basename "$0")" ""'
got=$("$ours" -ec 'echo one; false; echo two' 2>/dev/null; echo "[$?]")
if [ "$got" = "one
[1]" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: -ec flag: $got"; fi
got=$("$ours" -n -c 'echo RAN' 2>&1; echo "[$?]")
if [ "$got" = "[0]" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: -n: $got"; fi

echo "sh: $pass checks passed, $fail failed"
[ $fail = 0 ]
