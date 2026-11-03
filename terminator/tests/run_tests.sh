#!/bin/bash
# Engine tests: run each command line through Terminator's shell engine
# (no window) and compare with the expected output.
#   make test      or      ./tests/run_tests.sh
cd "$(dirname "$0")/.." || exit 1
T="$PWD/terminator"
[ -x "$T" ] || { echo "build first: make"; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
cd "$W" || exit 1
touch abc.txt abcd.txt def.txt
printf 'b\na\nc\nb\n' > names.txt
cat > check.sh <<'EOF'
#!/bin/bash
if [ -f "$1" ]; then
    echo "File exists, compiling..."
    if [ "$2" = "x" ]; then echo inner-x; elif [ "$2" = "y" ]; then echo inner-y; else echo inner-else; fi
else
    echo "Source file missing!"
fi
echo "argc=$#"
exit 3
EOF
chmod +x check.sh
printf 'v=sourced\n' > vars.sh

pass=0; fail=0
check() {   # check "description" "command line" "expected output"
    local got
    got=$(HOME="$W" "$T" -c "$2" 2>&1)
    if [ "$got" == "$3" ]; then
        pass=$((pass + 1)); printf '  ok    %s\n' "$1"
    else
        fail=$((fail + 1)); printf '  FAIL  %s\n        expected: %q\n        got:      %q\n' "$1" "$3" "$got"
    fi
}

echo "Task 2: commands"
check "external command"          'echo hello'                          'hello'
check "exit status \$?"           'false; echo $?'                      '1'
check "unknown command"           'nosuchcmd; echo $?'                  $'terminator: nosuchcmd: command not found\n127'
check "cd and pwd"                'cd /usr; pwd'                        '/usr'
check "cd -"                      'cd /usr; cd /tmp; cd - >/dev/null; pwd' '/usr'
check "variables"                 'x=5; echo "x is $x"'                 'x is 5'
check "&& and ||"                 'false && echo a || echo b'           'b'

echo "Task 3: quoting and expansion"
check "single quotes literal"     "echo '\$HOME'"                       '$HOME'
check "double quotes keep spaces" 'echo "a   b"'                        'a   b'
check "nested \$(...)"            'echo "in $(basename $(echo /x/mydir))"' 'in mydir'
check "line continuation"         $'echo one \\\ntwo'                   'one two'
check "newline inside quotes"     $'echo "hello\nworld"'                $'hello\nworld'
check "glob"                      'echo *.txt'                          'abc.txt abcd.txt def.txt names.txt'

echo "Task 4: redirection"
check "< and >"                   'sort < names.txt > sorted.out; cat sorted.out' $'a\nb\nb\nc'
check ">> appends"                'echo 1 > f; echo 2 >> f; cat f'      $'1\n2'
check "2> and 2>&1"               'ls nofile 2>/dev/null; ls nofile 2>&1 | wc -l' '1'
check "missing input file"        'sort < nofile; echo $?'              $'terminator: nofile: No such file or directory\n1'

echo "Task 5: pipes"
check "two commands"              'ls *.txt | wc -l'                    '4'
check "three commands"            'ls *.txt | sort -r | head -2'        $'names.txt\ndef.txt'
check "pipe + redirection"        'ls *.txt | sort > list.out; head -1 list.out' 'abc.txt'
check "early reader exit"         'yes | head -2'                       $'y\ny'
check "builtin in a pipeline"     'pwd | wc -l'                         '1'

echo "Task 11: scripts"
check "./script args + exit n"    './check.sh abc.txt y; echo rc=$?'    $'File exists, compiling...\ninner-y\nargc=2\nrc=3'
check "bash script.sh"            'bash check.sh nofile; echo rc=$?'    $'Source file missing!\nargc=1\nrc=3'
check "nested else"               'sh check.sh abc.txt z | head -2'     $'File exists, compiling...\ninner-else'
check "source keeps variables"    'source vars.sh; echo $v'             'sourced'
check "not executable"            'chmod -x check.sh; ./check.sh; echo $?' $'terminator: ./check.sh: Permission denied\n126'

echo "Syntax errors"
check "unexpected fi"             'fi'                                  "terminator: syntax error near unexpected token \`fi'"
check "unterminated quote"        'echo "abc'                           'terminator: syntax error: unexpected end of file'

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
