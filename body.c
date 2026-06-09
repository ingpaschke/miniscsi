/* body.c — C body of the miniscsi .NHsd SCSI disk driver (Retro68 port).
 *
 * Entry points, called from the head.s shim (which marshals A0=param block /
 * A1=DCE into C stack args and routes the return via jIODone or RTS):
 *     cInstall  — once at boot: install the DRVR, patch the DCE
 *     cPrime    — read/write blocks
 *     cControl  — accRun self-mount, drive icon, verify/format/eject
 *     cStatus   — DriveStatus
 *
 * Standard Inside Macintosh types + trap glue (SCSI Manager, PBMountVol,
 * PostEvent, DriverInstall) place registers by construction.  Only AddDrive is
 * hand-rolled: it's InterfaceLib glue a freestanding boot driver can't call.
 *
 * Position-independent: built -mpcrel + -fno-zero-initialized-in-bss, so the
 * image has 0 absolute relocations and runs at any load address as-is.
 */
#include <stddef.h>
#include <MacTypes.h>
#include <SCSI.h>           /* SCSI Manager + AppleDiskPartitions (Partition, pMapSIG) */
#include <Devices.h>        /* DCtlEntry, DriverInstall, accRun */
#include <Disks.h>          /* DrvSts, DrvQEl, GetDrvQHdr, kDriveStatus/kVerify/...    */
#include <Events.h>         /* PostEvent, diskEvt */

/* Fail the build if a struct field lands at the wrong offset. */
#define ASSERT_OFFSET(type, field, off) \
    typedef char assert_##type##_##field[offsetof(type, field) == (off) ? 1 : -1]

enum {                          /* result codes (Inside Macintosh) */
    errNone        =   0,       /* noErr      */
    errIO          = -36,       /* ioErr      */
    errParam       = -50,       /* paramErr   */
    errNoSuchDrive = -56,       /* nsDrvErr   */
    errControl     = -17,       /* controlErr */
    errStatus      = -18        /* statusErr  */
};

enum { kBytesPerBlock = 512, kBlockShift = 9 };   /* bytes >> 9 == blocks */

enum {                          /* SCSI transaction */
    kCmdRead10         = 0x28,   /* READ(10) opcode  */
    kCmdWrite10        = 0x2A,   /* WRITE(10) opcode */
    kCDBLength         = 10,
    kBusBusyBit        = 0x40,   /* SCSIStat: bus busy */
    kArbitrateTries    = 15,     /* SCSIGet retries */
    kBusFreeWaitMax    = 100,    /* SCSIStat BSY-poll bound per retry */
    kCompleteWaitTicks = 300,    /* SCSIComplete timeout */
    kWriteChunkBlocks  = 128     /* polled WRITE ceiling on the q800 (~256 blocks) */
};

enum { fOpened = 0x0020, fRAMBased = 0x0040 };    /* dCtlFlags bits patched at install */
enum { loUnitTableBase = 0x011C };                /* UTableBase: array of DCE handles */
enum { kMaxPartitions = 8, kFirstDriveNum = 20 }; /* resident table size; drive-num base */

#pragma pack(push, 2)

/* SCSI READ(10)/WRITE(10) CDB.  Byte-packed: the 16-bit length sits at an odd
 * offset, which the 68000 can't reach as an aligned word. */
typedef struct {
    unsigned char opcode;        /* +0  kCmdRead10 / kCmdWrite10 */
    unsigned char lunFlags;      /* +1 */
    unsigned char lba[4];        /* +2  logical block address, MSB first */
    unsigned char reserved;      /* +6 */
    unsigned char length[2];     /* +7  transfer length in blocks, MSB first */
    unsigned char control;       /* +9 */
} CDB10;
ASSERT_OFFSET(CDB10, lba, 2);
ASSERT_OFFSET(CDB10, length, 7);
typedef char assert_CDB10_size[sizeof(CDB10) == kCDBLength ? 1 : -1];

/* One resident slot per registered Apple_HFS partition.  The standard DrvQEl is
 * preceded by 4 OS flag bytes (AddDrive is handed &dqEl). */
typedef struct {
    long          osFlags;       /* +0  drive-queue flag bytes (the 4 before the element) */
    DrvQEl        dqEl;          /* +4  element linked into the system drive queue */
    UInt32        physStart;     /*     partition physical start block */
    UInt32        blockCount;    /*     partition size in blocks */
    short         inUse;
} DriveSlot;
ASSERT_OFFSET(DriveSlot, dqEl, 4);

#pragma pack(pop)

