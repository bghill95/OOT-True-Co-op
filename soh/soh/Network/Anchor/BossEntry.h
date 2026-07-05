#ifndef NETWORK_ANCHOR_BOSS_ENTRY_H
#define NETWORK_ANCHOR_BOSS_ENTRY_H
#ifdef __cplusplus

// Boss-room co-entry (Task 7): when one player crosses a boss door into a boss
// scene, the partner is pulled into the same fight. Registers the OnSceneInit
// detector that broadcasts BOSS_ENTRY. The receiver (Anchor::HandlePacket_BossEntry)
// and the boss table live in Packets/BossEntry.cpp.
//
// Called from Anchor::RegisterHooks with the current connection state (mirrors the
// CoopWarp / PuppetFairy pattern).
void RegisterBossEntryHooks(bool isConnected);

// Per-frame reconcile: ties the echo-latch lifetime to the pull warp (disarms it if the
// warp is superseded/expired). Called by the Anchor per-frame dispatcher in tick order.
void BossEntryTick();

#endif // __cplusplus
#endif // NETWORK_ANCHOR_BOSS_ENTRY_H
