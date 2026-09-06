#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
// Vec3f / Vec3s <-> json. EnZf's "headRot" is a bare s16 and needed none of this;
// EnTest's is a Vec3s and EnDodongo's bodyScale a Vec3f.
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Bb/z_en_bb.h"
#include "src/overlays/actors/ovl_En_Bw/z_en_bw.h"
#include "src/overlays/actors/ovl_En_Zf/z_en_zf.h"
#include "src/overlays/actors/ovl_En_Test/z_en_test.h"
#include "src/overlays/actors/ovl_En_Wf/z_en_wf.h"
#include "src/overlays/actors/ovl_En_Firefly/z_en_firefly.h"
#include "src/overlays/actors/ovl_En_Sb/z_en_sb.h"
#include "src/overlays/actors/ovl_En_Ik/z_en_ik.h"
#include "src/overlays/actors/ovl_En_Wallmas/z_en_wallmas.h"
#include "src/overlays/actors/ovl_En_Dodojr/z_en_dodojr.h"
#include "src/overlays/actors/ovl_En_Fd/z_en_fd.h"
#include "src/overlays/actors/ovl_En_Dodongo/z_en_dodongo.h"
#include "src/overlays/actors/ovl_En_Ny/z_en_ny.h"
#include "src/overlays/actors/ovl_En_Fz/z_en_fz.h"
#include "src/overlays/actors/ovl_En_Okuta/z_en_okuta.h"
// Flare Dancer action functions: non-static in z_en_fd.c. Its draw gate compares
// actionFunc against EnFd_Reappear, so the pointer needs a portable index.
void EnFd_Run(EnFd* self, PlayState* play);
void EnFd_SpinAndSpawnFire(EnFd* self, PlayState* play);
void EnFd_Reappear(EnFd* self, PlayState* play);
void EnFd_SpinAndGrow(EnFd* self, PlayState* play);
void EnFd_JumpToGround(EnFd* self, PlayState* play);
void EnFd_WaitForCore(EnFd* self, PlayState* play);
void EnFd_Land(EnFd* self, PlayState* play);
// Baby Dodongo action functions: non-static in z_en_dodojr.c. Its draw switch
// compares actionFunc against these two, so they need a portable index too.
void func_809F73AC(EnDodojr* self, PlayState* play);
void func_809F7BE4(EnDodojr* self, PlayState* play);
void func_809F74C4(EnDodojr* self, PlayState* play);
void func_809F758C(EnDodojr* self, PlayState* play);
void func_809F768C(EnDodojr* self, PlayState* play);
void func_809F773C(EnDodojr* self, PlayState* play);
void func_809F77AC(EnDodojr* self, PlayState* play);
void func_809F784C(EnDodojr* self, PlayState* play);
void func_809F786C(EnDodojr* self, PlayState* play);
void func_809F78EC(EnDodojr* self, PlayState* play);
void func_809F799C(EnDodojr* self, PlayState* play);
void func_809F7A00(EnDodojr* self, PlayState* play);
void func_809F7AB8(EnDodojr* self, PlayState* play);
void func_809F7B3C(EnDodojr* self, PlayState* play);
void func_809F7C48(EnDodojr* self, PlayState* play);
// Keese death state: its PostLimbDraw stops drawing the aura once this is set.
void EnFirefly_Die(EnFirefly* self, PlayState* play);
// Wallmaster action functions: defined non-static in z_en_wallmas.c, declared
// only there. Needed to map the actionFunc pointer to a portable index.
void EnWallmas_WaitToDrop(EnWallmas* self, PlayState* play);
void EnWallmas_Drop(EnWallmas* self, PlayState* play);
void EnWallmas_Land(EnWallmas* self, PlayState* play);
void EnWallmas_Stand(EnWallmas* self, PlayState* play);
void EnWallmas_JumpToCeiling(EnWallmas* self, PlayState* play);
void EnWallmas_ReturnToCeiling(EnWallmas* self, PlayState* play);
void EnWallmas_TakeDamage(EnWallmas* self, PlayState* play);
void EnWallmas_Cooldown(EnWallmas* self, PlayState* play);
void EnWallmas_Die(EnWallmas* self, PlayState* play);
void EnWallmas_TakePlayer(EnWallmas* self, PlayState* play);
void EnWallmas_WaitForProximity(EnWallmas* self, PlayState* play);
void EnWallmas_WaitForSwitchFlag(EnWallmas* self, PlayState* play);
void EnWallmas_Stun(EnWallmas* self, PlayState* play);
void EnWallmas_Walk(EnWallmas* self, PlayState* play);
}

