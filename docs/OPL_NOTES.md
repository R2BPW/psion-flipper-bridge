# EPOC OPL Compilation Notes

Hard-won knowledge from compiling OPL for the Psion MC218 (EPOC R5) using OPLTRAN.EXE under Wine.

## Build workflow

```sh
unix2dos file.opl                                    # DOS line endings required
wine /path/to/Epoc32/Release/Winc/Deb/OPLTRAN.EXE "C:\\file.opl"
cp file.opo /path/to/Psion/Documents/               # transfer to device
```

Files must be in Wine's `C:\` root (`~/.wine/drive_c/`). Subdirectories cause "Folder not found".

## Syntax restrictions (confirmed by compilation)

### Declarations

- **Strings need explicit length:** `LOCAL s$(64)` not `LOCAL s$`
- **No mixed types on one line:** `LOCAL s$(32), n%` fails. Put strings and integers on separate LOCAL/GLOBAL lines.
- **GLOBAL before executable code:** All GLOBAL declarations must precede any executable statements in a PROC.

### Procedure calls

- **No-arg calls without parens:** `Proc:` not `Proc:()`
- **String-returning procs need `$` suffix:** `PROC Foo$:(x%)` if it uses `RETURN s$`
- **Parameters are read-only:** `x% = 1` inside `PROC P:(x%)` causes "Bad assignment". Copy to a local first.

### Operators

- **MOD does not exist:** `x% MOD n` causes "Syntax error". Use `x% - (x%/n * n)`.
- **ENDWH not WEND:** WHILE loops close with `ENDWH`.
- **gFILL needs 3 args:** `gFILL w, h, 0` (mode 0 = fill with current gGREY color).
- **FREE does not exist:** ALLOC'd memory is freed on program exit. Just zero the pointer.
- **UPPER$ not UCASE$:** Use `UPPER$(s$)` for uppercase conversion.

### Serial I/O

- **IOC + PAUSE polling doesn't work for timeouts:** The status variable is not updated during `PAUSE`. Only `IOWAITSTAT` can wait for async completion, but it has no timeout.
- **Blocking IOW is reliable:** `IOW(-1, 1, #pbuf&, ln%)` blocks until data arrives. Use EOT byte (0x04) as end-of-message signal instead of trying to implement timeouts.

### Reserved words (cause "Declaration error" or "Syntax error")

- `cmd$` — conflicts with CMD$ keyword. Use `op$` or similar.
- `hex$` — conflicts with HEX$() function. Use `hx$`.
- `menu` — conflicts with MENU keyword. Cannot use as label name.
- `state%` — works as variable, listed here as caution (no conflict found).

### LOCAL placement

- **ALL LOCAL/GLOBAL declarations must be at the TOP of a PROC**, before any executable statements. Declaring LOCAL mid-procedure causes "Structure fault".

### TRAP limitations

- **TRAP only works before OPL keywords:** TRAP DELETE, TRAP LOADM, TRAP OPEN, etc.
- **TRAP does NOT work before procedure calls:** `TRAP MyProc:` causes "Syntax error". Use ONERR instead.

### GOTO inside IF

- GOTO from inside IF/ENDIF to a label outside may cause "Missing label" error. Restructure code to avoid GOTO inside IF blocks. Move GOTO after ENDIF.

### LOADM (loading OPO modules)

- `LOADM "file.opo"` loads an OPO as a procedure library.
- Procedures in the loaded module become callable by name.
- **GLOBAL variables are NOT shared** between caller and loaded module. Each module has its own GLOBAL namespace.
- **Inter-module communication:** Pass address via parameter. Caller passes `ADDR(result$)`, module writes via `POKE$ addr&, text$`.
- Module convention: define `PROC Task:(buf&)` as entry point, no `Main:`.
- Use `TRAP UNLOADM "file.opo"` to unload after use.

### Binary file I/O

- `IOOPEN(handle%, name$, mode%)`: mode 2 = create/replace as binary stream.
- `IOW(handle%, 2, #buf&, len%)`: write raw bytes. **Must use # before buffer variable.**
- `IOCLOSE(handle%)`: close file.
- `LOPEN`/`LPRINT` creates **text** files with EPOC text headers — wrong for binary .opo files.
- `ALLOC(size%)` allocates raw memory. `POKEB addr&, byte%` writes single byte. Memory freed on program exit.

### OPX build notes

- PETRAN requires 3 UIDs: `-uid1 0x10000079 -uid2 0x1000005d -uid3 0x101F9B01`
- Missing uid1 (KDynamicLibraryUid) causes "Nicht unterstützt" on Psion.
- DLLTOOL needs `--as "C:\\path\\AS.EXE"` flag — cannot find assembler automatically.
- OPX compile: use `/Epoc32/Include` (Cygwin path) for GCC -I flag.

## Wine / OPLTRAN quirks

- Running OPLTRAN rapidly in a loop causes sporadic false errors (Wine initialization race). Add `sleep 0.3` between compilations or compile individually.
- Resource file warning (`Unable to open resource file ...oplr.r01`) is harmless — compilation still succeeds.
- Non-ASCII characters (em dash, etc.) in REM comments can confuse OPLTRAN line counting and cause errors on wrong lines.