/* Zero-init globals live in the resident image's .data (-fno-zero-initialized-in-bss),
 * not a heap .bss: there is no relocation/startup pass, and PC-relative access must
 * reach in-image storage. */
static DriveSlot gDriveTable[kMaxPartitions] = {{0}};
static Boolean   gDidSelfMount = 0;  /* one-shot accRun guard */
static short     gTargetID = 0;      /* our disk's SCSI id (the ROM's D5 at install) */
static short     gRefNum = 0;        /* our driver refNum */

extern char drvr;                   /* DRVR header at image+0x1E (head.s) */

/* Desktop icon returned for Control csCode kDriveIcon: a 32x32 ICN# (128-byte
 * bitmap + 128-byte mask) followed by a Pascal "where" string ("SCSI").  A
 * generic 3D disk-drive box; without it the Finder draws a blank document icon.
 * (Format per Apple TN1189; a disk driver must supply its own icon bits.) */
static unsigned char gDriveIcon[] = {
    /* --- 32x32 icon bitmap (128 bytes): a generic 3D hard-disk box --- */
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x7f,0xff,0xfe,
    0x00,0x80,0x00,0x06, 0x01,0x00,0x00,0x0a,
    0x02,0x00,0x00,0x12, 0x07,0xff,0xff,0xe2,
    0x04,0x00,0x00,0x22, 0x04,0x00,0x00,0x22,
    0x04,0x00,0x00,0x22, 0x04,0x00,0x00,0x22,
    0x04,0x00,0x00,0x22, 0x04,0x00,0x00,0x22,
    0x04,0x00,0x00,0x22, 0x05,0xc0,0x00,0x22,
    0x05,0x40,0x00,0x24, 0x05,0xc0,0x00,0x28,
    0x04,0x00,0x00,0x30, 0x07,0xff,0xff,0xe0,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    /* --- mask (128 bytes): the box silhouette --- */
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x7f,0xff,0xfe,
    0x00,0xff,0xff,0xfe, 0x01,0xff,0xff,0xfe,
    0x03,0xff,0xff,0xfe, 0x07,0xff,0xff,0xfe,
    0x07,0xff,0xff,0xfe, 0x07,0xff,0xff,0xfe,
    0x07,0xff,0xff,0xfe, 0x07,0xff,0xff,0xfe,
    0x07,0xff,0xff,0xfe, 0x07,0xff,0xff,0xfe,
    0x07,0xff,0xff,0xfe, 0x07,0xff,0xff,0xfe,
    0x07,0xff,0xff,0xfe, 0x07,0xff,0xff,0xfc,
    0x07,0xff,0xff,0xf8, 0x07,0xff,0xff,0xf0,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    /* --- "where" Pascal string --- */
    4, 'S','C','S','I'
};

/* ============================================================================= */

/* Store MSB-first: SCSI CDB fields are big-endian and the length is byte-aligned. */
static void StoreBE32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >>  8); p[3] = (unsigned char)v;
}
static void StoreBE16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v;
}

