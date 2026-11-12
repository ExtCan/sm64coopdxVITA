#ifndef _ULTRA64_LIBAUDIO_H_
#define _ULTRA64_LIBAUDIO_H_

#include "abi.h"

typedef struct
{
#ifdef TARGET_VITA
    // The prebuilt sound data (sound/*_compressed.*) is laid out for 64-bit pointers.
    // Keep that layout on the 32-bit Vita: the pointer lives in the low half of an 8 byte slot.
    union { u8 *offset; u64 offset64; };
#else
    u8 *offset;
#endif
    s32 len;
#ifdef VERSION_SH
    s8 magic[2]; // tbl: 0x0204, otherwise: 0x0203

    // for ctl (else zeros):
    union {
        // unused, just for clarification (big endian)
        struct {
            u8 bank;
            u8 ff;
            u8 numInstruments;
            u8 numDrums;
        } as_u8;

        // used
        struct {
            s16 bankAndFf;
            s16 numInstrumentsAndDrums;
        } as_s16;
    } ctl;
#endif
} ALSeqData;

typedef struct
{
#ifndef VERSION_SH
    s16 revision;
#endif
    s16 seqCount;
#ifdef VERSION_SH
    s16 unk2;
    u8 *data;
    s32 pad[2];
#endif
    ALSeqData seqArray[1];
} ALSeqFile;

void alSeqFileNew(ALSeqFile *f, u8 *base);

#endif
