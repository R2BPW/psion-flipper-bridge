REM IRFLASH.OXH
REM OPL header for IrFlash OPX - fast IR burst generation
REM
REM Usage:
REM   INCLUDE "IRFLASH.OXH"
REM   IrInit:        REM open IrDA socket (call once)
REM   IrFlash:       REM one fast IR burst (~150-200ms vs ~955ms)
REM   IrClose:       REM cleanup (call at end)
REM   IrSetSlots:(n%) REM set discovery slots 1-8 (default 1)

DECLARE OPX IRFLASH,KUidOpxIrFlash&,KOpxIrFlashVersion%
  IrInit%:            : 1
  IrFlash%:           : 2
  IrClose%:           : 3
  IrSetSlots%:(n%)    : 4
END DECLARE

CONST KUidOpxIrFlash& = &101F9B01
CONST KOpxIrFlashVersion% = $0100
