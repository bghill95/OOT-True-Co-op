#include "Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/OTRGlobals.h"
#include "soh/Enhancements/nametag.h"

extern "C" {
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

// MARK: - Overrides

void Anchor::Enable() {
    // A PERMANENT hook, installed before the socket can ever connect: it is what carries
    // OnConnected / OnDisconnected over to the game thread (see TickConnection). It
    // cannot live in RegisterHooks, whose own hooks are conditioned on isConnected --
    // they do not exist yet at the moment a connection has to be reacted to.
    //
    // OnGameStateMainStart, NOT OnGameFrameUpdate, and that is load-bearing:
    // TickConnection calls RegisterHooks, which registers an OnGameFrameUpdate hook,
    // and RegisterGameHook inserts into the hook map immediately. Running from an
    // OnGameFrameUpdate hook would mean inserting into the unordered_map that
    // ExecuteHooks is iterating right then -- a rehash invalidates the loop's own
    // reference and iterator. Anchor registers nothing on OnGameStateMainStart.
    static bool sConnectionTickInstalled = false;
    if (!sConnectionTickInstalled) {
        sConnectionTickInstalled = true;
        GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateMainStart>(
            [this]() { TickConnection(); });
    }

    Network::Enable(CVarGetString(CVAR_REMOTE_ANCHOR("Host"), "anchor.hm64.org"),
                    CVarGetInteger(CVAR_REMOTE_ANCHOR("Port"), 43383));
    ownClientId = CVarGetInteger(CVAR_REMOTE_ANCHOR("LastClientId"), 0);
    roomState.ownerClientId = 0;
}

void Anchor::Disable() {
    Network::Disable();

    DropQueuedPackets();

    clients.clear();
    RefreshClientActors();
}

// Both of these are called from ReceiveFromServer -- the WEBSOCKET thread. What they
// used to do there does not belong to it: RegisterHooks rewrites GameInteractor's
// static hook maps (an unordered_map of std::function, rehashed in place) while the
// game thread is iterating those same maps in ExecuteHooks, and SendPacket_Handshake
// / IsSaveLoaded dereference gPlayState and gSaveContext, which the game thread
// reassigns across a scene transition. Same class of bug as the one that put a freed
// actor's arena under the graphics thread. Only a flag is raised here; TickConnection
// does the work on the frame hook.
void Anchor::OnConnected() {
    pendingConnected = true;
}

void Anchor::EnqueuePacket(nlohmann::json payload) {
    std::lock_guard<std::mutex> lock(incomingPacketQueueMutex);
    incomingPacketQueue.push_back(std::move(payload));
}

void Anchor::DropQueuedPackets() {
    std::lock_guard<std::mutex> lock(incomingPacketQueueMutex);
    incomingPacketQueue.clear();
}

void Anchor::TickConnection() {
    if (pendingDisconnected.exchange(false)) {
        // An ordinary network drop never goes through Disable() -- ReceiveFromServer
        // loops and reconnects by itself -- and the drain is conditioned on
        // isConnected, so it stops while the socket is down. Without this, whatever
        // was queued when the link died is replayed into the new session.
        DropQueuedPackets();
        RegisterHooks();
    }
    if (pendingConnected.exchange(false)) {
        SendPacket_Handshake();
        RegisterHooks();

        if (IsSaveLoaded()) {
            SendPacket_RequestTeamState();
        }
    }
}

void Anchor::OnDisconnected() {
    pendingDisconnected = true;
}

void Anchor::SendJsonToRemote(nlohmann::json payload) {
    if (!isConnected) {
        return;
    }

    payload["clientId"] = ownClientId;
    if (!payload.contains("quiet")) {
        SPDLOG_DEBUG("[Anchor] Sending payload:\n{}", payload.dump());
    }
    Network::SendJsonToRemote(payload);
}

void Anchor::OnIncomingJson(nlohmann::json payload) {
    // If it doesn't contain a type, it's not a valid payload
    if (!payload.contains("type")) {
        return;
    }

    // If it's not a quiet payload, log it
    if (!payload.contains("quiet")) {
        SPDLOG_DEBUG("[Anchor] Received payload:\n{}", payload.dump());
    }

    // The version filter that used to sit here read `clients` -- on the websocket
    // thread, while the game thread inserts into and erases from that same map. It
    // moved down into the drain (see ProcessIncomingPacketQueue) so that this
    // function touches nothing but the queue.

    // PLAYER_UPDATE used to be handled right here, on the WEBSOCKET thread, with the
    // note "no need to queue". There was: HandlePacket_PlayerUpdate writes clients[id]
    // (inserting a new entry when a peer appears), calls FxSync::IngestPlayerProjectiles
    // -- which writes sProj's per-client arrow vectors -- and can call
    // RefreshClientActors(), which spawns, kills and destroys puppet actors. Every one of
    // those is read or walked by the GAME thread in the same instant: DummyPlayer_Update
    // ticks the replicas, Anchor_GetPuppetActors iterates clients, Actor_UpdateAll walks
    // the actor list. Two crashes came out of that race -- W: died in
    // TickPuppetProjectiles reading an arrowReplicas whose begin pointer was 0 (the
    // vector was being reallocated under it), and G: died on an En_Ishi whose `update`
    // field alone had been overwritten.
    // A pose packet is absolute state, last-writer-wins, so deferring it to the drain at
    // the top of the next frame costs at most one frame of staleness and nothing else.
    // Queue all packets to be processed on the game thread
    std::lock_guard<std::mutex> lock(incomingPacketQueueMutex);

    // Poses coalesce: a client's queued pose is REPLACED IN PLACE by its newer one
    // rather than queued behind it. Without that, a scene load -- during which the
    // game thread stops draining -- builds an unbounded backlog and then replays it
    // in a single frame, which means N kill+respawn of every puppet at once. Replacing
    // in place rather than holding poses aside keeps them in TCP order with the rest,
    // so a stale pose can never be applied after the scene change that followed it.
    // is_string / is_number_unsigned rather than get<>, because nothing here is inside
    // a try and this is the websocket thread: an exception escaping ends the process.
    if (payload["type"].is_string() && payload["type"].get<std::string>() == PLAYER_UPDATE &&
        payload.contains("clientId") && payload["clientId"].is_number_unsigned()) {
        uint32_t poseClientId = payload["clientId"].get<uint32_t>();
        for (nlohmann::json& queued : incomingPacketQueue) {
            // find(), not operator[]: queued is non-const, so a subscript would INSERT
            // a null "clientId" into a packet that has none and defeat the
            // contains("clientId") guards its handler relies on.
            const nlohmann::json& q = queued;
            auto queuedType = q.find("type");
            auto queuedId = q.find("clientId");
            if (queuedType != q.end() && *queuedType == PLAYER_UPDATE && queuedId != q.end() &&
                *queuedId == poseClientId) {
                queued = std::move(payload);
                return;
            }
        }
    }
    incomingPacketQueue.push_back(std::move(payload));
}

void Anchor::ProcessIncomingPacketQueue() {
    // Take the queue and release the lock BEFORE handling anything. Holding it for
    // the whole drain deadlocks: HandlePacket_DisableAnchor -> Disable() ->
    // Network::Disable() joins the receive thread, which may itself be blocked on
    // this very mutex trying to enqueue. It also meant every blocking send a handler
    // makes froze the receive thread along with it.
    std::deque<nlohmann::json> batch;
    {
        std::lock_guard<std::mutex> lock(incomingPacketQueueMutex);
        batch.swap(incomingPacketQueue);
    }

    while (!batch.empty()) {
        // A handler can disable Anchor mid-batch (HandlePacket_DisableAnchor). What it
        // clears is the queue, not this batch, which was already swapped out of it --
        // so stop here rather than keep applying a dead session's items and warps.
        if (!isEnabled) {
            break;
        }
        nlohmann::json payload = batch.front();
        batch.pop_front();

        std::string packetType;
        try {
            packetType = payload["type"].get<std::string>();
        } catch (const std::exception& ex) {
            SPDLOG_WARN("[Anchor] dropping packet with no usable type: {}", ex.what());
            continue;
        }

        // Ignore packets from mismatched clients, except for ALL_CLIENT_STATE or
        // UPDATE_CLIENT_STATE. This ran on the websocket thread until it was found
        // reading `clients` while this thread was rehashing it.
        // contains() first: payload is non-const here, so a bare operator[] would
        // INSERT "clientId": null and defeat the contains("clientId") guards the
        // handlers below rely on. And is_number_unsigned rather than a bare get<>:
        // this runs outside the try below, so a malformed clientId must not throw
        // its way out of the game frame.
        if (packetType != ALL_CLIENT_STATE && packetType != UPDATE_CLIENT_STATE &&
            payload.contains("clientId") && payload["clientId"].is_number_unsigned()) {
            uint32_t clientId = payload["clientId"].get<uint32_t>();
            auto it = clients.find(clientId);
            if (it != clients.end() && it->second.clientVersion != clientVersion) {
                continue;
            }
        }

        isProcessingIncomingPacket = true;

        // Every handler below reads its payload with json::get<>, which throws on a
        // missing or mistyped field — a truncated packet, or a peer on a different
        // build. Unhandled, that exception unwinds out of the game frame and takes
        // the process with it, and leaves isProcessingIncomingPacket latched true,
        // which silently disables every local broadcast for the rest of the session.
        // One bad packet must cost one packet, nothing more.
        try {
        // packetType here is a string so we can't use a switch statement
        if (packetType == ALL_CLIENT_STATE)
            HandlePacket_AllClientState(payload);
        else if (packetType == BGM_POS)
            HandlePacket_BgmPos(payload);
        else if (packetType == BGM_POS_REQUEST)
            HandlePacket_BgmPosRequest(payload);
        else if (packetType == BGM_RESTART)
            HandlePacket_BgmRestart(payload);
        else if (packetType == BGM_STATE)
            HandlePacket_BgmState(payload);
        else if (packetType == BOMBF_EVENT)
            HandlePacket_BombfEvent(payload);
        else if (packetType == BOMB_EVENT)
            HandlePacket_BombEvent(payload);
        else if (packetType == BREAK_EVENT)
            HandlePacket_BreakEvent(payload);
        else if (packetType == BOSS_ENTRY)
            HandlePacket_BossEntry(payload);
        else if (packetType == CARRY_EVENT)
            HandlePacket_CarryEvent(payload);
        else if (packetType == CUTSCENE_SYNC)
            HandlePacket_CutsceneSync(payload);
        else if (packetType == CAPTURE_PLAYER)
            HandlePacket_CapturePlayer(payload);
        else if (packetType == DAMAGE_PLAYER)
            HandlePacket_DamagePlayer(payload);
        else if (packetType == DISABLE_ANCHOR)
            HandlePacket_DisableAnchor(payload);
        else if (packetType == DYNA_STATE)
            HandlePacket_DynaState(payload);
        else if (packetType == ENEMY_DESPAWN)
            HandlePacket_EnemyDespawn(payload);
        else if (packetType == ENEMY_DIED)
            HandlePacket_EnemyDied(payload);
        else if (packetType == ENEMY_HIT)
            HandlePacket_EnemyHit(payload);
        else if (packetType == ENEMY_HIT_REQUEST)
            HandlePacket_EnemyHitRequest(payload);
        else if (packetType == ENEMY_ROSTER)
            HandlePacket_EnemyRoster(payload);
        else if (packetType == ENEMY_SPAWN)
            HandlePacket_EnemySpawn(payload);
        else if (packetType == ENEMY_STATE)
            HandlePacket_EnemyState(payload);
        else if (packetType == ENTRANCE_DISCOVERED)
            HandlePacket_EntranceDiscovered(payload);
        else if (packetType == FX_EVENT)
            HandlePacket_FxEvent(payload);
        else if (packetType == GAME_COMPLETE)
            HandlePacket_GameComplete(payload);
        else if (packetType == GIVE_ITEM)
            HandlePacket_GiveItem(payload);
        else if (packetType == PLAYER_LIFE_STATE)
            HandlePacket_PlayerLifeState(payload);
        else if (packetType == PLAYER_SFX)
            HandlePacket_PlayerSfx(payload);
        else if (packetType == PLAYER_UPDATE)
            HandlePacket_PlayerUpdate(payload);
        else if (packetType == UPDATE_TEAM_STATE)
            HandlePacket_UpdateTeamState(payload);
        else if (packetType == REQUEST_TEAM_STATE)
            HandlePacket_RequestTeamState(payload);
        else if (packetType == REQUEST_TELEPORT)
            HandlePacket_RequestTeleport(payload);
        else if (packetType == RUPEE_CHANGE)
            HandlePacket_RupeeChange(payload);
        else if (packetType == SERVER_MESSAGE)
            HandlePacket_ServerMessage(payload);
        else if (packetType == SET_CHECK_STATUS)
            HandlePacket_SetCheckStatus(payload);
        else if (packetType == SET_FLAG)
            HandlePacket_SetFlag(payload);
        else if (packetType == TELEPORT_TO)
            HandlePacket_TeleportTo(payload);
        else if (packetType == TIME_SYNC)
            HandlePacket_TimeSync(payload);
        else if (packetType == UNSET_FLAG)
            HandlePacket_UnsetFlag(payload);
        else if (packetType == UPDATE_BEANS_COUNT)
            HandlePacket_UpdateBeansCount(payload);
        else if (packetType == UPDATE_CLIENT_STATE)
            HandlePacket_UpdateClientState(payload);
        else if (packetType == UPDATE_ROOM_STATE)
            HandlePacket_UpdateRoomState(payload);
        else if (packetType == UPDATE_DUNGEON_ITEMS)
            HandlePacket_UpdateDungeonItems(payload);
        } catch (const std::exception& ex) {
            SPDLOG_WARN("[Anchor] handler for '{}' threw: {}", packetType, ex.what());
        }

        isProcessingIncomingPacket = false;
    }
}

// MARK: - Misc/Helpers

// Kills all existing anchor actors and respawns them with the new client data
void Anchor::RefreshClientActors() {
    if (!IsSaveLoaded()) {
        return;
    }

    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;

    while (actor != NULL) {
        if (actor->id == ACTOR_EN_OE2 && actor->update == DummyPlayer_Update) {
            NameTag_RemoveAllForActor(actor);
            Actor_Kill(actor);
        }
        actor = actor->next;
    }

    actorIndexToClientId.clear();

    // Scoped, not a bare flag. ShouldActorInit converts every ACTOR_PLAYER spawned
    // while this is set into a puppet -- so if anything threw out of the loop below,
    // the flag latched true and the next REAL Link was converted too. ACTORCAT_PLAYER
    // then had no head, and Actor_InitContext dereferenced it on the following scene
    // load (z_actor.c:2566 -> func_8002C0C0). Same shape as the isHandlingUpdateTeamState
    // latch fixed earlier; this was the second one.
    struct RefreshGuard {
        bool& flag;
        explicit RefreshGuard(bool& f) : flag(f) {
            flag = true;
        }
        ~RefreshGuard() {
            flag = false;
        }
    } refreshGuard(refreshingActors);

    for (auto& [clientId, client] : clients) {
        if (!client.online || client.self) {
            continue;
        }

        actorIndexToClientId.push_back(clientId);
        // We are using a hook `ShouldActorInit` to override the init/update/draw/destroy functions of the Player we
        // spawn We quickly store a mapping of "index" to clientId, then within the init function we use this to get the
        // clientId and store it on player->zTargetActiveTimer (unused s32 for the dummy) for convenience
        auto dummy = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_PLAYER, client.posRot.pos.x,
                                 client.posRot.pos.y, client.posRot.pos.z, client.posRot.rot.x, client.posRot.rot.y,
                                 client.posRot.rot.z, actorIndexToClientId.size() - 1, false);
        client.player = (Player*)dummy;
    }
}

bool Anchor::IsSaveLoaded() {
    if (gPlayState == nullptr) {
        return false;
    }

    if (GET_PLAYER(gPlayState) == nullptr) {
        return false;
    }

    if (gSaveContext.fileNum < 0 || gSaveContext.fileNum > 2) {
        return false;
    }

    if (gSaveContext.gameMode != GAMEMODE_NORMAL) {
        return false;
    }

    return true;
}
