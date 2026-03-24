// IRFLASH OPX v2 — 38kHz carrier via 38400 baud SIR
//
// 0x00 at 38400 baud = 9 SIR pulses at ~38kHz = TSOP carrier (mark)
// 0xFF at 38400 baud = no pulses = silence (space)
// Entire message encoded as mark/space durations in one Write().

#include <e32base.h>
#include <d32comm.h>
#include <oplapi.h>
#include <opx.h>
#include <oplerr.h>

const TInt KUidOpxIrFlashValue = 0x101F9B01;
const TInt KOpxIrFlashVersion = 0x100;

// Timing in bytes at 38400 baud (~260us per byte):
// Mark: 0x00 bytes, Space: 0xFF bytes
const TInt KMarkHeader  = 32;   // ~8.3ms header mark
const TInt KSpaceHeader = 16;   // ~4.2ms header space
const TInt KMarkBit     = 4;    // ~1.0ms bit mark (constant)
const TInt KSpaceBit0   = 4;    // ~1.0ms space = digit 0
const TInt KSpaceBit1   = 8;    // ~2.1ms space = digit 1
const TInt KSpaceBit2   = 12;   // ~3.1ms space = digit 2
const TInt KMarkTrail   = 4;    // ~1.0ms trailing mark (ends signal)

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
    void DoSendMsg();
    void AppendMark(TDes8& aBuf, TInt aCount);
    void AppendSpace(TDes8& aBuf, TInt aCount);
    CIrFlash(OplAPI& aOplAPI);
    ~CIrFlash();
private:
    RBusDevComm iComm;
    TBool iInitialized;
    TInt iUnit;
    };

void CIrFlash::AppendMark(TDes8& aBuf, TInt aCount)
    {
    for (TInt i = 0; i < aCount && aBuf.Length() < aBuf.MaxLength(); i++)
        aBuf.Append(0x00);
    }

void CIrFlash::AppendSpace(TDes8& aBuf, TInt aCount)
    {
    for (TInt i = 0; i < aCount && aBuf.Length() < aBuf.MaxLength(); i++)
        aBuf.Append(0xFF);
    }

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
    config.iRate = EBps38400;
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
    // Legacy single burst (20 bytes of 0x00 at 38kHz)
    if (!iInitialized)
        {
        iOplAPI.Push((TInt16)KErrNotReady);
        return;
        }
    TBuf8<20> burst;
    for (TInt i = 0; i < 20; i++) burst.Append(0x00);
    TRequestStatus status;
    iComm.Write(status, burst);
    User::WaitForRequest(status);
    iOplAPI.Push((TInt16)status.Int());
    }

void CIrFlash::DoSendMsg()
    {
    TPtrC8 msg = iOplAPI.PopString8();

    if (!iInitialized)
        {
        iOplAPI.Push((TInt16)KErrNotReady);
        return;
        }

    // Max buffer: 30 chars * ~60 bytes/char + header/trailer = ~2000
    HBufC8* buf = HBufC8::New(3000);
    if (!buf)
        {
        iOplAPI.Push((TInt16)KErrNoMemory);
        return;
        }
    TPtr8 ptr = buf->Des();

    // Header: long mark + space (signals start of message)
    AppendMark(ptr, KMarkHeader);
    AppendSpace(ptr, KSpaceHeader);

    for (TInt i = 0; i < msg.Length(); i++)
        {
        TUint8 ch = msg[i];
        TInt code = -1;
        if (ch >= 'A' && ch <= 'Z')      code = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') code = ch - 'a';
        else if (ch == ' ')              code = 26;
        if (code < 0) continue;

        // Ternary decomposition
        TInt d2 = code / 9;
        TInt rem = code - d2 * 9;
        TInt d1 = rem / 3;
        TInt d0 = rem - d1 * 3;

        // Encode 3 ternary digits as mark+space pairs
        TInt digits[3];
        digits[0] = d2;
        digits[1] = d1;
        digits[2] = d0;

        for (TInt d = 0; d < 3; d++)
            {
            AppendMark(ptr, KMarkBit);
            TInt spaceLen = (digits[d] == 0) ? KSpaceBit0 :
                            (digits[d] == 1) ? KSpaceBit1 : KSpaceBit2;
            AppendSpace(ptr, spaceLen);
            }
        }

    // Trailing mark (terminates last space for TSOP)
    AppendMark(ptr, KMarkTrail);

    // Send everything in one shot
    TRequestStatus status;
    iComm.Write(status, ptr);
    User::WaitForRequest(status);

    delete buf;
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
    case 5: DoSendMsg(); break;
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
