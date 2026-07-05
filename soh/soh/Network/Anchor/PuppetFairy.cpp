#include "PuppetFairy.h"
#include "Anchor.h"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <string>
#include <unordered_map>
#include <cmath>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_En_Elf/z_en_elf.h"

extern PlayState* gPlayState;
}

// One cosmetic companion fairy per remote-player puppet so every screen shows a
// fairy per player (vanilla only draws the local player's own Navi).
//
// The fairy is a plain ACTOR_EN_ELF spawned with params FAIRY_KOKIRI — a pure
// parent-follower (action func func_80A0353C) with no Navi AI, no C-up/talk
// update, and no attention flag. Its Init reads no parent, so we can spawn it
// parentless. We deliberately do NOT use the engine's parent-follow: that path
// self-kills when the parent dies, and RefreshClientActors frees/respawns
// puppets wholesale, so a raw-Actor* parent would be a same-frame
// use-after-free. Instead we suppress the fairy's own update via a
// ShouldActorUpdate override and drive a custom follow that re-resolves the
// puppet from client.player every frame (never cached — Task 2 discipline).
//
// clientId -> fairy Actor*. Only fairies WE spawned live here; the local Navi
// and every other En_Elf are absent, which is the discriminator that keeps our
// override off them.
static std::unordered_map<uint32_t /*clientId*/, Actor*> sFairies;

// Parked sentinel: puppets get shoved to -9999 by DummyPlayer when out of scene,
// and we mirror that for the fairy. Anything this far out is "parked".
static constexpr f32 PARKED_COORD = -9999.0f;

static bool IsGlobalRoom() {
    return std::string("soh-global") == CVarGetString(CVAR_REMOTE_ANCHOR("RoomId"), "");
}

// CLAMP a per-axis velocity target to +/-20, preserving sign (matches the
// engine follower's func_80A02C98 / func_80A02BD8 clamp).
static f32 ClampFairyVel(f32 v) {
    f32 sign = (v >= 0.0f) ? 1.0f : -1.0f;
    v = fabsf(v);
    if (v > 20.0f) {
        v = 20.0f;
    }
    return v * sign;
}

static void SnapFairyAway(EnElf* elf) {
    elf->actor.world.pos.x = PARKED_COORD;
    elf->actor.world.pos.y = PARKED_COORD;
    elf->actor.world.pos.z = PARKED_COORD;
    elf->actor.velocity.x = 0.0f;
    elf->actor.velocity.y = 0.0f;
    elf->actor.velocity.z = 0.0f;
}