/**
 * One bug class, ten enemies: "the mirror's Draw reads a frozen private field".
 *
 * A suppressed mirror never runs Update, but it DOES run Draw every frame. Any
 * private field the AI mutates and the draw path reads therefore stays at its
 * Init value on the mirror — while world.pos, joints and colliders keep tracking
 * the authority. The result ranges from a cosmetic mismatch (wrong eye texture)
 * to an enemy drawn wrong, or not at all, while its collider still hurts.
 *
 * None of these fields is part of the generic stream (they are not pos / rot /
 * joints / collider bits), so each actor carries its own as extras.
 *
 * On actionFunc: the pointer value is a local code address and means nothing on
 * the other machine, but these overlays declare their action functions
 * non-static, so a small table converts the pointer to an index and back
 * (sWallmasActions, sDodojrActions, and the one "dying" bit EnFirefly needs).
 * That also gives a clean authority handoff: the copy resumes in the authority's
 * state rather than wherever it was left.
 *
 * Deliberately NOT carried: ice/freeze timers (EnBb::fireIceTimer,
 * EnBw/EnZf/EnTest::iceTimer). The draw path decrements them itself and bumps
 * colorFilterTimer, so replaying them would fight the stream every frame.
 */

template <typename T> static void ReadField(const nlohmann::json& x, const char* key, T& out) {
    if (x.contains(key) && !x[key].is_null()) {
        out = x[key].get<T>();
    }
}

// Bubble / flying skull (EN_BB). flameScaleX/Y are zero-initialised, so an
// unpatched mirror draws the bare skull with NO flame — the clearest tell of
// which Bubble you are facing. moveMode gates the whole draw (BBMOVE_HIDDEN),
// and action + maxSpeed gate the trail it emits.
static void EnBb_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnBb* bb = (EnBb*)actor;
    x["fsx"] = bb->flameScaleX;
    x["fsy"] = bb->flameScaleY;
    x["fpa"] = bb->flamePrimAlpha;
    x["fpb"] = bb->flamePrimBlue;
    x["act"] = bb->action;
    x["mv"] = bb->moveMode;
    x["msp"] = bb->maxSpeed;
}

static void EnBb_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnBb* bb = (EnBb*)actor;
        ReadField(x, "fsx", bb->flameScaleX);
        ReadField(x, "fsy", bb->flameScaleY);
        ReadField(x, "fpa", bb->flamePrimAlpha);
        ReadField(x, "fpb", bb->flamePrimBlue);
        ReadField(x, "act", bb->action);
        ReadField(x, "mv", bb->moveMode);
        ReadField(x, "msp", bb->maxSpeed);
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnBb extras parse error: {}", ex.what());
    }
}

// Torch slug (EN_BW). unk_248 is the whole-body scale applied in Draw: frozen at
// its Init value the slug is drawn at the wrong size. unk_258 / unk_260 are the
// writhing rotation and squash, unk_220 the body state, unk_23A a texture scroll.
//
// color1 came later, from the Fire Temple audit: the draw feeds all four of its
// channels into the prim and env colours, and Update is its only writer -- Init
// leaves it opaque red, then the action funcs recolour it per state and finally
// fade the alpha out on death (`shadowAlpha = color1.a -= 6`). A mirror kept the
// Init red for the slug's whole life and never faded it, so the body vanished in
// one frame instead of dissolving. Packed into one number rather than four keys.
static void EnBw_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnBw* bw = (EnBw*)actor;
    x["u220"] = bw->unk_220;
    x["u23a"] = bw->unk_23A;
    x["u248"] = bw->unk_248;
    x["u258"] = bw->unk_258;
    x["u260"] = bw->unk_260;
    x["c1"] = ((uint32_t)bw->color1.r << 24) | ((uint32_t)bw->color1.g << 16) | ((uint32_t)bw->color1.b << 8) |
              (uint32_t)bw->color1.a;
    // The death fade writes both halves of `shadowAlpha = color1.a -= 6`. Carrying
    // only the left one left the slug's shadow at full strength while its body
    // dissolved.
    x["sa"] = bw->actor.shape.shadowAlpha;
}