static Boolean StringsEqual(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static DriveSlot *FindSlotForDrive(short driveNum)
{
    short i;
    for (i = 0; i < kMaxPartitions; i++)
        if (gDriveTable[i].inUse && gDriveTable[i].dqEl.dQDrive == driveNum)
            return &gDriveTable[i];
    return NULL;
}

/* Lowest drive number >= kFirstDriveNum not already in the system drive queue. */
static short GetFreeDriveNumber(void)
{
    short candidate = kFirstDriveNum;
    for (;;) {
        DrvQElPtr e = (DrvQElPtr)GetDrvQHdr()->qHead;
        Boolean   taken = false;
        while (e) {
            if (e->dQDrive == candidate) { taken = true; break; }
            e = (DrvQElPtr)e->qLink;
        }
        if (!taken) return candidate;
        candidate++;
    }
}

/* _AddDrive (InterfaceLib glue, so hand-rolled): D0 = (driveNum<<16)|refNum, A0 = &DrvQEl. */
static void AddDriveToQueue(short driveNum, short refNum, DrvQElPtr el)
{
    register long  d0 asm("d0") =
        (((long)(unsigned short)driveNum) << 16) | (unsigned short)refNum;
    register void *a0 asm("a0") = el;
    asm volatile(".short 0xA04E" : "+d"(d0) : "a"(a0) : "d1", "d2", "a1", "memory", "cc");
}

/* One polled READ(10)/WRITE(10) of `blockCount` blocks at `lba` to/from `buffer`:
 * arbitrate (retry + BSY-wait) -> select -> command -> read/write -> complete.
 * Success = SCSIComplete ok AND target status GOOD AND transfer ok. */
static OSErr ScsiTransfer(unsigned long lba, unsigned short blockCount,
                          Ptr buffer, Boolean isWrite)
{
    CDB10     cdb;
    SCSIInstr tib[2];
    short     targetStatus = 0, message = 0;
    OSErr     cmdResult, xferResult, completeResult;
    short     tries;

    cdb.opcode   = isWrite ? kCmdWrite10 : kCmdRead10;
    cdb.lunFlags = 0;
    StoreBE32(cdb.lba, lba);            /* logical block address */
    cdb.reserved = 0;
    StoreBE16(cdb.length, blockCount);  /* transfer length in blocks */
    cdb.control  = 0;

    tib[0].scOpcode = scInc;            /* one auto-incrementing transfer of the whole buffer */
    tib[0].scParam1 = (long)buffer;
    tib[0].scParam2 = (long)blockCount * kBytesPerBlock;
    tib[1].scOpcode = scStop;
    tib[1].scParam1 = 0;
    tib[1].scParam2 = 0;

    for (tries = kArbitrateTries; tries > 0; tries--) {        /* arbitrate for the bus */
        if (SCSIGet() == errNone) break;
        { short wait = kBusFreeWaitMax;
          while (wait-- > 0) if (!(SCSIStat() & kBusBusyBit)) break; }
    }
    if (tries == 0) return errIO;                              /* never won the bus */

    if (SCSISelect(gTargetID) != errNone) return errIO;        /* select fail: bus free */

    cmdResult = SCSICmd((Ptr)&cdb, sizeof cdb);
    if (cmdResult == errNone)
        xferResult = isWrite ? SCSIWrite((Ptr)tib) : SCSIRead((Ptr)tib);
    else
        xferResult = -1;                                       /* skip transfer, still Complete */

    completeResult = SCSIComplete(&targetStatus, &message, kCompleteWaitTicks);

    if (completeResult != errNone) return errIO;
    if ((targetStatus & 0xFF) != 0) return errIO;              /* device status not GOOD */
    if (cmdResult != errNone || xferResult != errNone) return errIO;
    return errNone;
}

/* ============================================================================= */
/* partition discovery + mount.  ScanPartitionMap (scan + AddDrive) runs at INSTALL so the ROM
 * can read boot blocks through the drive queue and our disk can be a startup disk; the actual
 * PBMountVol (MountAllPartitions) waits for the first accRun, once the File Manager is up. */

static void RegisterPartition(unsigned long physStart, unsigned long blockCount)
{
    DriveSlot *slot = NULL;
    short i, driveNum;

    for (i = 0; i < kMaxPartitions; i++)
        if (!gDriveTable[i].inUse) { slot = &gDriveTable[i]; break; }
    if (!slot) return;                                         /* table full */

    slot->physStart     = physStart;
    slot->blockCount    = blockCount;
    slot->dqEl.dQDrvSz  = (unsigned short)blockCount;          /* low word */
    slot->inUse         = true;

    driveNum            = GetFreeDriveNumber();
    slot->dqEl.dQDrive  = driveNum;
    AddDriveToQueue(driveNum, gRefNum, &slot->dqEl);           /* sets dQDrive/dQRefNum from D0 */
}

static void MountAllPartitions(void)
{
    ParamBlockRec pb;
    short i, k, driveNum;
    for (i = 0; i < kMaxPartitions; i++) {
        if (!gDriveTable[i].inUse) continue;
        driveNum = gDriveTable[i].dqEl.dQDrive;
        for (k = 0; k < (short)sizeof pb; k++) ((char *)&pb)[k] = 0;
        pb.ioParam.ioVRefNum = driveNum;                       /* MountVol takes the drive num here */
        if (PBMountVol(&pb) == errNone)
            PostEvent(diskEvt, (unsigned long)(unsigned short)driveNum);  /* show it on the desktop */
    }
}

static void ScanPartitionMap(void)
{
    char             buffer[kBytesPerBlock];
    const Partition *entry = (const Partition *)buffer;
    unsigned long    mapBlocks, blk;

    if (ScsiTransfer(1, 1, (Ptr)buffer, false) != errNone) return;
    if (entry->pmSig != pMapSIG) return;                       /* no Apple partition map */
    mapBlocks = entry->pmMapBlkCnt;

    for (blk = 1; blk <= mapBlocks; blk++) {
        if (ScsiTransfer(blk, 1, (Ptr)buffer, false) != errNone) continue;
        if (StringsEqual((const char *)entry->pmParType, "Apple_HFS"))
            RegisterPartition(entry->pmPyPartStart, entry->pmPartBlkCnt);
    }
}

/* ============================================================================= */
/* driver entry points */

short cInstall(short scsiID)
{
    short        unit, refNum;
    DCtlPtr      dce;
    DCtlHandle  *unitTable;

    gTargetID = scsiID & 7;
    unit      = 32 + gTargetID;
    refNum    = ~unit;
    gRefNum   = refNum;
    DriverInstall((DRVRHeaderPtr)&drvr, refNum);   /* _DrvrInstall (leaves dCtlDriver null) */

    unitTable = *(DCtlHandle **)loUnitTableBase;   /* UTableBase[unit] -> DCE handle -> DCE */
    dce       = *unitTable[unit];
    dce->dCtlDriver = &drvr;
    dce->dCtlFlags  = *(short *)&drvr;  /* = the DRVR header's drvrFlags (incl dNeedTime) */
    dce->dCtlFlags |=  fOpened;
    dce->dCtlFlags &= ~fRAMBased;       /* our DRVR is a fixed ptr, not a relocatable handle */

    /* Register our drives now — read the APM and AddDrive each Apple_HFS partition — rather than
     * at accRun.  The ROM reads boot blocks via Prime through the drive queue right after install,
     * so a drive deferred to accRun is invisible at boot and the disk can't be a startup disk
     * (TN1189: the driver creates the boot drive's queue element at install). */
    ScanPartitionMap();
    return errNone;
}

long cPrime(void *pbArg, void *dceArg)
{
    ParmBlkPtr     pb  = (ParmBlkPtr)pbArg;
    DCtlPtr        dce = (DCtlPtr)dceArg;
    DriveSlot     *slot = FindSlotForDrive(pb->ioParam.ioVRefNum);
    unsigned long  relBlock, blocksLeft, lba;
    long           requested;
    unsigned char *buffer;
    Boolean        isWrite;
    unsigned short chunk;

    if (!slot) return errNoSuchDrive;

    relBlock   = (unsigned long)dce->dCtlPosition >> kBlockShift;
    requested  = pb->ioParam.ioReqCount;
    blocksLeft = (unsigned long)requested >> kBlockShift;
    if (relBlock + blocksLeft > slot->blockCount) return errParam;   /* off the partition */

    lba     = slot->physStart + relBlock;
    buffer  = (unsigned char *)pb->ioParam.ioBuffer;
    isWrite = (pb->ioParam.ioTrap & 1) != 0;

    /* Writes are chunked (the q800 polled SCSIWrite hangs past ~256 blocks); reads
     * go in one shot (16-bit count) and loop only for very large transfers. */
    while (blocksLeft > 0) {
        chunk = isWrite ? kWriteChunkBlocks : 0xFFFF;
        if ((unsigned long)chunk > blocksLeft) chunk = (unsigned short)blocksLeft;
        if (ScsiTransfer(lba, chunk, (Ptr)buffer, isWrite) != errNone) return errIO;
        lba        += chunk;
        buffer     += (unsigned long)chunk << kBlockShift;
        blocksLeft -= chunk;
    }
    pb->ioParam.ioActCount = requested;
    return errNone;
}

long cControl(void *pbArg, void *dceArg)
{
    CntrlParamPtr pb = (CntrlParamPtr)pbArg;
    (void)dceArg;
    switch (pb->csCode) {
    case accRun:                         /* first tick once the File Manager is up: mount once */
        if (!gDidSelfMount) { gDidSelfMount = true; MountAllPartitions(); }
        return errNone;
    case kDriveIcon:                     /* return our desktop icon (ICN# + mask + "where") */
        *(Ptr *)pb->csParam = (Ptr)gDriveIcon;
        return errNone;
    case kVerify:
    case kFormat:
    case kEject:
        return errNone;                  /* no-ops */
    default:
        return errControl;
    }
}

long cStatus(void *pbArg, void *dceArg)
{
    CntrlParamPtr pb = (CntrlParamPtr)pbArg;
    DriveSlot    *slot;
    DrvSts       *status;
    short         i;
    (void)dceArg;

    if (pb->csCode != kDriveStatus) return errStatus;

    slot = FindSlotForDrive(pb->ioVRefNum);
    if (!slot) return errNoSuchDrive;

    status = (DrvSts *)pb->csParam;
    for (i = 0; i < (short)sizeof(DrvSts); i++) ((char *)status)[i] = 0;
    status->diskInPlace = 8;             /* nonejectable disk in place */
    status->installed   = 1;
    status->dQDrive     = slot->dqEl.dQDrive;
    status->dQRefNum    = slot->dqEl.dQRefNum;
    return errNone;
}
