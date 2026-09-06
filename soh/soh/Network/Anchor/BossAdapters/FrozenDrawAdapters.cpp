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
}

/**
 * One bug class, seven enemies: "the mirror's Draw reads a frozen private field".
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
 * Two kinds of field were deliberately left out everywhere:
 * - ice/freeze timers (EnBb::fireIceTimer, EnBw/EnZf/EnTest::iceTimer): the draw
 *   path decrements them itself and bumps colorFilterTimer, so replaying them
 *   would fight the stream every frame.
 * - actionFunc (EnFirefly, EnDodojr): a function pointer. Its value is a local
 *   code address and comparing it against a file-static symbol cannot be
 *   reproduced across processes. EnDodojr's draw switch keys off nothing else,
 *   so that actor gets no adapter at all rather than a broken one.
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
}

static void EnFirefly_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnFirefly* ff = (EnFirefly*)actor;
        ReadField(x, "fire", ff->onFire);
        ReadField(x, "aura", ff->auraType);
        ReadField(x, "tmr", ff->timer);
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
}
