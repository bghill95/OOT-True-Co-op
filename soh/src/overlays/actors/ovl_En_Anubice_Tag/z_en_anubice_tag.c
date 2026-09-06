/*
 * File: z_en_anubice_tag.c
 * Overlay: ovl_En_Anubice_Tag
 * Description: Spawns and manages the Anubis enemy
 */

#include "z_en_anubice_tag.h"
#include "vt.h"

#define FLAGS ACTOR_FLAG_UPDATE_CULLING_DISABLED

void EnAnubiceTag_Init(Actor* thisx, PlayState* play);
void EnAnubiceTag_Destroy(Actor* thisx, PlayState* play);
void EnAnubiceTag_Update(Actor* thisx, PlayState* play);
void EnAnubiceTag_Draw(Actor* thisx, PlayState* play);

void EnAnubiceTag_SpawnAnubis(EnAnubiceTag* this, PlayState* play);
void EnAnubiceTag_ManageAnubis(EnAnubiceTag* this, PlayState* play);

const ActorInit En_Anubice_Tag_InitVars = {
    ACTOR_EN_ANUBICE_TAG,
    ACTORCAT_SWITCH,
    FLAGS,
    OBJECT_GAMEPLAY_KEEP,
    sizeof(EnAnubiceTag),
    (ActorFunc)EnAnubiceTag_Init,
    (ActorFunc)EnAnubiceTag_Destroy,
    (ActorFunc)EnAnubiceTag_Update,
    (ActorFunc)EnAnubiceTag_Draw,
    NULL,
};

void EnAnubiceTag_Init(Actor* thisx, PlayState* play) {
    EnAnubiceTag* this = (EnAnubiceTag*)thisx;

    osSyncPrintf("\n\n");
    // "Anubis control tag generated"
    osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ アヌビス制御タグ発生 ☆☆☆☆☆ %d\n" VT_RST, this->actor.params);

    if (this->actor.params < -1) {
        this->actor.params = 0;
    }
    if (this->actor.params != 0) {
        this->triggerRange = this->actor.params * 40.0f;
    }
    this->actionFunc = EnAnubiceTag_SpawnAnubis;
}

void EnAnubiceTag_Destroy(Actor* thisx, PlayState* play) {
}

void EnAnubiceTag_SpawnAnubis(EnAnubiceTag* this, PlayState* play) {
    this->anubis = (EnAnubice*)Actor_SpawnAsChild(&play->actorCtx, &this->actor, play, ACTOR_EN_ANUBICE,
                                                  this->actor.world.pos.x, this->actor.world.pos.y,
                                                  this->actor.world.pos.z, 0, this->actor.yawTowardsPlayer, 0, 0);

    if (this->anubis != NULL) {
        this->actionFunc = EnAnubiceTag_ManageAnubis;
    }
}

s32 Anchor_IsMirroredActor(Actor* actor);
Player* Anchor_NearestPlayer(Actor* from);

void EnAnubiceTag_ManageAnubis(EnAnubiceTag* this, PlayState* play) {
    EnAnubice* anubis;
    Vec3f offset;

    if (this->anubis != NULL) {
        anubis = this->anubis;
        if (anubis->actor.update == NULL) {
            return;
        }
        // This tag is ACTORCAT_SWITCH, so it runs on BOTH clients — but the
        // Anubis it steers is a tracked enemy that only the authority simulates.
        // On a mirror, everything below writes world.pos and the Anubis's own
        // isMirroringLink / isLinkOutOfRange straight over the streamed state,
        // once per frame, using the LOCAL player's distance and yaw. Let the
        // authority's tag do the steering and just watch here.
        if (Anchor_IsMirroredActor(&anubis->actor)) {
            return;
        }
    } else {
        Actor_Kill(&this->actor);
        return;
    }

    if (anubis->deathTimer != 0) {
        Actor_Kill(&this->actor);
        return;
    }

    // Co-op: this tag is what wakes the Anubis and makes it shadow a player, and
    // it decides on its OWN xzDistToPlayer / yawTowardsPlayer — the distance to
    // the LOCAL Link. Being ACTORCAT_SWITCH it is not a tracked actor, so
    // EnemySync's perception override never rewrites those fields for it, and
    // the guard above leaves only the authority's tag in charge. The result was
    // that the other player could stand in front of an Anubis and it would never
    // emerge, whatever they did — aiming the fireball at the nearest player (see
    // En_Anubice) fixed where it shoots, not whether it wakes at all.
    {
        Player* nearest = Anchor_NearestPlayer(&this->actor);
        f32 dx = nearest->actor.world.pos.x - this->actor.world.pos.x;
        f32 dz = nearest->actor.world.pos.z - this->actor.world.pos.z;

        this->actor.xzDistToPlayer = sqrtf(SQ(dx) + SQ(dz));
        this->actor.yawTowardsPlayer = Math_FAtan2F(dx, dz) * (0x8000 / M_PI);
    }

    if (this->actor.xzDistToPlayer < (200.0f + this->triggerRange)) {
        if (!anubis->isLinkOutOfRange) {
            if (!anubis->isKnockedback) {
                anubis->isMirroringLink = true;
                offset.x = -Math_SinS(this->actor.yawTowardsPlayer) * this->actor.xzDistToPlayer;
                offset.z = -Math_CosS(this->actor.yawTowardsPlayer) * this->actor.xzDistToPlayer;
                Math_ApproachF(&anubis->actor.world.pos.x, (this->actor.world.pos.x + offset.x), 0.3f, 10.0f);
                Math_ApproachF(&anubis->actor.world.pos.z, (this->actor.world.pos.z + offset.z), 0.3f, 10.0f);
                return;
            }
        }
    } else {
        if (anubis->isMirroringLink) {
            anubis->isLinkOutOfRange = true;
        }
    }
}

void EnAnubiceTag_Update(Actor* thisx, PlayState* play) {
    EnAnubiceTag* this = (EnAnubiceTag*)thisx;

    this->actionFunc(this, play);
}

void EnAnubiceTag_Draw(Actor* thisx, PlayState* play) {
    EnAnubiceTag* this = (EnAnubiceTag*)thisx;

    if (BREG(0) != 0) {
        DebugDisplay_AddObject(this->actor.world.pos.x, this->actor.world.pos.y, this->actor.world.pos.z,
                               this->actor.world.rot.x, this->actor.world.rot.y, this->actor.world.rot.z, 1.0f, 1.0f,
                               1.0f, 0xFF, 0, 0, 0xFF, 4, play->state.gfxCtx);
    }
}
