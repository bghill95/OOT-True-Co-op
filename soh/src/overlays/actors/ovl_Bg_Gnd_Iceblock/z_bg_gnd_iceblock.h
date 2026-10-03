#ifndef Z_BG_GND_ICEBLOCK_H
#define Z_BG_GND_ICEBLOCK_H

#include <libultraship/libultra.h>
#include "global.h"

struct BgGndIceblock;

typedef void (*BgGndIceblockActionFunc)(struct BgGndIceblock*, PlayState*);

typedef struct BgGndIceblock {
    /* 0x0000 */ DynaPolyActor dyna;
    /* 0x0164 */ BgGndIceblockActionFunc actionFunc;
    /* 0x0168 */ Vec3f targetPos;
} BgGndIceblock; // size = 0x0174

#ifdef __cplusplus
extern "C" {
#endif
// Co-op: rebuild the file-static grid index from the streamed position. See the
// definition in z_bg_gnd_iceblock.c; called from DynaSync's mirror step.
void BgGndIceblock_CoopRebuildGridIndex(BgGndIceblock* self);
#ifdef __cplusplus
}
#endif

#endif
