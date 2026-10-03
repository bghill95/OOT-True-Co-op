#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Vm/z_en_vm.h"
}

/**
 * Beamos (EN_VM).
 *
 * The scanning head rotation lives in this->headRotY (0x25C), applied at draw
 * time — it is neither shape.rot nor a jointTable limb, so the generic stream
 * never carries it and a suppressed mirror's head freezes mid-scan while the
 * authority's keeps turning. Stream the field as an extra.
 *
 * The LASER was the bigger hole. Everything that draws it is written in the
 * update, so on the mirror it all stayed at its spawn value — beamScale is not
 * even initialised in Init, so Matrix_Scale ran on zeroes and no beam was drawn
 * at all. The AT quad, meanwhile, IS carried by the generic stream (quadVerts +
 * colEnable) and is resubmitted: the peer was taking an invisible laser. That is
 * the "invisible and deadly" sub-case the backlog calls out, so the fix is to
 * make the beam visible rather than to stop submitting the quad.
 *
 *   beamScale  x is the beam's thickness, z its length.
 *   beamRot    where it points.
 *   unk_260    the beam's own phase counter: it gates the ground impact in
 *              EnVm_Draw and the range raycast in EnVm_PostLimbDraw.
 *   unk_21C    the action state; the limb override hides limb 10 on unk_21C == 3
 *              (dying), which the mirror never reached.
 *
 * Not streamed, on purpose:
 *   beamScale.z is recomputed by EnVm_PostLimbDraw's raycast whenever unk_260
 *   reaches 3 — but only when the ray HITS, so the streamed value is still the
 *   right starting point and we carry it.
 *   beamTexScroll advances by a constant every frame; the mirror ages it in its
 *   own draw instead of paying for it on the wire (see z_en_vm.c).
 *
 * Death needs no phase handoff: a Beamos dies to a bomb via the generic
 * ENEMY_DIED path (permanent Actor_Kill + force-kill fallback).
 */

static void EnVm_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnVm* vm = (EnVm*)actor;
    x["hry"] = vm->headRotY;
    x["bsx"] = vm->beamScale.x;
    x["bsz"] = vm->beamScale.z;
    x["brx"] = vm->beamRot.x;
    x["bry"] = vm->beamRot.y;
    x["b26"] = vm->unk_260;
    x["b21"] = vm->unk_21C;
}

static void EnVm_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnVm* vm = (EnVm*)actor;
        if (x.contains("hry")) {
            vm->headRotY = x["hry"].get<int16_t>();
        }
        if (x.contains("bsx")) {
            vm->beamScale.x = x["bsx"].get<float>();
        }
        if (x.contains("bsz")) {
            vm->beamScale.z = x["bsz"].get<float>();
        }
        if (x.contains("brx")) {
            vm->beamRot.x = x["brx"].get<int16_t>();
        }
        if (x.contains("bry")) {
            vm->beamRot.y = x["bry"].get<int16_t>();
        }
        if (x.contains("b26")) {
            vm->unk_260 = x["b26"].get<int16_t>();
        }
        if (x.contains("b21")) {
            vm->unk_21C = x["b21"].get<int32_t>();
        }
    } catch (const std::exception& ex) { SPDLOG_WARN("[EnemySync] EnVm extras parse error: {}", ex.what()); }
}

void RegisterEnVmAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = EnVm_SerializeExtras;
    adapter.DeserializeExtras = EnVm_DeserializeExtras;
    EnemySync::RegisterAdapter(ACTOR_EN_VM, adapter);
}
