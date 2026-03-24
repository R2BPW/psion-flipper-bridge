// IRFLASH.H
// OPX for fast IR burst generation via IrDA sockets
// Bypasses IrDA discovery overhead by using 1 discovery slot

#if !defined(__IRFLASH_H__)
#define __IRFLASH_H__

#include <e32base.h>
#include <oplapi.h>
#include <opx.h>
#include <es_sock.h>
#include <ir_sock.h>

// OPX UID - using a value in the test range
// For production, request from Symbian
const TInt KUidOpxIrFlashValue = 0x101F9B01;
const TUid KUidOpxIrFlash = {KUidOpxIrFlashValue};

// OPX version
const TInt KOpxIrFlashVersion = 0x100;

class CIrFlash : public COpxBase
    {
public:
    static CIrFlash* NewL(OplAPI& aOplAPI);
    virtual void RunL(TInt aProcNum);
    virtual TBool CheckVersion(TInt aVersion);

private:
    enum TExtensions
        {
        EIrInit = 1,     // IrInit:       - open socket, set 1 slot
        EIrFlash,        // IrFlash:      - one burst (connect attempt)
        EIrClose,        // IrClose:      - cleanup
        EIrSetSlots,     // IrSetSlots:(n%) - set discovery slots (1-8)
        };

    void DoInit();
    void DoFlash();
    void DoClose();
    void DoSetSlots();

    void ConstructL();
    CIrFlash(OplAPI& aOplAPI);
    ~CIrFlash();

private:
    RSocketServ iSockServ;
    TBool iInitialized;
    TInt iSlots;
    };

inline CIrFlash* IrFlash() { return ((CIrFlash*)Dll::Tls()); }

#endif // __IRFLASH_H__