static void EnBw_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnBw* bw = (EnBw*)actor;
        ReadField(x, "u220", bw->unk_220);
        ReadField(x, "u23a", bw->unk_23A);
        ReadField(x, "u248", bw->unk_248);
        ReadField(x, "u258", bw->unk_258);
        ReadField(x, "u260", bw->unk_260);
        ReadField(x, "sa", bw->actor.shape.shadowAlpha);
        if (x.contains("c1")) {
            uint32_t packed = x["c1"].get<uint32_t>();
            bw->color1.r = (packed >> 24) & 0xFF;
            bw->color1.g = (packed >> 16) & 0xFF;
            bw->color1.b = (packed >> 8) & 0xFF;
            bw->color1.a = packed & 0xFF;
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnBw extras parse error: {}", ex.what());
    }
}

// Lizalfos / Dinolfos (EN_ZF). alpha is the one that matters: the fade path sets
// it to 0 (z_en_zf.c:635), and a mirror frozen there draws a fully TRANSPARENT
// enemy whose body and sword colliders still land. swordSheathed decides whether
// the blade is drawn, headRot aims the head, action gates the blade trail.
static void EnZf_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnZf* zf = (EnZf*)actor;
    x["alp"] = zf->alpha;
    x["hr"] = zf->headRot;
    x["swh"] = zf->swordSheathed;
    x["act"] = zf->action;
}

static void EnZf_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnZf* zf = (EnZf*)actor;
        ReadField(x, "alp", zf->alpha);
        ReadField(x, "hr", zf->headRot);
        ReadField(x, "swh", zf->swordSheathed);
        ReadField(x, "act", zf->action);
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnZf extras parse error: {}", ex.what());
    }
}

// Stalfos (EN_TEST). unk_7DE draws the shield, unk_7C8 is the action id the limb
// draw keys off, meleeWeaponState drives the blade trail. That last one is also
// written from PostLimbDraw (reset to -1 once consumed), which is fine: the
// stream re-supplies it before each frame, so the mirror replays the authority's
// trail instead of inventing one.
static void EnTest_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnTest* test = (EnTest*)actor;
    x["u7c8"] = test->unk_7C8;
    x["u7de"] = test->unk_7DE;
    x["mws"] = test->meleeWeaponState;
    // The limb draw adds headRot into the neck rotation, and only
    // EnTest_UpdateHeadRot writes it. Frozen, the mirror's Stalfos stares dead
    // ahead while the authority's tracks the player.
    x["hr"] = test->headRot;
}

