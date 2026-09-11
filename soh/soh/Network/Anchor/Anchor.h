#ifndef NETWORK_ANCHOR_H
#define NETWORK_ANCHOR_H
#ifdef __cplusplus

#include "soh/Network/Network.h"
#include <libultraship/libultraship.h>
#include <queue>
#include <deque>
#include <atomic>
#include <mutex>

extern "C" {
#include "variables.h"
#include "z64.h"
}

void DummyPlayer_Init(Actor* actor, PlayState* play);
void DummyPlayer_Update(Actor* actor, PlayState* play);
void DummyPlayer_Draw(Actor* actor, PlayState* play);
void DummyPlayer_Destroy(Actor* actor, PlayState* play);

// Life-state carried by PLAYER_LIFE_STATE (edge-triggered) and cached per client.
// Default 0 (ALIVE) so a value-initialized/legacy-peer client reads as alive.
typedef enum {
    LIFE_STATE_ALIVE = 0,
    LIFE_STATE_REVIVING = 1,
    LIFE_STATE_DOWNED = 2,
    LIFE_STATE_GAME_OVER = 3,
} LifeState;

typedef struct {
    uint32_t clientId;
    std::string name;
    Color_RGB8 color;
    std::string clientVersion;
    std::string teamId;
    bool online;
    bool self;
    uint32_t seed;
    bool isSaveLoaded;
    bool isGameComplete;
    s16 sceneNum;
    s32 entranceIndex;
    u8 lifeState; // LifeState; default 0 (ALIVE)

    // Only available in PLAYER_UPDATE packets
    s32 linkAge;
    PosRot posRot;
    Vec3s jointTable[24];
    Vec3s upperLimbRot;
    s8 currentBoots;
    s8 currentShield;
    s8 currentTunic;
    u32 stateFlags1;
    u32 stateFlags2;
    u8 buttonItem0;
    s8 itemAction;
    s8 heldItemAction;
    u8 modelGroup;
    s8 invincibilityTimer;
    s16 unk_862;
    s16 dekuStickFlame; // player->unk_860: burning deku stick timer
    f32 dekuStickLength; // player->unk_85C: remaining stick fraction (draw scale + tip reach)
    f32 bowString; // player->unk_858: bow string draw amount (stale on puppets = bow stuck drawn)
    f32 shapeYOffset; // player->actor.shape.yOffset: the render offset a ledge climb uses. world.pos
                      // jumps to the top of the ledge in one frame and this pulls the model back down
                      // for the pull-up; without it the puppet teleported onto the ledge and then
                      // played the climb in mid-air. Most visible getting out of water.
    u8 currentMask;  // player->currentMask: worn mask (PlayerMask). Never streamed before -> puppets showed no mask.
    s8 meleeWeaponState; // player->meleeWeaponState: swing trail live (the puppet owns a blure from Player_Init)
    bool bgsBroken;      // owner's gSaveContext.swordHealth <= 0: snapped Giant's Knife (model reads OUR save otherwise)
    s16 giId;            // get-item presentation: getItemEntry.getItemId while unk_862 != 0, else -1
    s8 giMod;            // ... and its modIndex (MOD_NONE / MOD_RANDOMIZER)
    s8 actionVar1;
    bool boomActive; // thrower's EN_BOOM is in flight (boomPos/boomYaw valid)
    Vec3f boomPos;
    bool carryActive;  // this client is carrying an actor (carryPos/carryRot are its pose)
    Vec3f carryPos;
    Vec3s carryRot;
    s16 boomYaw;
    s16 boomPitch;
    bool horseActive; // this client sits on an EN_HORSE (the fields below are its pose and gait)
    Vec3f horsePos;
    Vec3s horseRot;
    s8 horseType;     // HORSE_EPONA / HORSE_HNI
    s8 horseAnim;     // EnHorse animationIdx
    f32 horseFrame;   // skelAnime.curFrame
    f32 horseSpeed;   // skelAnime.playSpeed

    // Ptr to the dummy player
    Player* player;
} AnchorClient;

// A client counts as "alive" for gameplay/authority purposes while ALIVE or
// mid-fairy-revive (REVIVING). Task 8 wires this into ComputeAuthorityClientId.
inline bool IsClientAlive(const AnchorClient& client) {
    return client.lifeState == LIFE_STATE_ALIVE || client.lifeState == LIFE_STATE_REVIVING;
}

typedef struct {
    uint32_t ownerClientId;
    u8 pvpMode;           // 0 = off, 1 = on, 2 = on with friendly fire
    u8 showLocationsMode; // 0 = none, 1 = team, 2 = all
    u8 teleportMode;      // 0 = off, 1 = team, 2 = all
    u8 syncItemsAndFlags; // 0 = off, 1 = on
} RoomState;

