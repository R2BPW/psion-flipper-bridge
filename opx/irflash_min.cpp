// IRFLASH_MIN.CPP - Minimal OPX test (no sockets, no IrDA)
// Just returns 42 to verify OPX loading works

#include <e32base.h>
#include <oplapi.h>
#include <opx.h>
#include <oplerr.h>

const TInt KUidOpxIrFlashValue = 0x101F9B01;
const TInt KOpxIrFlashVersion = 0x100;

class CIrFlash : public COpxBase
    {
public:
    static CIrFlash* NewL(OplAPI& aOplAPI);
    virtual void RunL(TInt aProcNum);
    virtual TBool CheckVersion(TInt aVersion);
private:
    CIrFlash(OplAPI& aOplAPI);
    ~CIrFlash();
    };

CIrFlash::CIrFlash(OplAPI& aOplAPI) : COpxBase(aOplAPI)
    {
    }

CIrFlash* CIrFlash::NewL(OplAPI& aOplAPI)
    {
    CIrFlash* self = new(ELeave) CIrFlash(aOplAPI);
    return self;
    }

CIrFlash::~CIrFlash()
    {
    Dll::FreeTls();
    }

void CIrFlash::RunL(TInt aProcNum)
    {
    switch (aProcNum)
        {
    case 1: // IrInit - just return 42
        iOplAPI.Push((TInt16)42);
        break;
    case 2: // IrFlash - return 43
        iOplAPI.Push((TInt16)43);
        break;
    case 3: // IrClose - return 0
        iOplAPI.Push((TInt16)0);
        break;
    case 4: // IrSetSlots - pop arg, return 0
        iOplAPI.PopInt16();
        iOplAPI.Push((TInt16)0);
        break;
    default:
        User::Leave(KOplErrOpxProcNotFound);
        }
    }

TBool CIrFlash::CheckVersion(TInt aVersion)
    {
    if ((aVersion & 0x0f00) > (KOpxIrFlashVersion & 0x0f00))
        return EFalse;
    return ETrue;
    }

EXPORT_C COpxBase* NewOpxL(OplAPI& aOplAPI)
    {
    CIrFlash* tls = (CIrFlash*)Dll::Tls();
    if (tls == NULL)
        {
        tls = CIrFlash::NewL(aOplAPI);
        TInt ret = Dll::SetTls(tls);
        User::LeaveIfError(ret);
        }
    return (COpxBase*)tls;
    }

EXPORT_C TInt Version()
    {
    return KOpxIrFlashVersion;
    }

TInt E32Dll(TDllReason)
    {
    return KErrNone;
    }
