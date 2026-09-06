#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
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
// Baby Dodongo action functions: non-static in z_en_dodojr.c. Its draw switch
// compares actionFunc against these two, so they need a portable index too.
void func_809F73AC(EnDodojr* self, PlayState* play);
void func_809F7BE4(EnDodojr* self, PlayState* play);
void func_809F74C4(EnDodojr* self, PlayState* play);
void func_809F758C(EnDodojr* self, PlayState* play);
void func_809F773C(EnDodojr* self, PlayState* play);
void func_809F77AC(EnDodojr* self, PlayState* play);
void func_809F784C(EnDodojr* self, PlayState* play);
void func_809F786C(EnDodojr* self, PlayState* play);
void func_809F78EC(EnDodojr* self, PlayState* play);
void func_809F799C(EnDodojr* self, PlayState* play);
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
static void EnBw_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnBw* bw = (EnBw*)actor;
    x["u220"] = bw->unk_220;
    x["u23a"] = bw->unk_23A;
    x["u248"] = bw->unk_248;
    x["u258"] = bw->unk_258;
    x["u260"] = bw->unk_260;
}

static void EnBw_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnBw* bw = (EnBw*)actor;
        ReadField(x, "u220", bw->unk_220);
        ReadField(x, "u23a", bw->unk_23A);
        ReadField(x, "u248", bw->unk_248);
        ReadField(x, "u258", bw->unk_258);
        ReadField(x, "u260", bw->unk_260);
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
}

static void EnTest_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnTest* test = (EnTest*)actor;
        ReadField(x, "u7c8", test->unk_7C8);
        ReadField(x, "u7de", test->unk_7DE);
        ReadField(x, "mws", test->meleeWeaponState);
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
}

static void EnWf_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnWf* wf = (EnWf*)actor;
        ReadField(x, "eye", wf->eyeIndex);
        ReadField(x, "act", wf->action);
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
static EnDodojrActionFunc sDodojrActions[] = {
    NULL,          func_809F73AC, func_809F7BE4, func_809F74C4, func_809F758C, func_809F773C,
    func_809F77AC, func_809F784C, func_809F786C, func_809F78EC, func_809F799C,
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

static void Register(int16_t id, void (*ser)(Actor*, nlohmann::json&),
                     void (*deser)(Actor*, const nlohmann::json&)) {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = ser;
    adapter.DeserializeExtras = deser;
    EnemySync::RegisterAdapter(id, adapter);
}

void RegisterFrozenDrawAdapters() {
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
}
