# Demo: every feature in order

Start in a scratch directory:

```sh
mkdir -p ~/lab && cd ~/lab && touch abc.txt abcd.txt def.txt
printf 'b\na\nc\nb\n' > names.txt
cp /path/to/terminator/examples/check.sh . && chmod +x check.sh
/path/to/terminator/terminator          # or: terminator useProcess=True
```

## 01 Window and tabs
- `Ctrl+Shift+T` (or click `+`) opens tab 2. `cd /tmp` there; tab 1 stays in `~/lab`.
- `Ctrl+Page_Up` / `Ctrl+Page_Down` or a click switches tabs.
- In tab 2: `sleep 3; echo done in tab 2`, switch to tab 1 at once, wait, switch back: the output is there.

## 02 Commands
```
ls -l
cd ..
cd -
pwd
nosuchcmd
echo $?
```

## 03 Multiline input
```
echo "hello          <Enter>
world"               <Enter>
echo one \           <Enter>
two                  <Enter>
echo "Files in $(basename $(pwd)): $(ls | \
wc -l)"
echo '$HOME' "$HOME"
```

## 04 Redirection
```
sort < names.txt > out.txt
cat out.txt
echo more >> out.txt
sort < missing.txt
ls nofile 2> err.txt; cat err.txt
```

## 05 Pipes
```
ls *.txt | wc -l
ls *.txt | sort -r | head -2
ls *.txt | sort > sorted.txt
yes | head -3
```

## 06 multiWatch
```
multiWatch ["date +%T", "ls | wc -l"]
```
Wait a few seconds, then `Ctrl+C`.

## 07 Hotkeys
```
bind -l
bind clear-screen Ctrl+Shift+L
bind nonsense Ctrl+Q
```
`Ctrl+Shift+L` now clears; restart Terminator and it still does.

## 08 Signals and jobs
```
sleep 100            then Ctrl+C
sleep 100            then Ctrl+Z
jobs
bg %1
jobs
fg %1                then Ctrl+C
sleep 2 &
echo hi              (press Enter after 2 s: [1]+ Done sleep 2)
cat                  type lines, then Ctrl+D
```

## 09 History
```
history
history 5
```
- `↑`/`↓` browse. Type something in tab 1, press `↑` in tab 2.
- `Ctrl+R`, then `sort` (no exact match: longest-common-substring hits),
  `ls -l` (exact), `zzzz` (no match).

## 10 Auto-complete
```
./myprog de<Tab>          -> ./myprog def.txt
./myprog def.txt abc<Tab> -> 1. abc.txt   2. abcd.txt, press 1
```

## 11 Scripts
```
cat check.sh
./check.sh abc.txt
bash check.sh missing.c
source check.sh abc.txt
```

## Both modes
Repeat any of the above after `./terminator useProcess=True`.
In process mode, `kill -SEGV <tab pid>` from another terminal closes only that tab.
