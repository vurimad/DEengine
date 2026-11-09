# console

The engine's console (docs/design.md, AI-friendly: the command API): named commands and variables, run from text. A person types them in the console window (`, `tools/console_ui.h`), a launch gives them with `--exec`, an agent sends them over the agents' socket (`tools/agent_server.h`, `tools/console.py`). All three run the same text the same way.

## Contents

| File | What it gives |
| --- | --- |
| `console.h` | `Console`: commands (a handler and its user pointer) and variables (bool, int, float, where they live); `execute` a line; `complete` a name; the log (the last `kLogLines` lines said); `Args` (a command's words, read as booleans and numbers), `Output` (what a command says back) |

## The language

- A line holds statements split by `;` (outside quotes): `stat gpu; r.taa 0; camera.goto 10 2 40`.
- A statement's first word names a command or a variable, the rest are its arguments; quotes group words (`"a b"`) and go.
- Names are matched in any case. A variable alone says its value (`r.taa` → `r.taa = on`); with a value it takes it, or refuses what it can't hold (out of range, not a number).
- An unknown name fails, naming what it might have been. `help` lists everything; `help NAME` says what one takes and does.

## Rules and invariants

- Main thread only. Running never blocks; a command that has work to do later sets something the frame acts on (`camera.goto` places the camera next frame).
- Every line that runs is logged (`console: > stat gpu`), whoever ran it, and what fails is a warning: an agent's changes are on the record.
- Whoever adds commands with a `user` pointer removes them (`Console::remove(user)`) before that pointer dies: an application's go when it shuts down.
- Names are `system.what` (`r.taa`, `camera.goto`), as Unreal's are; `stat` keeps Unreal's own spelling.
- No allocation while nothing runs; a line that runs may allocate (Tools heap): it's never per frame.
