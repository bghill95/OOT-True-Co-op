#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Rr/z_en_rr.h"
}

/**
 * Like Like (EN_RR).
 *
 * The Like Like has no skeleton, so the generic stream's jointTable carries
 * nothing for it: EnRr_Draw builds the entire body by walking bodySegs[5] and
 * applying each segment's height, scale, scaleMod and rot to the matrix stack.
 * All of that is AI-computed. On a suppressed mirror the array stays at whatever
 * EnRr_InitBodySegments left, so the enemy is drawn as a rigid column at its
 * spawn proportions while the streamed world.pos drags it around — including,
 * on the three that guard the Gerudo Training Ground chest room, the swallowing
 * animation that never plays.
 *
 * Cost is comparable to what a skeletal enemy already pays: 5 segments x 10
 * values, against 24 limbs x 3 for a jointTable. Sent as one flat array so the
 * payload stays compact and its length alone validates the shape.
 *
 * Not carried:
 * - mouthPos and effectPos[] are written from inside Draw via Matrix_MultVec3f,
 *   so the mirror derives them itself from the segments above.
 * - actionFunc is a function pointer (a local code address); the draw path does
 *   not read it.
 * - grabTimer / hasPlayer and friends belong to the grab, which only the
 *   authority can run: see the wrong-player note in the audit backlog.
 */

// height + scale(3) + scaleMod(3) + rot(3)
#define RR_SEG_VALUES 10
#define RR_SEG_COUNT 5

static void EnRr_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnRr* rr = (EnRr*)actor;
    nlohmann::json segs = nlohmann::json::array();
    for (int32_t i = 0; i < RR_SEG_COUNT; i++) {
        EnRrBodySegment* s = &rr->bodySegs[i];
        segs.push_back(s->height);
        segs.push_back(s->scale.x);
        segs.push_back(s->scale.y);
        segs.push_back(s->scale.z);
        segs.push_back(s->scaleMod.x);
        segs.push_back(s->scaleMod.y);
        segs.push_back(s->scaleMod.z);
        segs.push_back(s->rot.x);
        segs.push_back(s->rot.y);
        segs.push_back(s->rot.z);
    }
    x["segs"] = segs;
    x["eft"] = rr->effectTimer;
    x["sct"] = rr->scrollTimer;
}

static void EnRr_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnRr* rr = (EnRr*)actor;
        if (x.contains("segs") && x["segs"].is_array() &&
            x["segs"].size() == (size_t)(RR_SEG_COUNT * RR_SEG_VALUES)) {
            const nlohmann::json& segs = x["segs"];
            for (int32_t i = 0; i < RR_SEG_COUNT; i++) {
                EnRrBodySegment* s = &rr->bodySegs[i];
                size_t o = (size_t)i * RR_SEG_VALUES;
                s->height = segs[o + 0].get<float>();
                s->scale.x = segs[o + 1].get<float>();
                s->scale.y = segs[o + 2].get<float>();
                s->scale.z = segs[o + 3].get<float>();
                s->scaleMod.x = segs[o + 4].get<float>();
                s->scaleMod.y = segs[o + 5].get<float>();
                s->scaleMod.z = segs[o + 6].get<float>();
                s->rot.x = segs[o + 7].get<int16_t>();
                s->rot.y = segs[o + 8].get<int16_t>();
                s->rot.z = segs[o + 9].get<int16_t>();
            }
        }
        if (x.contains("eft")) {
            rr->effectTimer = x["eft"].get<int16_t>();
        }
        if (x.contains("sct")) {
            rr->scrollTimer = x["sct"].get<int16_t>();
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnRr extras parse error: {}", ex.what());
    }
}

void RegisterEnRrAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = EnRr_SerializeExtras;
    adapter.DeserializeExtras = EnRr_DeserializeExtras;
    EnemySync::RegisterAdapter(ACTOR_EN_RR, adapter);
}