// Custom follow for one of our fairies. Reimplements the file-static
// func_80A02A20 (hover-bob) + func_80A02C98/func_80A02BD8 (clamped velocity
// solve) from z_en_elf.c against the puppet instead of an Actor* parent.
static void DrivePuppetFairy(uint32_t clientId, Actor* actor) {
    EnElf* elf = (EnElf*)actor;

    // Re-resolve the puppet every frame; never cache a raw Actor* — the puppet
    // may have been freed and respawned by RefreshClientActors. Only touch
    // client.player->... behind the online + non-null guard.
    Player* puppet = nullptr;
    AnchorClient* client = nullptr;
    if (Anchor::Instance != nullptr) {
        auto cit = Anchor::Instance->clients.find(clientId);
        if (cit != Anchor::Instance->clients.end() && cit->second.online &&
            cit->second.player != nullptr && cit->second.player->actor.update != NULL) {
            client = &cit->second;
            puppet = cit->second.player;
        }
    }

    // Parked: puppet in another scene / offline / save not loaded (same predicate
    // DummyPlayer uses). SNAP away — a lerp under the 20/frame clamp would streak
    // the fairy across the whole map. The lifecycle tick reaps it shortly after.
    bool parked = (client == nullptr) || (gPlayState == nullptr) ||
                  (client->sceneNum != gPlayState->sceneNum) || !client->isSaveLoaded || !client->online;
    if (parked) {
        SnapFairyAway(elf);
        return;
    }

    Vec3f puppetPos = puppet->actor.world.pos;
    f32 hover = (1500.0f * elf->actor.scale.y) + 40.0f; // matches z_en_elf.c:687

    // Just came back from being parked → snap next to the puppet before resuming
    // (otherwise it would slowly crawl back from -9999 at 20/frame).
    if (elf->actor.world.pos.x <= (PARKED_COORD + 1.0f)) {
        elf->actor.world.pos.x = puppetPos.x;
        elf->actor.world.pos.y = puppetPos.y + hover;
        elf->actor.world.pos.z = puppetPos.z;
        elf->actor.velocity.x = 0.0f;
        elf->actor.velocity.y = 0.0f;
        elf->actor.velocity.z = 0.0f;
    }

    SkelAnime_Update(&elf->skelAnime);

    // Hover-bob offset — func_80A02A20 with the case-0 params EnElf_Init set for
    // FAIRY_KOKIRI (unk_2AE/2B0/2B4/2B8 already initialised there).
    elf->unk_28C.x = Math_SinS(elf->unk_2AC) * elf->unk_2B8;
    elf->unk_28C.y = Math_SinS(elf->unk_2AA) * elf->unk_2B4;
    elf->unk_28C.z = Math_CosS(elf->unk_2AC) * elf->unk_2B8;
    elf->unk_2AC += elf->unk_2B0;
    elf->unk_2AA += elf->unk_2AE;

    // Follow target = puppet world.pos + hover height.
    Vec3f target = puppetPos;
    target.y += hover;

    // func_80A02C98 (xz) + func_80A02BD8 (y): velocity toward target * 0.2,
    // clamped to 20/axis, stepped in.
    const f32 rate = 0.2f;
    f32 xVel = ClampFairyVel(((target.x + elf->unk_28C.x) - elf->actor.world.pos.x) * rate);
    f32 yVel = ClampFairyVel(((target.y + elf->unk_28C.y) - elf->actor.world.pos.y) * rate);
    f32 zVel = ClampFairyVel(((target.z + elf->unk_28C.z) - elf->actor.world.pos.z) * rate);

    Math_StepToF(&elf->actor.velocity.y, yVel, 32.0f);
    Math_StepToF(&elf->actor.velocity.x, xVel, 1.5f);
    Math_StepToF(&elf->actor.velocity.z, zVel, 1.5f);
    Actor_UpdatePos(&elf->actor);

    // Face travel direction (func_80A0353C tail) and advance the draw timer that
    // drives the env-alpha shimmer (z_en_elf.c:1517). EnElf_Update normally does
    // both of these; we do them here since its update is suppressed.
    elf->unk_2BC = Math_Atan2S(elf->actor.velocity.z, elf->actor.velocity.x);
    elf->actor.shape.rot.y = elf->unk_2BC;
    elf->timer++;

    // Keep the two point lights riding with the fairy (EnElf_UpdateLights default
    // branch: glow radius 100, no-glow radius -1, white).
    Lights_PointGlowSetInfo(&elf->lightInfoGlow, (s16)elf->actor.world.pos.x, (s16)elf->actor.world.pos.y,
                            (s16)elf->actor.world.pos.z, 255, 255, 255, 100);
    Lights_PointNoGlowSetInfo(&elf->lightInfoNoGlow, (s16)elf->actor.world.pos.x, (s16)elf->actor.world.pos.y,
                              (s16)elf->actor.world.pos.z, 255, 255, 255, -1);
}

