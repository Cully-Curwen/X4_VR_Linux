# UI checklists

The menu (`x4vr` with no command) needs a terminal and keys, so CI can't run it. Before committing a
change under `tools/linux/ui/`, run the matching checklist here, by hand or scripted, and say so in
the commit message.

| Checklist | Covers |
|---|---|
| [menu.md](menu.md) | the main menu, profiles, the checklist, tiling and uninstall screens |

## Setup: a fake home with an installed build

Nothing outside `$T` is read or written. The installed copy has the built-in profiles and defaults
(`share/x4vr`), which the build folder lacks.

```bash
cmake --build build/clangd                       # all of it: the install needs the mod too
T=$(mktemp -d)
DESTDIR=$T/install cmake --install build/clangd/linux >/dev/null
X4VR=$(find "$T/install" -path '*/bin/x4vr')
mkdir "$T/home"
STATE=$T/home/.local/state/x4vr                  # stereo.txt, profile, profiles/
```

Start it with an empty environment, so your own `XDG_*` folders and settings are never used:

```bash
env -i HOME="$T/home" PATH="$PATH" TERM="$TERM" "$X4VR"
```

Afterwards: `rm -rf "${T:?}"`.

Items marked **REAL** run in your own home instead (`x4vr` as installed): they only look, change
nothing, and then Quit.

## By hand

Start it as above in a terminal at least 120×50. Follow each item; check the screen and, where an
item names a file, check it in `$STATE` from a second terminal.

## Scripted (tmux), for an AI agent or a script

Run in a detached tmux session of fixed size, send keys, read the screen as text:

```bash
tmux new-session -d -s x4vr -x 120 -y 50 "env -i HOME=$T/home PATH=$PATH TERM=xterm-256color $X4VR"

screen()   { tmux capture-pane -p -t x4vr; }                       # the screen as text
selected() { tmux capture-pane -e -p -t x4vr | grep -F $'\e[7m' | sed 's/\x1b\[[0-9;]*m//g'; }  # the highlighted row
key()      { tmux send-keys -t x4vr "$@"; sleep 0.3; }              # e.g. key Enter, key Right, key Escape
type()     { local s=$1 i; for ((i=0;i<${#s};i++)); do tmux send-keys -t x4vr -l "${s:i:1}"; sleep 0.05; done; }
go()       { tmux send-keys -t x4vr Home; sleep 0.1                 # move to the row whose text contains $1
             for i in $(seq 80); do selected | grep -qF -- "$1" && return 0; tmux send-keys -t x4vr Down; sleep 0.05; done
             echo "NOT FOUND: $1"; return 1; }
```

- `type` sends one character at a time: the menu reads one key per keypress, so text sent at once
  loses characters.
- In a prompt, clear the suggested name first: `for i in $(seq 12); do key BSpace; done`.
- `go` before every `key Enter`, and only press Enter if it found the row: Enter on the wrong row acts
  on it (the last row is Quit). Match the row as shown, with its `▸` for actions: `go '▸ Uninstall'`.

End with `tmux kill-session -t x4vr`.

## Reporting

One line per item: `PASS` or `FAIL`. For each FAIL, the `screen` output and the files checked.
