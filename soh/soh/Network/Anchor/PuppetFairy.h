#ifndef NETWORK_ANCHOR_PUPPET_FAIRY_H
#define NETWORK_ANCHOR_PUPPET_FAIRY_H
#ifdef __cplusplus

// Registers/refreshes the companion-fairy hooks (one cosmetic En_Elf per remote
// puppet). Called from Anchor::RegisterHooks with the current connection state;
// hooks unregister and the tracking map is dropped when disconnected.
void RegisterPuppetFairyHooks(bool isConnected);

// Per-frame fairy spawn/reap lifecycle. Driven by the Anchor per-frame dispatcher in
// explicit tick order, not by a self-registered OnGameFrameUpdate hook.
void PuppetFairyTick();

#endif // __cplusplus
#endif // NETWORK_ANCHOR_PUPPET_FAIRY_H
