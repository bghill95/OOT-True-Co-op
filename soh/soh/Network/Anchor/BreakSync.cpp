#include "BreakSync.h"
#include "Anchor.h"
#include "EnemySync.h" // ESYNC_LOG
#include "JsonConversions.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <cmath>
#include <unordered_map>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_En_Kusa/z_en_kusa.h"
#include "src/overlays/actors/ovl_Obj_Tsubo/z_obj_tsubo.h"
#include "src/overlays/actors/ovl_Obj_Kibako/z_obj_kibako.h"
#include "src/overlays/actors/ovl_Obj_Kibako2/z_obj_kibako2.h"
#include "src/overlays/actors/ovl_En_Ishi/z_en_ishi.h"
#include "src/overlays/actors/ovl_Obj_Comb/z_obj_comb.h"
#include "src/overlays/actors/ovl_En_Wood02/z_en_wood02.h"
#include "src/overlays/actors/ovl_En_Kanban/z_en_kanban.h"
extern PlayState* gPlayState;
}

namespace BreakSync {

// One event kind: the receiver picks the spoof channel from the actor id.
//
// Per-actor break channels (from the full breakables scan, 2026-09-02):
//   EN_KUSA      grass    AC_HIT alone (no acHitInfo read); break also killed
//                         is detected via ACTOR_FLAG_GRASS_DESTROYED edge
//                         because a cut bush may survive as a stump (Obj_Mure
//                         regrowth) and never hit OnActorDestroy.
//   OBJ_TSUBO    pots     AC_HIT + acHitInfo->toucher.dmgFlags & 0x4FC1FFFC
//   OBJ_KIBAKO   sm crate AC_HIT alone
//   OBJ_KIBAKO2  bg crate home.rot.z != 0 (the vanilla player-bonk channel)
//   EN_ISHI      rocks    AC_HIT + dmgFlags & 0x40000048, ROCK_SMALL only
//   OBJ_COMB     beehive  AC_HIT + elements[0] acHitInfo; dmgFlags must NOT
//                         match 0x4001F866 (that mask only wobbles it)
//   EN_WOOD02    trees    home.rot.y != 0 (player-bonk drop channel)
// dmgFlags 0x8 (explosive) passes every gate above: & 0x4FC1FFFC != 0,
// & 0x40000048 != 0, & 0x4001F866 == 0.
//
// Known limits, on purpose: EN_KANBAN (cut signs, cosmetic, no clean remote
// trigger), OBJ_HANA (not breakable), Wood02 proximity bushes (local-player
// gated). Drops roll each client's own RNG — same rule as thrown pots.

static constexpr uint32_t SPOOF_DMG_FLAGS = 0x00000008; // explosive

// EnKanbanCutType lives in z_en_kanban.c only; values verified there.
enum KanbanCut : uint8_t {
    KCUT_POST = 0,
    KCUT_VERT_L = 1,
    KCUT_HORIZ = 2,
    KCUT_DIAG_L = 3,
    KCUT_DIAG_R = 4,
    KCUT_VERT_R = 5,
};

static bool IsSyncedBreakable(int16_t id) {
    switch (id) {
        case ACTOR_EN_KUSA:
        case ACTOR_OBJ_TSUBO:
        case ACTOR_OBJ_KIBAKO:
        case ACTOR_OBJ_KIBAKO2:
        case ACTOR_EN_ISHI:
        case ACTOR_OBJ_COMB:
        case ACTOR_EN_WOOD02:
        case ACTOR_EN_KANBAN:
            return true;
        default:
            return false;
    }
}

struct BreakState {
    uint64_t key = 0;
    bool announced = false;      // we already sent BREAK for this actor
    bool remoteBreaking = false; // break was remote-triggered: never echo it
    uint16_t kanbanPrevParts = 0; // signposts break piece by piece, not once
    uint16_t latchTicks = 0;      // frames since remoteBreaking was armed
};

static std::unordered_map<Actor*, BreakState> sTracked;
static std::unordered_map<uint64_t, Actor*> sByKey;

// The attacker element the spoofed AC_HIT points at (Tsubo/Ishi/Comb deref it
// unconditionally — AC_HIT alone would read a stale/NULL pointer).
static ColliderInfo sSpoofHitInfo;

bool SyncEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("BreakSync"), 1);
}

static void Reset() {
    sTracked.clear();
    sByKey.clear();
}

