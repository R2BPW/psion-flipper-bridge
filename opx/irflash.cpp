// IRFLASH.CPP
// OPX for fast IR burst generation
//
// Uses IrDA sockets with KDiscoverySlotsOpt=1 for minimal
// discovery time. Each Connect() attempt generates one IR burst
// (XID frame) and fails fast (~150-200ms vs ~955ms with OPL).

#include "irflash.h"
#include <oplerr.h>

#pragma data_seg(".E32_UID")
__WINS_UID(0, KUidOpxValue, KUidOpxIrFlashValue)
#pragma data_seg()


// ---- OPX procedure implementations ----

void CIrFlash::DoInit()
    {
    if (iInitialized)
        {
        iSockServ.Close();
        }

    TInt err = iSockServ.Connect();
    if (err != KErrNone)
        {
        iOplAPI.Push((TInt16)err);
        return;
        }

    iInitialized = ETrue;
    iSlots = 1;
    iOplAPI.Push((TInt16)0);
    }

void CIrFlash::DoFlash()
    {
    if (!iInitialized)
        {
        iOplAPI.Push((TInt16)KErrNotReady);
        return;
        }

    // Open a fresh socket for each burst
    // (socket must be reopened after failed Connect)
    RSocket sock;
    TInt err = sock.Open(iSockServ, KIrdaAddrFamily, KSockSeqPacket, KIrTinyTP);
    if (err != KErrNone)
        {
        iOplAPI.Push((TInt16)err);
        return;
        }

    // Set minimum discovery slots for fastest burst
    sock.SetOpt(KDiscoverySlotsOpt, KLevelIrlap, iSlots);

    // Set up address (broadcast discovery, no specific target)
    TIrdaSockAddr addr;
    addr.SetRemoteDevAddr(0);
    addr.SetHomePort(1);

    // Connect attempt triggers IrLAP discovery = IR burst
    // Will fail (no peer) but the XID frame is already sent
    TRequestStatus status;
    sock.Connect(addr, status);
    User::WaitForRequest(status);
    // status.Int() will be an error - that's expected

    sock.Close();

    iOplAPI.Push((TInt16)0);
    }

void CIrFlash::DoClose()
    {
    if (iInitialized)
        {
        iSockServ.Close();
        iInitialized = EFalse;
        }
    iOplAPI.Push((TInt16)0);
    }

void CIrFlash::DoSetSlots()
    {
    TInt16 slots = iOplAPI.PopInt16();
    if (slots < 1) slots = 1;
    if (slots > 8) slots = 8;
    iSlots = slots;
    iOplAPI.Push((TInt16)0);
    }


// ---- COpxBase implementation ----

CIrFlash::CIrFlash(OplAPI& aOplAPI)
    : COpxBase(aOplAPI), iInitialized(EFalse), iSlots(1)
    {
    __DECLARE_NAME(_S("CIrFlash"));
    }

CIrFlash* CIrFlash::NewL(OplAPI& aOplAPI)
    {
    CIrFlash* self = new(ELeave) CIrFlash(aOplAPI);
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop();
    return self;
    }

void CIrFlash::ConstructL()
    {
    }

CIrFlash::~CIrFlash()
    {
    if (iInitialized)
        {
        iSockServ.Close();
        }
    Dll::FreeTls();
    }

void CIrFlash::RunL(TInt aProcNum)
    {
    switch (aProcNum)
        {
    case EIrInit:
        DoInit();
        break;
    case EIrFlash:
        DoFlash();
        break;
    case EIrClose:
        DoClose();
        break;
    case EIrSetSlots:
        DoSetSlots();
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


// ---- DLL exports ----

EXPORT_C COpxBase* NewOpxL(OplAPI& aOplAPI)
    {
    CIrFlash* tls = (CIrFlash*)Dll::Tls();
    if (tls == NULL)
        {
        tls = CIrFlash::NewL(aOplAPI);
        CleanupStack::PushL(tls);
        TInt ret = Dll::SetTls(tls);
        User::LeaveIfError(ret);
        CleanupStack::Pop();
        }
    return (COpxBase*)tls;
    }

EXPORT_C TInt Version()
    {
    return KOpxIrFlashVersion;
    }

TInt E32Dll(TDllReason /*aReason*/)
    {
    return KErrNone;
    }