static void EnTest_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnTest* test = (EnTest*)actor;
        ReadField(x, "u7c8", test->unk_7C8);
        ReadField(x, "u7de", test->unk_7DE);
        ReadField(x, "mws", test->meleeWeaponState);
        ReadField(x, "hr", test->headRot);

        // Undo the stream for ACTOR_FLAG_REACT_TO_LENS on an invisible Stalfos.
        //
        // The flag is in STREAMED_ACTOR_FLAGS, and that is right for actors where
        // it is genuine AI state -- Bongo Bongo's vanish, Dead Hand's wait -- so it
        // cannot simply be dropped from the list. Here it is not state: EnTest_Update
        // derives it from play->actorCtx.lensActive, THIS client's Lens of Truth. A
        // mirror never runs that update, so it inherited the other player's lens.
        //
        // Re-deriving it here rather than at the draw is what makes it correct
        // everywhere at once: Actor_UpdateAll reads this same flag to decide whether
        // the actor goes into invisibleActors[] (drawn masked by the lens circle) or
        // is drawn plainly, and ACTOR_FLAG_ATTENTION_ENABLED rides with it for
        // Z-targeting. Fixing only the limb draw left the flag lying: with our lens
        // up and the peer's down, the engine drew the Stalfos plainly, full screen,
        // outside the lens circle and untargetable.
        if (gPlayState != NULL && test->actor.params == STALFOS_TYPE_INVISIBLE) {
            if (gPlayState->actorCtx.lensActive) {
                test->actor.flags |= ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_REACT_TO_LENS;
                test->actor.shape.shadowDraw = ActorShadow_DrawFeet;
            } else {
                test->actor.flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_REACT_TO_LENS);
                test->actor.shape.shadowDraw = NULL;
            }
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnTest extras parse error: {}", ex.what());
    }
}

// Wolfos (EN_WF). Only the eye texture actually diverges: the draw guard on
// action / unk_300 is dead code — the decomp comment at z_en_wf.c:1443 states the
// condition is always true — so a frozen action never hides the mirror's copy.
// action rides along anyway; it costs nothing and keeps the pair consistent.
static void EnWf_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnWf* wf = (EnWf*)actor;
    x["eye"] = wf->eyeIndex;
    x["act"] = wf->action;
    // The head-turn, exactly the same field and the same symptom as EnTest::headRot
    // above: func_80B36F40 writes it from Update only, and OverrideLimbDraw subtracts
    // its .y from the head and eye limbs. Frozen, the mirror's Wolfos stares straight
    // ahead while the authority's tracks the player -- and the clamp goes to 0x3127,
    // about 70 degrees, so it is plainly visible on an enemy that circles you.
    x["hr"] = wf->unk_4D4;
}

static void EnWf_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnWf* wf = (EnWf*)actor;
        ReadField(x, "eye", wf->eyeIndex);
        ReadField(x, "act", wf->action);
        ReadField(x, "hr", wf->unk_4D4);
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnWf extras parse error: {}", ex.what());
    }
}

// Keese (EN_FIREFLY). onFire and auraType decide whether the mirror sees a plain
// Keese or a fire/ice one — the difference between walking into it and burning.
// timer positions the orbiting aura effect.
static void EnFirefly_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnFirefly* ff = (EnFirefly*)actor;
    x["fire"] = ff->onFire;
    x["aura"] = ff->auraType;
    x["tmr"] = ff->timer;
    // PostLimbDraw stops drawing the aura once the Keese is dying; that test
    // is on actionFunc, so send the one bit it actually needs.
    x["dying"] = (ff->actionFunc == EnFirefly_Die) ? 1 : 0;
}

static void EnFirefly_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnFirefly* ff = (EnFirefly*)actor;
        ReadField(x, "fire", ff->onFire);
        ReadField(x, "aura", ff->auraType);
        ReadField(x, "tmr", ff->timer);
        if (x.value("dying", 0) != 0) {
            ff->actionFunc = EnFirefly_Die;
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnFirefly extras parse error: {}", ex.what());
    }
}

// Shell Blade (EN_SB). `fire` is the ignition counter its Draw reads.
static void EnSb_SerializeExtras(Actor* actor, nlohmann::json& x) {
    x["fire"] = ((EnSb*)actor)->fire;
}

static void EnSb_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        ReadField(x, "fire", ((EnSb*)actor)->fire);
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnSb extras parse error: {}", ex.what());
    }
}