static uint64_t PosKey(const Actor* actor) {
    int16_t qx = (int16_t)lroundf(actor->world.pos.x);
    int16_t qy = (int16_t)lroundf(actor->world.pos.y);
    int16_t qz = (int16_t)lroundf(actor->world.pos.z);
    return ((uint64_t)(uint16_t)qx << 48) | ((uint64_t)(uint16_t)qy << 32) | ((uint64_t)(uint16_t)qz << 16) |
           (uint16_t)actor->id;
}

static void OnBreakableInit(Actor* actor) {
    if (!SyncEnabled() || sTracked.contains(actor)) {
        return;
    }
    if (actor->id == ACTOR_EN_KANBAN && actor->params == ENKANBAN_PIECE) {
        return; // flying sign pieces are dynamic children, not identities
    }
    BreakState st;
    st.key = PosKey(actor);
    sTracked[actor] = st;
    sByKey[st.key] = actor;
}

static void SendBreak(Actor* actor, BreakState& st) {
    if (st.announced || st.remoteBreaking) {
        return;
    }
    st.announced = true;
    nlohmann::json payload;
    payload["key"] = st.key;
    Anchor::Instance->SendPacket_BreakEvent(payload);
    ESYNC_LOG("[BreakSync] break tx id={} key={:#x}", actor->id, st.key);
}

// Grass never reliably dies on cut (stump + regrowth), so watch the flag edge.
static void OnKusaPreUpdate(Actor* actor) {
    auto it = sTracked.find(actor);
    if (it == sTracked.end() || !SyncEnabled()) {
        return;
    }
    if (actor->flags & ACTOR_FLAG_GRASS_DESTROYED) {
        SendBreak(actor, it->second);
    }
}

// Signposts: multiple cuts per post — announce every partFlags shrink with the
// cutType the cutter actually computed (post-adjustment). The mirror replays
// the cut through the vanilla path; the receiver's own spoofed cut shrinks its
// partFlags too, so the remoteBreaking latch absorbs the echo edge.
static void OnKanbanPreUpdate(Actor* actor) {
    auto it = sTracked.find(actor);
    if (it == sTracked.end() || !SyncEnabled()) {
        return;
    }
    BreakState& st = it->second;
    EnKanban* sign = (EnKanban*)actor;
    // A remote cut that never lands (packet raced a state we can't cut) must
    // not gag every later cut: un-stick the latch after a second, loudly.
    if (st.remoteBreaking && ++st.latchTicks > 20) {
        SPDLOG_WARN("[BreakSync] kanban latch timeout key={:#x} parts={:#x} acFlags={:#x} inv={}", st.key,
                    sign->partFlags, sign->collider.base.acFlags, sign->invincibilityTimer);
        st.remoteBreaking = false;
        st.latchTicks = 0;
    }
    if (sign->partFlags < st.kanbanPrevParts) {
        if (st.remoteBreaking) {
            st.remoteBreaking = false;
            st.latchTicks = 0;
        } else {
            nlohmann::json payload;
            payload["key"] = st.key;
            payload["cut"] = sign->cutType;
            Anchor::Instance->SendPacket_BreakEvent(payload);
            ESYNC_LOG("[BreakSync] kanban cut tx key={:#x} cut={}", st.key, sign->cutType);
        }
    }
    st.kanbanPrevParts = sign->partFlags;
}

// While the receiver's spoofed cut runs, the local player's sword-animation
// field is borrowed so sCutTypes[] resolves to the sender's cut style, then
// restored right after the sign's update (OnActorUpdate window only).
static bool sKanbanAnimOverride = false;
static s8 sKanbanSavedAnim = 0;
static Actor* sKanbanOverrideTarget = NULL;

static void OnKanbanPostUpdate(Actor* actor) {
    // Restore ONLY once the targeted sign ran its update: any other EnKanban
    // (flying pieces, other posts) updating first must not undo the override,
    // or the target reads the local player's real last slash instead.
    if (sKanbanAnimOverride && actor == sKanbanOverrideTarget && gPlayState != NULL) {
        GET_PLAYER(gPlayState)->meleeWeaponAnimation = sKanbanSavedAnim;
        sKanbanAnimOverride = false;
        sKanbanOverrideTarget = NULL;
    }
}

// Trees announce on the bonk channel (they drop but never die either).
static void OnWoodPreUpdate(Actor* actor) {
    auto it = sTracked.find(actor);
    if (it == sTracked.end() || !SyncEnabled()) {
        return;
    }
    if (actor->home.rot.y != 0) {
        SendBreak(actor, it->second);
    }
}