class Anchor : public Network {
  private:
    bool refreshingActors = false;
    bool justLoadedSave = false;
    bool isHandlingUpdateTeamState = false;
    bool isProcessingIncomingPacket = false;
    // Frame (gPlayState->gameplayFrames) of our last locally-originated RUPEE_CHANGE
    // send; the receive path only drift-snaps to a peer's absolute total after a
    // ~40-frame quiet window (see Packets/RupeeChange.cpp).
    uint32_t lastLocalRupeeSendFrame = 0;
    // Same role as the rupee one, for the small-key drift snap. One counter is
    // enough: only the dungeon we are standing in can change.
    uint32_t lastLocalDungeonKeySendFrame = 0;
    // Raised by OnConnected / OnDisconnected on the websocket thread, consumed by
    // TickConnection on the game thread. See the comment above OnConnected.
    std::atomic<bool> pendingConnected{ false };
    std::atomic<bool> pendingDisconnected{ false };
    // A deque, not a queue: OnIncomingJson replaces a client's queued pose in place
    // with its newer one, which needs to see the elements.
    std::deque<nlohmann::json> incomingPacketQueue;
    std::mutex incomingPacketQueueMutex;
    // Drops whatever is still queued. Anything held belongs to the session that just
    // ended, and replaying it into the next one applies a dead session's state.
    void DropQueuedPackets();

    // How a GAME-THREAD caller adds to the queue. Handlers run with the drain no
    // longer holding the mutex, so a handler pushing packets of its own
    // (UpdateTeamState replays the server's catch-up queue) races the receive thread
    // unless it takes the lock here. Such packets are picked up by the next frame's
    // drain, not the current batch.
    // NOT for OnIncomingJson: it already holds incomingPacketQueueMutex and pushes
    // directly. Calling this from there would lock a non-recursive mutex twice and
    // hang the receive thread.
    void EnqueuePacket(nlohmann::json payload);

    nlohmann::json PrepClientState();
    nlohmann::json PrepRoomState();
    void RegisterHooks();
    void RefreshClientActors();
    void HandlePacket_AllClientState(nlohmann::json payload);
    void HandlePacket_BgmPos(nlohmann::json payload);
    void HandlePacket_BgmPosRequest(nlohmann::json payload);
    void HandlePacket_BgmRestart(nlohmann::json payload);
    void HandlePacket_BgmState(nlohmann::json payload);
    void HandlePacket_BossEntry(nlohmann::json payload);
    void HandlePacket_ConsumeAdultTradeItem(nlohmann::json payload);
    void HandlePacket_CutsceneSync(nlohmann::json payload);
    void HandlePacket_CapturePlayer(nlohmann::json payload);
    void HandlePacket_DamagePlayer(nlohmann::json payload);
    void HandlePacket_DisableAnchor(nlohmann::json payload);
    void HandlePacket_EnemyDespawn(nlohmann::json payload);
    void HandlePacket_BombfEvent(nlohmann::json payload);
    void HandlePacket_BombEvent(nlohmann::json payload);
    void HandlePacket_BreakEvent(nlohmann::json payload);
    void HandlePacket_CarryEvent(nlohmann::json payload);
    void HandlePacket_DynaState(nlohmann::json payload);
    void HandlePacket_EnemyDied(nlohmann::json payload);
    void HandlePacket_EnemyHit(nlohmann::json payload);
    void HandlePacket_EnemyHitRequest(nlohmann::json payload);
    void HandlePacket_EnemyRoster(nlohmann::json payload);
    void HandlePacket_EnemySpawn(nlohmann::json payload);
    void HandlePacket_EnemyState(nlohmann::json payload);
    void HandlePacket_EntranceDiscovered(nlohmann::json payload);
    void HandlePacket_FxEvent(nlohmann::json payload);
    void HandlePacket_GameComplete(nlohmann::json payload);
    void HandlePacket_GiveItem(nlohmann::json payload);
    void HandlePacket_PlayerLifeState(nlohmann::json payload);
    void HandlePacket_PlayerSfx(nlohmann::json payload);
    void HandlePacket_PlayerUpdate(nlohmann::json payload);
    void HandlePacket_RequestTeamState(nlohmann::json payload);
    void HandlePacket_RequestTeleport(nlohmann::json payload);
    void HandlePacket_RupeeChange(nlohmann::json payload);
    void HandlePacket_ServerMessage(nlohmann::json payload);
    void HandlePacket_SetCheckStatus(nlohmann::json payload);
    void HandlePacket_SetFlag(nlohmann::json payload);
    void HandlePacket_TeleportTo(nlohmann::json payload);
    void HandlePacket_TimeSync(nlohmann::json payload);
    void HandlePacket_UnsetFlag(nlohmann::json payload);
    void HandlePacket_UpdateBeansCount(nlohmann::json payload);
    void HandlePacket_UpdateClientState(nlohmann::json payload);
    void HandlePacket_UpdateDungeonItems(nlohmann::json payload);
    void HandlePacket_UpdateRoomState(nlohmann::json payload);
    void HandlePacket_UpdateTeamState(nlohmann::json payload);

