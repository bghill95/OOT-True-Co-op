#ifndef NETWORK_ANCHOR_TIME_SYNC_H
#define NETWORK_ANCHOR_TIME_SYNC_H
#ifdef __cplusplus

// World-clock sync (dayTime + day counters). See Packets/TimeSync.cpp.
// Driven by the Anchor per-frame dispatcher (HookHandlers.cpp).
void TimeSyncTick();

#endif
#endif