// Iron Knuckle (EN_IK). drawMode indexes sDrawFuncs, so a frozen mirror keeps
// running the wrong draw function for the whole fight. unk_2FA and unk_2FB are
// the armour bits the limb draw reads to decide which limbs still wear plate:
// stuck at their Init value, the mirror shows an Iron Knuckle still fully
// armoured after the authority has smashed the armour off — losing the one
// visual cue that says how far along the fight is. unk_2F8 is the action id the
// limb draw keys off, unk_2FE the axe trail.
//
// Note on the axe itself: it does hurt the non-authority player. Its quad
// carries AT_ON | AT_TYPE_ENEMY with 0x40 of damage in the collider (sQuadInit,
// z_en_ik.c:115), and SubmitColliders streams AT quad vertices, so the mirrored
// axe lands through the normal player damage path. Only the explicit
// damagePlayer call — which exists to pierce invincibility frames — is
// authority-only, so the mirror simply keeps its i-frames. Not worth a packet.
static void EnIk_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnIk* ik = (EnIk*)actor;
    x["dm"] = ik->drawMode;
    x["u2f8"] = ik->unk_2F8;
    x["u2fa"] = ik->unk_2FA;
    x["u2fb"] = ik->unk_2FB;
    x["u2fe"] = ik->unk_2FE;
}

static void EnIk_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnIk* ik = (EnIk*)actor;
        ReadField(x, "dm", ik->drawMode);
        ReadField(x, "u2f8", ik->unk_2F8);
        ReadField(x, "u2fa", ik->unk_2FA);
        ReadField(x, "u2fb", ik->unk_2FB);
        ReadField(x, "u2fe", ik->unk_2FE);
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnIk extras parse error: {}", ex.what());
    }
}

// Wallmaster (EN_WALLMAS). Both its draw functions branch on actionFunc, and
// the shadow on the floor is sized from timer: EnWallmas_Draw skips the skeleton
// entirely while the hand is still WaitToDrop, and EnWallmas_DrawXlu picks
// between the actor scale and a timer-derived radius. Frozen on the mirror, the
// hand is drawn in a state that has nothing to do with the position the stream
// is giving it — a shadow stuck at one size under a hand that has already
// dropped, or no hand at all above a shadow that keeps growing.
//
// actionFunc is a function pointer, so the value itself means nothing across
// two processes. These are non-static symbols, so map them to an index and back.
// The table doubles as a clean handoff if authority changes: the copy resumes
// in the authority's state instead of whatever it was left in.
static EnWallmasActionFunc sWallmasActions[] = {
    NULL,
    EnWallmas_WaitToDrop,
    EnWallmas_Drop,
    EnWallmas_Land,
    EnWallmas_Stand,
    EnWallmas_JumpToCeiling,
    EnWallmas_ReturnToCeiling,
    EnWallmas_TakeDamage,
    EnWallmas_Cooldown,
    EnWallmas_Die,
    EnWallmas_TakePlayer,
    EnWallmas_WaitForProximity,
    EnWallmas_WaitForSwitchFlag,
    EnWallmas_Stun,
    EnWallmas_Walk,
};

static void EnWallmas_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnWallmas* wm = (EnWallmas*)actor;
    uint8_t idx = 0;
    for (uint8_t i = 1; i < ARRAY_COUNT(sWallmasActions); i++) {
        if (wm->actionFunc == sWallmasActions[i]) {
            idx = i;
            break;
        }
    }
    x["af"] = idx;
    x["tmr"] = wm->timer;
}

static void EnWallmas_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnWallmas* wm = (EnWallmas*)actor;
        if (x.contains("af")) {
            uint8_t idx = x["af"].get<uint8_t>();
            // 0 means the authority held an actionFunc we do not know: leave ours
            // alone rather than nulling the pointer an authority handoff would run.
            if (idx > 0 && idx < ARRAY_COUNT(sWallmasActions)) {
                wm->actionFunc = sWallmasActions[idx];
            }
        }
        ReadField(x, "tmr", wm->timer);
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnWallmas extras parse error: {}", ex.what());
    }
}

