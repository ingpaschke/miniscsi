# miniscsi

A small open SCSI disk driver for the classic 68k Macintosh, written in C with a
tiny m68k assembly shim and compiled with [Retro68](https://github.com/autc04/Retro68).

The ROM loads it from an `Apple_Driver43` partition at boot and JSRs offset 0 with
the SCSI target id in `D5`. From there it's an ordinary Device Manager `.NHsd`
disk driver: it reads the Apple Partition Map, registers and mounts every
`Apple_HFS` volume it finds, and services reads and writes over the old polled
SCSI Manager (`READ(10)` / chunked `WRITE(10)`). Real `DriveStatus` and a desktop
drive icon round it out.

The image is **fully position-independent** (`-mpcrel`, zero relocations): the ROM
can load it at any address and it runs as-is — no relocation pass, no A5 world.
The whole driver is **~1.8 KB** and links with no library at all.

## Files

| file       | role |
|------------|------|
| `body.c`   | the driver — install, Prime (read/write), Control, Status, partition scan + self-mount |
| `head.s`   | boot preamble + DRVR header, A0/A1 → C marshaling, IODone routing, `memset` |
| `boot.ld`  | flat link script — header at offset 0, no trampoline, no runtime |
| `Makefile` | assemble + compile + link + flatten → `driver.bin` |

## Build

Needs a Retro68 toolchain (defaults to `/opt/retro68`; override with `RETRO68=`):

```sh
make                 # -> driver.bin
```

## Deploy

`driver.bin` is the raw blob that goes into an `Apple_Driver43` partition. The ROM
boot path reads it according to the partition map entry's `pmBootSize` (blob size
in bytes) and validates `pmBootCksum` (a 16-bit additive checksum over the blob),
then JSRs offset 0 with the SCSI id in `D5`. A prebuilt blob is attached to each
[release](../../releases).

## Status

Verified on an emulated Quadra 800 (QEMU): boots, mounts single- and multi-HFS
volumes, reads and writes (integrity-checked), `DriveStatus`, drive icon. Not yet
tried on real hardware.

## License

MIT — see [LICENSE](LICENSE).
