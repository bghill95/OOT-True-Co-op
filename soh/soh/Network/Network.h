#ifndef NETWORK_H
#define NETWORK_H
#ifdef __cplusplus

#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>

// Anything logged from the RECEIVE THREAD must go through this. See the comment at
// the top of Network.cpp: the SPDLOG_* macros read the default logger through an
// unsynchronised .get(), and logger::should_log() dereferences that pointer before
// it checks whether the level is even enabled -- so a log line that is filtered out
// still crashes. Fetch the pointer once, check it, drop the line if it is gone.
#define NET_LOG(MACRO, ...)                             \
    do {                                                \
        auto* netLogger_ = spdlog::default_logger_raw();\
        if (netLogger_ != nullptr) {                    \
            MACRO(netLogger_, __VA_ARGS__);             \
        }                                               \
    } while (0)
#ifdef ENABLE_REMOTE_CONTROL
#include <SDL2/SDL_net.h>
#endif
#include <nlohmann/json.hpp>

class Network {
  private:
#ifdef ENABLE_REMOTE_CONTROL
    IPaddress networkAddress;
    TCPsocket networkSocket;
#endif
    std::thread receiveThread;
    std::string receivedData;

    void ReceiveFromServer();
    void HandleRemoteData(char payload[512]);
    void HandleRemoteJson(std::string payload);

  public:
    // Written by the receive thread (ReceiveFromServer) and read by the game thread
    // on every frame that registers hooks or sends a packet: plain bools here were a
    // data race with no happens-before edge at all.
    std::atomic<bool> isEnabled;
    std::atomic<bool> isConnected;

    void Enable(const char* host, uint16_t port);
    void Disable();
    /**
     * Raw data handler
     *
     * If you are developing a new remote, you should probably use the json methods instead. This
     * method requires you to parse the data and ensure packets are complete manually, we cannot
     * gaurentee that the data will be complete, or that it will only contain one packet with this
     */
    virtual void OnIncomingData(char payload[512]);
    /**
     * Json handler
     *
     * This method will be called when a complete json packet is received. All json packets must
     * be delimited by a null terminator (\0).
     */
    virtual void OnIncomingJson(nlohmann::json payload);
    virtual void OnConnected();
    virtual void OnDisconnected();
    void SendDataToRemote(const char* payload);
    virtual void SendJsonToRemote(nlohmann::json packet);
};

#endif // __cplusplus
#endif // NETWORK_H