// Baby Dodongo (EN_DODOJR). Its whole draw is a two-way switch on actionFunc —
// inflated/exploding form versus the normal one — and nothing else. Frozen at
// Init, the mirror shows the wrong body for the entire life of the actor,
// including the swollen state that says it is about to burst. Same portable
// index trick as the Wallmaster above; these symbols are non-static too.
// All fifteen states, not just the two the draw switch tests: the point of the
// table is also a clean authority handoff, and a copy resuming in an unlisted
// state would silently keep whatever it was left in. The swollen about-to-burst
// chain (func_809F7AB8 -> func_809F7B3C) is exactly that case.
static EnDodojrActionFunc sDodojrActions[] = {
    NULL,          func_809F73AC, func_809F7BE4, func_809F74C4, func_809F758C, func_809F768C,
    func_809F773C, func_809F77AC, func_809F784C, func_809F786C, func_809F78EC, func_809F799C,
    func_809F7A00, func_809F7AB8, func_809F7B3C, func_809F7C48,
};

static void EnDodojr_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnDodojr* dj = (EnDodojr*)actor;
    uint8_t idx = 0;
    for (uint8_t i = 1; i < ARRAY_COUNT(sDodojrActions); i++) {
        if (dj->actionFunc == sDodojrActions[i]) {
            idx = i;
            break;
        }
    }
    x["af"] = idx;
}

static void EnDodojr_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnDodojr* dj = (EnDodojr*)actor;
        if (x.contains("af")) {
            uint8_t idx = x["af"].get<uint8_t>();
            if (idx > 0 && idx < ARRAY_COUNT(sDodojrActions)) {
                dj->actionFunc = sDodojrActions[idx];
            }
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnDodojr extras parse error: {}", ex.what());
    }
}

// Spike (EN_NY). Its draw picks between two skins and decides whether the spikes
// are out, from three fields the action functions own:
//   unk_1D8  metal skin alpha, unk_1D4 rock skin alpha -- the crossfade between
//            the rolling form and the stone form,
//   unk_1E0  the spike scale, drawn only above 0.25.
// Init leaves them on metal, spikes fully out, so a mirror shows a Spike that is
// permanently armed while the authority has turned it to stone -- exactly the cue
// that says when it is harmless and can be hit. The sphere collider's dim.scale is
// written in the same update and read back by the Collider_UpdateSpheres in the
// draw, so it stayed at full size too.
static void EnNy_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnNy* ny = (EnNy*)actor;
    x["d4"] = ny->unk_1D4;
    x["d8"] = ny->unk_1D8;
    x["e0"] = ny->unk_1E0;
    x["cs"] = ny->elements[0].dim.scale;
}

static void EnNy_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnNy* ny = (EnNy*)actor;
        ReadField(x, "d4", ny->unk_1D4);
        ReadField(x, "d8", ny->unk_1D8);
        ReadField(x, "e0", ny->unk_1E0);
        ReadField(x, "cs", ny->elements[0].dim.scale);
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnNy extras parse error: {}", ex.what());
    }
}

// Octorok (EN_OKUTA). headScale is the snout deformation -- the swell before it
// spits, the suck as it drops back into the water. EnOkuta_UpdateHeadScale reads
// the CURRENT action function to pick the shape, so nothing about it can be derived
// on a mirror, whose actionFunc pointer is its own. Left frozen it is merely stiff,
// except in one case that is worse: if suppression starts before the first local
// update, headScale is still {0,0,0} from the actor memset and the limb draw scales
// the head to nothing -- a headless Octorok. Three floats, only while it moves.
static void EnOkuta_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnOkuta* ok = (EnOkuta*)actor;
    x["hsx"] = ok->headScale.x;
    x["hsy"] = ok->headScale.y;
    x["hsz"] = ok->headScale.z;
}

static void EnOkuta_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnOkuta* ok = (EnOkuta*)actor;
        ReadField(x, "hsx", ok->headScale.x);
        ReadField(x, "hsy", ok->headScale.y);
        ReadField(x, "hsz", ok->headScale.z);
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnOkuta extras parse error: {}", ex.what());
    }
}