  public:
    uint32_t ownClientId;
    u8 myLifeState = 0; // our own cached LifeState (LIFE_STATE_ALIVE); broadcast via PLAYER_LIFE_STATE
    inline static const std::string clientVersion = (char*)gBuildVersion;

    // Packet types //
    inline static const std::string ALL_CLIENT_STATE = "ALL_CLIENT_STATE";
    inline static const std::string BGM_POS = "BGM_POS";
    inline static const std::string BGM_POS_REQUEST = "BGM_POS_REQUEST";
    inline static const std::string BGM_RESTART = "BGM_RESTART";
    inline static const std::string BGM_STATE = "BGM_STATE";
    inline static const std::string BOMBF_EVENT = "BOMBF_EVENT";
    inline static const std::string BOMB_EVENT = "BOMB_EVENT";
    inline static const std::string BREAK_EVENT = "BREAK_EVENT";
    inline static const std::string BOSS_ENTRY = "BOSS_ENTRY";
    inline static const std::string CAPTURE_PLAYER = "CAPTURE_PLAYER";
    inline static const std::string CARRY_EVENT = "CARRY_EVENT";
    inline static const std::string CUTSCENE_SYNC = "CUTSCENE_SYNC";
    inline static const std::string DAMAGE_PLAYER = "DAMAGE_PLAYER";
    inline static const std::string DISABLE_ANCHOR = "DISABLE_ANCHOR";
    inline static const std::string DYNA_STATE = "DYNA_STATE";
    inline static const std::string ENEMY_DESPAWN = "ENEMY_DESPAWN";
    inline static const std::string ENEMY_DIED = "ENEMY_DIED";
    inline static const std::string ENEMY_HIT = "ENEMY_HIT";
    inline static const std::string ENEMY_HIT_REQUEST = "ENEMY_HIT_REQUEST";
    inline static const std::string ENEMY_ROSTER = "ENEMY_ROSTER";
    inline static const std::string ENEMY_SPAWN = "ENEMY_SPAWN";
    inline static const std::string ENEMY_STATE = "ENEMY_STATE";
    inline static const std::string ENTRANCE_DISCOVERED = "ENTRANCE_DISCOVERED";
    inline static const std::string FX_EVENT = "FX_EVENT";
    inline static const std::string GAME_COMPLETE = "GAME_COMPLETE";
    inline static const std::string GIVE_ITEM = "GIVE_ITEM";
    inline static const std::string HANDSHAKE = "HANDSHAKE";
    inline static const std::string PLAYER_LIFE_STATE = "PLAYER_LIFE_STATE";
    inline static const std::string PLAYER_SFX = "PLAYER_SFX";
    inline static const std::string PLAYER_UPDATE = "PLAYER_UPDATE";
    inline static const std::string REQUEST_TEAM_STATE = "REQUEST_TEAM_STATE";
    inline static const std::string REQUEST_TELEPORT = "REQUEST_TELEPORT";
    inline static const std::string RUPEE_CHANGE = "RUPEE_CHANGE";
    inline static const std::string SERVER_MESSAGE = "SERVER_MESSAGE";
    inline static const std::string SET_CHECK_STATUS = "SET_CHECK_STATUS";
    inline static const std::string SET_FLAG = "SET_FLAG";
    inline static const std::string TELEPORT_TO = "TELEPORT_TO";
    inline static const std::string TIME_SYNC = "TIME_SYNC";
    inline static const std::string UNSET_FLAG = "UNSET_FLAG";
    inline static const std::string UPDATE_BEANS_COUNT = "UPDATE_BEANS_COUNT";
    inline static const std::string UPDATE_CLIENT_STATE = "UPDATE_CLIENT_STATE";
    inline static const std::string UPDATE_DUNGEON_ITEMS = "UPDATE_DUNGEON_ITEMS";
    inline static const std::string UPDATE_ROOM_STATE = "UPDATE_ROOM_STATE";
    inline static const std::string UPDATE_TEAM_STATE = "UPDATE_TEAM_STATE";

