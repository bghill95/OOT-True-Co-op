#ifndef NETWORK_ANCHOR_PUPPET_FAIRY_H
#define NETWORK_ANCHOR_PUPPET_FAIRY_H
#ifdef __cplusplus

// Registers/refreshes the companion-fairy hooks (one cosmetic En_Elf per remote
// puppet). Called from Anchor::RegisterHooks with the current connection state;
// hooks unregister and the tracking map is dropped when disconnected.
void RegisterPuppetFairyHooks(bool isConnected);

#endif // __cplusplus
#endif // NETWORK_ANCHOR_PUPPET_FAIRY_H