// Per-frame lifecycle: reap fairies whose puppet is gone, then spawn one for
// every live puppet that lacks a fairy.
static void PuppetFairyLifecycle() {
    if (Anchor::Instance == nullptr || gPlayState == nullptr) {
        return;
    }

    auto& clients = Anchor::Instance->clients;

    // 1) Reap. A puppet is live only when the client is online with a non-null
    //    player whose actor is still updating. Read client.player->actor.update
    //    only behind that guard so we never dereference a freed puppet. Erase the
    //    map entry in lockstep with the kill so no stale Actor* survives.
    for (auto it = sFairies.begin(); it != sFairies.end();) {
        auto cit = clients.find(it->first);
        bool puppetLive = (cit != clients.end()) && !cit->second.self && cit->second.online &&
                          cit->second.player != nullptr && cit->second.player->actor.update != NULL;
        if (!puppetLive) {
            Actor_Kill(it->second);
            it = sFairies.erase(it);
        } else {
            ++it;
        }
    }

    // 2) Spawn. Suppressed in the global room, same as nametags (DummyPlayer.cpp).
    if (IsGlobalRoom()) {
        return;
    }

    for (auto& [clientId, client] : clients) {
        if (client.self || !client.online || client.player == nullptr ||
            client.player->actor.update == NULL) {
            continue;
        }
        if (sFairies.count(clientId)) {
            continue;
        }

        Vec3f pos = client.player->actor.world.pos;
        Actor* fairy = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_ELF, pos.x, pos.y + 60.0f, pos.z, 0,
                                   0, 0, FAIRY_KOKIRI, false);
        if (fairy == nullptr) {
            continue;
        }

        // Tint the outer (aura) color with the client's color; the inner core
        // stays white. Draw reads outerColor.rgb + a timer-driven env alpha, so
        // this is all the tint the fairy needs.
        EnElf* elf = (EnElf*)fairy;
        elf->outerColor.r = (f32)client.color.r;
        elf->outerColor.g = (f32)client.color.g;
        elf->outerColor.b = (f32)client.color.b;

        sFairies[clientId] = fairy;
    }
}

// Per-frame entry point, called by the Anchor per-frame dispatcher in explicit tick
// order (see HookHandlers.cpp).
void PuppetFairyTick() {
    PuppetFairyLifecycle();
}

void RegisterPuppetFairyHooks(bool isConnected) {
    // On disconnect the OnActorDestroy/OnSceneInit hooks are torn down, so any
    // surviving entries would go stale and a reconnect's first tick could iterate
    // dangling Actor*. Drop the map. The orphaned fairy actors self-destruct on
    // their own: with the ShouldActorUpdate override gone, their normal update
    // (func_80A0353C) runs, sees the NULL parent, and Actor_Kills them. Mirrors
    // EnemySync::RegisterHooks' Reset-on-disconnect.
    if (!isConnected) {
        sFairies.clear();
    }

    // NOTE: the spawn/reap lifecycle is NOT registered here; it is driven by the Anchor
    // per-frame dispatcher (PuppetFairyTick) so the Anchor-internal tick order is explicit.
    // The disconnect path above still drops the tracking map.

    // Follow override: suppress each of our fairies' own update and drive it. Any
    // En_Elf NOT in sFairies (local Navi, forest fairies, healing fairies) falls
    // straight through untouched — that absence is the discriminator.
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_ELF, isConnected, [](void* actorRef, bool* should) {
        Actor* actor = (Actor*)actorRef;
        for (auto& [clientId, fairy] : sFairies) {
            if (fairy == actor) {
                *should = false;
                DrivePuppetFairy(clientId, actor);
                return;
            }
        }
    });

    // Scene teardown frees the fairy actors; drop any entry the instant its actor
    // is destroyed so the follow override never touches a freed pointer.
    COND_HOOK(OnActorDestroy, isConnected, [](void* actorRef) {
        Actor* actor = (Actor*)actorRef;
        for (auto it = sFairies.begin(); it != sFairies.end();) {
            if (it->second == actor) {
                it = sFairies.erase(it);
            } else {
                ++it;
            }
        }
    });

    // Belt-and-suspenders: a fresh scene starts with an empty map.
    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) { sFairies.clear(); });
}