void Ingest(const nlohmann::json& payload) {
    try {
        uint64_t key = payload.at("key").get<uint64_t>();
        Actor* actor = NULL;
        auto it = sByKey.find(key);
        if (it != sByKey.end() && it->second->update != NULL) {
            actor = it->second;
        } else {
            // Fuzzy fallback: some spawns settle (floor snap) before the key
            // capture on one client but not the other — the same signpost keyed
            // 15 units apart in Y across sessions. Same actor id, nearest
            // spawn position wins; a miss is loud, never silent.
            int16_t wantId = (int16_t)(key & 0xFFFF);
            Vec3f wantPos = { (float)(int16_t)((key >> 48) & 0xFFFF), (float)(int16_t)((key >> 32) & 0xFFFF),
                              (float)(int16_t)((key >> 16) & 0xFFFF) };
            float bestD = 80.0f;
            for (auto& [a, stt] : sTracked) {
                if (a->id != wantId || a->update == NULL) {
                    continue;
                }
                float d = Math_Vec3f_DistXYZ(&a->home.pos, &wantPos);
                if (d < bestD) {
                    bestD = d;
                    actor = a;
                }
            }
            if (actor == NULL) {
                SPDLOG_WARN("[BreakSync] rx unknown key={:#x} id={} (no fuzzy match)", key, wantId);
                return; // unknown or already dead (e.g. broke by its own physics)
            }
            SPDLOG_WARN("[BreakSync] rx fuzzy-matched key={:#x} id={} d={:.0f}", key, wantId, bestD);
        }
        auto st = sTracked.find(actor);
        if (st == sTracked.end() || st->second.announced || st->second.remoteBreaking) {
            return;
        }
        st->second.remoteBreaking = true;
        st->second.latchTicks = 0;

        // A culled actor never runs its update, so the spoofed AC_HIT would sit
        // unconsumed forever (arrow-shot signpost with the peer's Link far
        // away). Keep every replay target updating from here on.
        actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;

        sSpoofHitInfo.toucher.dmgFlags = SPOOF_DMG_FLAGS;

        if (actor->id == ACTOR_EN_KANBAN) {
            // Replay the cut through the vanilla path. cutType is derived from
            // dmgFlags + the LOCAL player's sword anim, then flipped when the
            // local player stands behind the sign — pick the base so the local
            // flip lands on the sender's cut when reachable (mirror otherwise,
            // cosmetic only). sCutTypes anim indices: 0=VERT_L, 2=DIAG_R, 4=HORIZ.
            EnKanban* sign = (EnKanban*)actor;
            uint8_t cut = payload.value("cut", (uint8_t)0);
            Player* localPlayer = GET_PLAYER(gPlayState);
            int16_t yawDiff = (int16_t)(actor->yawTowardsPlayer - actor->shape.rot.y);
            bool localFlip = yawDiff > 0x4000 || yawDiff < -0x4000;
            if (cut == KCUT_POST) {
                sSpoofHitInfo.toucher.dmgFlags = 0x8; // outside 0x700: post cut
            } else {
                sSpoofHitInfo.toucher.dmgFlags = 0x100; // inside 0x700: styled cut
                s8 anim;
                switch (cut) {
                    case KCUT_HORIZ:
                        anim = 4;
                        break;
                    case KCUT_DIAG_R:
                    case KCUT_DIAG_L:
                        anim = 2; // DIAG_R base; local flip may mirror
                        break;
                    default: // CUT_VERT_L / CUT_VERT_R
                        anim = 0; // VERT_L base; local flip may mirror
                        break;
                }
                // Two cuts ingested in the same packet drain (both before either
                // target updates) used to save the ALREADY-SPOOFED value, and the
                // first sign's restore was lost -- leaving the local player's
                // meleeWeaponAnimation stuck on a spoofed index for good.
                if (sKanbanAnimOverride) {
                    localPlayer->meleeWeaponAnimation = sKanbanSavedAnim;
                }
                sKanbanSavedAnim = localPlayer->meleeWeaponAnimation;
                sKanbanAnimOverride = true;
                sKanbanOverrideTarget = actor;
                localPlayer->meleeWeaponAnimation = anim;
            }
            sign->collider.info.acHitInfo = &sSpoofHitInfo;
            sign->collider.base.acFlags |= AC_HIT;
            sign->invincibilityTimer = 0; // a fresh remote cut must not be i-framed away
            ESYNC_LOG("[BreakSync] kanban cut rx key={:#x} cut={} flip={}", key, cut, localFlip);
            return;
        }

        switch (actor->id) {
            case ACTOR_EN_KUSA:
                ((EnKusa*)actor)->collider.base.acFlags |= AC_HIT;
                break;
            case ACTOR_OBJ_TSUBO: {
                ObjTsubo* tsubo = (ObjTsubo*)actor;
                tsubo->collider.info.acHitInfo = &sSpoofHitInfo;
                tsubo->collider.base.acFlags |= AC_HIT;
                break;
            }
            case ACTOR_OBJ_KIBAKO:
                ((ObjKibako*)actor)->collider.base.acFlags |= AC_HIT;
                break;
            case ACTOR_OBJ_KIBAKO2:
                // The vanilla player-bonk channel; no collider spoof needed.
                actor->home.rot.z = 1;
                break;
            case ACTOR_EN_ISHI: {
                if (actor->params & 1) {
                    break; // ROCK_LARGE can only be lifted, never hit-broken
                }
                EnIshi* ishi = (EnIshi*)actor;
                ishi->collider.info.acHitInfo = &sSpoofHitInfo;
                ishi->collider.base.acFlags |= AC_HIT;
                break;
            }
            case ACTOR_OBJ_COMB: {
                ObjComb* comb = (ObjComb*)actor;
                comb->colliderItems[0].info.acHitInfo = &sSpoofHitInfo;
                comb->collider.base.acFlags |= AC_HIT;
                break;
            }
            case ACTOR_EN_WOOD02:
                actor->home.rot.y = 1; // bonk-drop channel
                break;
            default:
                break;
        }
        ESYNC_LOG("[BreakSync] break rx id={} key={:#x}", actor->id, key);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[BreakSync] malformed BREAK_EVENT: {}", e.what());
    }
}