    static Anchor* Instance;
    std::map<uint32_t, AnchorClient> clients;
    std::vector<uint32_t> actorIndexToClientId;
    RoomState roomState;

    void Enable();
    void Disable();
    void OnIncomingJson(nlohmann::json payload);
    void OnConnected();
    void OnDisconnected();
    void DrawMenu();
    void ProcessIncomingPacketQueue();
    // Game-thread half of OnConnected / OnDisconnected.
    void TickConnection();
    void SendJsonToRemote(nlohmann::json packet);
    bool IsSaveLoaded();
    bool CanTeleportTo(uint32_t clientId);

    void SendPacket_BgmPosRequest();
    void SendPacket_BgmPos(uint32_t targetClientId, u16 seqId, u32 scriptCounter, s16 sceneNum);
    void SendPacket_BgmRestart(u16 seqId);
    void SendPacket_BgmState();
    void SendPacket_BossEntry(s16 sceneNum, s32 entranceIndex);
    void SendPacket_BombfEvent(nlohmann::json& event);
    void SendPacket_BombEvent(nlohmann::json& event);
    void SendPacket_BreakEvent(nlohmann::json& event);
    void SendPacket_FxEvent(nlohmann::json& event);
    void SendPacket_CarryEvent(nlohmann::json& event);
    void SendPacket_ClearTeamState(std::string teamId);
    void SendPacket_CutsceneSync(u8 kind, s16 sceneNum, s32 entranceIndex, s32 cutsceneIndex, s16 csFlag);
    // An enemy simulated on THIS client grabbed another player: tell them to be
    // taken. mode 0 = respawn at the last entrance, 1 = warp to `entrance`.
    void SendPacket_CapturePlayer(u32 clientId, u8 mode, s16 entrance);
    void SendPacket_DamagePlayer(u32 clientId, u8 damageEffect, u8 damage);
    void SendPacket_EnemyDespawn(uint64_t enemyKey);
    void SendPacket_EnemyDied(Actor* actor, uint64_t enemyKey, bool permanent, u32 dmgFlags);
    void SendPacket_EnemyHit(Actor* actor, uint64_t enemyKey, u8 damage, u32 dmgFlags, Vec3s hitPos, u8 health);
    void SendPacket_EnemyHitRequest(Actor* actor, uint64_t enemyKey, u8 damage, u32 dmgFlags, Vec3s hitPos,
                                    int16_t attackerActorId);
    void SendPacket_EnemyRosterRequest(int16_t roomNum);
    void SendPacket_EnemySpawn(uint64_t enemyKey, int16_t actorId, uint16_t params, Vec3f pos, Vec3s rot,
                               int16_t roomNum, uint64_t parentKey);
    void SendPacket_DynaState(nlohmann::json& dynas);
    void SendPacket_EnemyState(nlohmann::json& enemies);
    void SendPacket_EntranceDiscovered(u16 entranceIndex);
    void SendPacket_GameComplete();
    void SendPacket_GiveItem(u16 modId, s16 getItemId);
    void SendPacket_Handshake();
    void SendPacket_PlayerLifeState(u8 state);
    void SendPacket_PlayerSfx(u16 sfxId);
    void SendPacket_PlayerUpdate();
    void SendPacket_RequestTeamState();
    void SendPacket_RequestTeleport(u32 clientId);
    void SendPacket_RupeeChange(s16 delta);
    void SendPacket_SetCheckStatus(RandomizerCheck rc);
    void SendPacket_SetFlag(s16 sceneNum, s16 flagType, s16 flag);
    void SendPacket_TeleportTo(u32 clientId);
    void SendPacket_TimeSync(bool force);
    void SendPacket_UnsetFlag(s16 sceneNum, s16 flagType, s16 flag);
    void SendPacket_UpdateBeansCount();
    void SendPacket_UpdateClientState();
    // keyDelta: the small-key change this event carries (+1 picked up, -1 spent,
    // 0 for a boss key / compass / map, which are bits and travel absolute).
    void SendPacket_UpdateDungeonItems(s8 keyDelta = 0);
    void SendPacket_UpdateRoomState();
    void SendPacket_UpdateTeamState();
};

typedef enum {
    // Starting at 5 to continue from the last value in the PlayerDamageResponseType enum
    DUMMY_PLAYER_HIT_RESPONSE_STUN = 5,
    DUMMY_PLAYER_HIT_RESPONSE_FIRE,
    DUMMY_PLAYER_HIT_RESPONSE_NORMAL,
} DummyPlayerDamageResponseType;

class AnchorRoomWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override{};
    void DrawElement() override;
    void Draw() override;
    void UpdateElement() override{};
};

#endif // __cplusplus
#endif // NETWORK_ANCHOR_H