// Freezard (EN_FZ). Three faults stacked on one actor.
//
// envAlpha is the alpha the body draw hands to gDPSetEnvColor, and the combiner
// multiplies the final alpha by ENVIRONMENT. Only Init, Appear, Disappear and Melt
// write it, all from the update. For the instances spawned with params < 0, Init sets
// it to 0 and EnFz_Appear is what raises it to 255 — so on a mirror those Freezards
// were completely transparent, and still solid, and still freezing.
//
// state drives which spawn function fills the ice-smoke table, and isActive gates the
// body draw. Both are streamed so the mirror's own breath (run from EnFz_Draw, see the
// overlay) starts and stops on the same frames as the authority's.
//
// customColliderPos: collider3 is the freeze hitbox, and the overlay walks it onto the
// breath particle by hand. The generic cylinder path would drag it back onto the
// actor's own position every frame, which is what put an invisible freeze on the
// Freezard's body. Taking ownership means placing the other two ourselves — they are
// the body colliders and belong exactly where the generic path would have put them.
static void EnFz_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnFz* fz = (EnFz*)actor;
    x["ea"] = fz->envAlpha;
    x["st"] = fz->state;
    x["act"] = fz->isActive;
    x["frz"] = fz->isFreezing;
    x["ish"] = fz->iceSmokeFreezingSpawnHeight;
    x["ut2"] = fz->unusedTimer2;
}

static void EnFz_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnFz* fz = (EnFz*)actor;
        ReadField(x, "ea", fz->envAlpha);
        ReadField(x, "st", fz->state);
        ReadField(x, "act", fz->isActive);
        ReadField(x, "frz", fz->isFreezing);
        ReadField(x, "ish", fz->iceSmokeFreezingSpawnHeight);
        ReadField(x, "ut2", fz->unusedTimer2);
        // state indexes a four-entry table in the overlay; a corrupt or hostile packet
        // must not walk off it.
        if (fz->state > 3) {
            fz->state = 3;
        }
        // We own cylinder placement now: put the two body colliders where the generic
        // path would have, and leave collider3 on the breath particle.
        Collider_UpdateCylinder(actor, &fz->collider1);
        Collider_UpdateCylinder(actor, &fz->collider2);
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnFz extras parse error: {}", ex.what());
    }
}

static void Register(int16_t id, void (*ser)(Actor*, nlohmann::json&),
                     void (*deser)(Actor*, const nlohmann::json&)) {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = ser;
    adapter.DeserializeExtras = deser;
    EnemySync::RegisterAdapter(id, adapter);
}

// Flare Dancer (EN_FD), the Fire Temple mini-boss. The worst case of this whole
// class: its ENTIRE body draw sits behind
//     if (this->actionFunc != EnFd_Reappear && !(this->fadeAlpha < 0.9f))
// and both fields are written only by Update. EnFd_Init leaves actionFunc on
// EnFd_Reappear and fadeAlpha at 0, so on a mirror the test failed forever while
// the 12-sphere AT collider was still dragged along and submitted -- an invisible
// mini-boss that takes your hearts. invincibilityTimer comes too: the limb
// override blanks limbs 13 and 21 while it runs, which is the only visual cue
// that the core is out and the body is open.
//
// Not carried: coreActive, spinTimer, attackTimer and the rest are read by Update
// alone, and a mirror's Update never runs.
static EnFdActionFunc sFdActions[] = {
    NULL, EnFd_Run, EnFd_SpinAndSpawnFire, EnFd_Reappear, EnFd_SpinAndGrow, EnFd_JumpToGround, EnFd_WaitForCore,
    EnFd_Land,
};

static void EnFd_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnFd* fd = (EnFd*)actor;
    uint8_t idx = 0;
    for (uint8_t i = 1; i < ARRAY_COUNT(sFdActions); i++) {
        if (fd->actionFunc == sFdActions[i]) {
            idx = i;
            break;
        }
    }
    x["af"] = idx;
    // 0..255 fade, and the draw gate only asks whether it is below 0.9 -- whole
    // units are plenty and keep this off the float path.
    x["fa"] = (int16_t)fd->fadeAlpha;
    x["inv"] = fd->invincibilityTimer;
    // EnFd_Fade mirrors fadeAlpha into the shadow, and EnFd_Reappear puts it back
    // to 0xFF. Frozen, it leaves an opaque disc on the floor under a body that has
    // faded out -- an artefact the fade fix above CREATES, since before it nothing
    // was drawn at all to be inconsistent with.
    x["sa"] = fd->actor.shape.shadowAlpha;
}

