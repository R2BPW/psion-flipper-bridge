// IRFLASH OPX - SIR burst via device driver
// Minimal version: Init, Flash, Close only

#include <e32base.h>
#include <d32comm.h>
#include <oplapi.h>
#include <opx.h>
#include <oplerr.h>

const TInt KUidOpxIrFlashValue = 0x101F9B01;
const TInt KOpxIrFlashVersion = 0x100;

static const TUint8 KBurstBytes[] = {
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55
    };

class CIrFlash : public COpxBase
    {
public:
    static CIrFlash* NewL(OplAPI& aOplAPI);
    virtual void RunL(TInt aProcNum);
    virtual TBool CheckVersion(TInt aVersion);
private:
    void DoInit();
    void DoFlash();
    void DoClose();
    CIrFlash(OplAPI& aOplAPI);
    ~CIrFlash();
private:
    RBusDevComm iComm;
    TBool iInitialized;
    TInt iUnit;
    };

void CIrFlash::DoInit()
    {
    if (iInitialized)
        {
        iComm.Close();
        iInitialized = EFalse;
        }

    TInt err = iComm.Open(1);
    if (err == KErrNone)
        iUnit = 1;
    else
        {
        err = iComm.Open(0);
        if (err == KErrNone)
            iUnit = 0;
        }

    if (err != KErrNone)
        {
        iOplAPI.Push((TInt16)err);
        return;
        }

    TCommConfig configBuf;
    iComm.Config(configBuf);
    TCommConfigV01& config = configBuf();
    config.iRate = EBps4800;
    config.iDataBits = EData8;
    config.iStopBits = EStop1;
    config.iParity = EParityNone;
    config.iHandshake = 0;
    config.iSIREnable = ESIREnable;
    config.iSIRSettings = KConfigSIRPulseWidthMinimum;
    iComm.SetConfig(configBuf);

    iInitialized = ETrue;
    iOplAPI.Push((TInt16)iUnit);
    }

void CIrFlash::DoFlash()
    {
    if (!iInitialized)
        {
        iOplAPI.Push((TInt16)KErrNotReady);
        return;
        }
    TPtrC8 burstData(KBurstBytes, 20);
    TRequestStatus status;
    iComm.Write(status, burstData);
    User::WaitForRequest(status);
    iOplAPI.Push((TInt16)status.Int());
    }

void CIrFlash::DoClose()
    {
    if (iInitialized)
        {
        iComm.Close();
        iInitialized = EFalse;
        }
    iOplAPI.Push((TInt16)0);
    }

CIrFlash::CIrFlash(OplAPI& aOplAPI)
    : COpxBase(aOplAPI), iInitialized(EFalse), iUnit(0)
    {
    }

CIrFlash* CIrFlash::NewL(OplAPI& aOplAPI)
    {
    CIrFlash* self = new(ELeave) CIrFlash(aOplAPI);
    return self;
    }

CIrFlash::~CIrFlash()
    {
    if (iInitialized)
        iComm.Close();
    Dll::FreeTls();
    }

void CIrFlash::RunL(TInt aProcNum)
    {
    switch (aProcNum)
        {
    case 1: DoInit(); break;
    case 2: DoFlash(); break;
    case 3: DoClose(); break;
    case 4:
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