void RegisterHooks(bool isConnected) {
    if (!isConnected) {
        Reset();
    }

    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) { Reset(); });

    COND_ID_HOOK(OnActorInit, ACTOR_EN_KUSA, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_TSUBO, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_KIBAKO, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_KIBAKO2, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_EN_ISHI, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_COMB, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_EN_WOOD02, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_EN_KANBAN, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_KUSA, isConnected,
                 [](void* actor, bool* should) { OnKusaPreUpdate((Actor*)actor); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_WOOD02, isConnected,
                 [](void* actor, bool* should) { OnWoodPreUpdate((Actor*)actor); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_KANBAN, isConnected,
                 [](void* actor, bool* should) { OnKanbanPreUpdate((Actor*)actor); });
    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_KANBAN, isConnected, [](void* actor) { OnKanbanPostUpdate((Actor*)actor); });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actorRef) {
        Actor* a = (Actor*)actorRef;
        auto it = sTracked.find(a);
        if (it == sTracked.end()) {
            return;
        }
        // A death while connected = it broke (these actors have no other way
        // to die mid-scene). Killed-by-own-physics after a synced THROW breaks
        // on both sides already; the receiver's copy is then dead and Ingest
        // drops the echo.
        // EN_KUSA included: grass that DIES on cut (rather than surviving as a
        // regrowing stump) never trips the GRASS_DESTROYED pre-update edge —
        // the flag is set and the actor killed in the same update frame. The
        // announced latch keeps this from double-sending with the flag edge.
        // ROCK_LARGE (EN_ISHI params bit 0) is excluded: it can only be lifted and
        // thrown, never hit-broken, so Ingest bails out of the replay switch --
        // but only AFTER latching remoteBreaking and forcing culling off, which
        // then blocks that rock from every later break sync in the scene.
        // Actor_Delete WITHOUT a prior Actor_Kill (update still set) is a room
        // unload or the scene teardown, not a break: that used to broadcast one
        // BREAK per tracked prop whenever the sender changed room or scene, and
        // shatter every pot and rock the peer still had loaded around it.
        if (a->update == NULL && SyncEnabled() &&
            (a->id == ACTOR_OBJ_TSUBO || a->id == ACTOR_OBJ_KIBAKO || a->id == ACTOR_OBJ_KIBAKO2 ||
                              (a->id == ACTOR_EN_ISHI && !(a->params & 1)) || a->id == ACTOR_OBJ_COMB ||
                              a->id == ACTOR_EN_KUSA)) {
            SendBreak(a, it->second);
        }
        auto bk = sByKey.find(it->second.key);
        if (bk != sByKey.end() && bk->second == a) {
            sByKey.erase(bk);
        }
        sTracked.erase(it);
    });
}

} // namespace BreakSync