static void EnFd_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnFd* fd = (EnFd*)actor;
        if (x.contains("af")) {
            uint8_t idx = x["af"].get<uint8_t>();
            // 0 means the authority held an actionFunc we do not know: keep ours
            // rather than nulling a pointer an authority handoff would then call.
            if (idx > 0 && idx < ARRAY_COUNT(sFdActions)) {
                fd->actionFunc = sFdActions[idx];
            }
        }
        if (x.contains("fa")) {
            fd->fadeAlpha = (f32)x["fa"].get<int16_t>();
        }
        ReadField(x, "inv", fd->invincibilityTimer);
        ReadField(x, "sa", fd->actor.shape.shadowAlpha);
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnFd extras parse error: {}", ex.what());
    }
}

// Dodongo (EN_DODONGO). bodyScale is applied to limbs 15 and 16 in the limb
// override, and only EnDodongo_SwallowBomb writes it, from colorFilterTimer. A
// mirror keeps the 1.0 its Init set, so the Dodongo that swallowed a bomb stays
// its normal size on the peer's screen -- losing the one cue that says step back.
//
// Deliberately NOT carried: actionState. It gates the tail blur, which is only
// cosmetic, but it ALSO gates EnDodongo_UpdateQuad in PostLimbDraw, which rewrites
// the AT quad vertices from skelAnime.curFrame -- and curFrame is frozen on a
// mirror. Streaming actionState alone would drive that rewrite off a stale frame;
// it would need curFrame with it, for a tail trail.
static void EnDodongo_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnDodongo* dd = (EnDodongo*)actor;
    x["bs"] = dd->bodyScale;
}

static void EnDodongo_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnDodongo* dd = (EnDodongo*)actor;
        ReadField(x, "bs", dd->bodyScale);
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnDodongo extras parse error: {}", ex.what());
    }
}

void RegisterFrozenDrawAdapters() {
    Register(ACTOR_EN_FD, EnFd_SerializeExtras, EnFd_DeserializeExtras);
    Register(ACTOR_EN_DODONGO, EnDodongo_SerializeExtras, EnDodongo_DeserializeExtras);
    Register(ACTOR_EN_BB, EnBb_SerializeExtras, EnBb_DeserializeExtras);
    Register(ACTOR_EN_BW, EnBw_SerializeExtras, EnBw_DeserializeExtras);
    Register(ACTOR_EN_ZF, EnZf_SerializeExtras, EnZf_DeserializeExtras);
    Register(ACTOR_EN_TEST, EnTest_SerializeExtras, EnTest_DeserializeExtras);
    Register(ACTOR_EN_WF, EnWf_SerializeExtras, EnWf_DeserializeExtras);
    Register(ACTOR_EN_FIREFLY, EnFirefly_SerializeExtras, EnFirefly_DeserializeExtras);
    Register(ACTOR_EN_SB, EnSb_SerializeExtras, EnSb_DeserializeExtras);
    Register(ACTOR_EN_IK, EnIk_SerializeExtras, EnIk_DeserializeExtras);
    Register(ACTOR_EN_WALLMAS, EnWallmas_SerializeExtras, EnWallmas_DeserializeExtras);
    Register(ACTOR_EN_DODOJR, EnDodojr_SerializeExtras, EnDodojr_DeserializeExtras);
    Register(ACTOR_EN_NY, EnNy_SerializeExtras, EnNy_DeserializeExtras);
    Register(ACTOR_EN_OKUTA, EnOkuta_SerializeExtras, EnOkuta_DeserializeExtras);

    // Freezard owns its cylinder placement — see the note above EnFz_SerializeExtras.
    ActorSyncAdapter fz;
    fz.SerializeExtras = EnFz_SerializeExtras;
    fz.DeserializeExtras = EnFz_DeserializeExtras;
    fz.customColliderPos = true;
    EnemySync::RegisterAdapter(ACTOR_EN_FZ, fz);
}
