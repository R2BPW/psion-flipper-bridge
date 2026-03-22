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

## Wine / OPLTRAN quirks

- Running OPLTRAN rapidly in a loop causes sporadic false errors (Wine initialization race). Add `sleep 0.3` between compilations or compile individually.
- Resource file warning (`Unable to open resource file ...oplr.r01`) is harmless — compilation still succeeds.
