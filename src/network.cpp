#include "medium/network.hpp"
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
namespace med {
struct Endpoint::Impl {
    bool initialized = false, server{};
    HSteamListenSocket listener{};
    std::vector<uint32_t> connections;
    static void changed(SteamNetConnectionStatusChangedCallback_t *info) {
        auto *self = reinterpret_cast<Impl *>(info->m_info.m_nUserData);
        if (!self)
            return;
        auto *sockets = SteamNetworkingSockets();
        auto conn = info->m_hConn;
        if (info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting && self->server) {
            if (self->connections.size() >= 2) {
                sockets->CloseConnection(conn, 1, "Arena full", false);
                return;
            }
            if (sockets->AcceptConnection(conn) != k_EResultOK) {
                sockets->CloseConnection(conn, 1, "Accept failed", false);
                return;
            }
            self->connections.push_back(conn);
        }
        if (info->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
            info->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
            sockets->CloseConnection(conn, 0, nullptr, false);
            std::erase(self->connections, conn);
        }
    }
    ~Impl() {
        if (initialized) {
            for (auto c : connections)
                SteamNetworkingSockets()->CloseConnection(c, 0, "Shutdown", false);
            if (listener)
                SteamNetworkingSockets()->CloseListenSocket(listener);
            GameNetworkingSockets_Kill();
        }
    }
    void init(bool host, uint16_t port, int lag, float loss, bool chaos) {
        SteamDatagramErrMsg error;
        if (!GameNetworkingSockets_Init(nullptr, error))
            throw std::runtime_error(std::string("GNS init: ") + error);
        initialized = true;
        server = host;
        auto *utils = SteamNetworkingUtils();
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, lag);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, loss);
        if (chaos) {
            utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg, 5);
            utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max, 15);
            utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct, 20);
            utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketReorder_Send, 3);
            utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketReorder_Time, 10);
            utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketDup_Send, 2);
            utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketDup_TimeMax, 5);
        }

        std::array<SteamNetworkingConfigValue_t, 5> options{};
        options[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                          reinterpret_cast<void *>(changed));
        options[1].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData, reinterpret_cast<int64_t>(this));
        // Room for several whole snapshots queued at once, each up to `max_message_bytes`, and a
        // send rate that does not throttle a busy room's snapshot stream on loopback.
        options[2].SetInt32(k_ESteamNetworkingConfig_SendBufferSize, 512 * 1024);
        options[3].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, 3000);
        options[4].SetInt32(k_ESteamNetworkingConfig_SendRateMax, 1024 * 1024);
        SteamNetworkingIPAddr addr;
        addr.Clear();
        addr.SetIPv4(0x7f000001, port);
        if (host) {
            listener = SteamNetworkingSockets()->CreateListenSocketIP(addr, static_cast<int>(options.size()),
                                                                      options.data());
            if (!listener)
                throw std::runtime_error("Cannot bind loopback duel port");
        } else {
            auto c = SteamNetworkingSockets()->ConnectByIPAddress(addr, static_cast<int>(options.size()),
                                                                  options.data());
            if (!c)
                throw std::runtime_error("Cannot connect to loopback duel server");
            connections.push_back(c);
        }
    }
};
Endpoint::Endpoint(bool server, uint16_t port, int lag, float loss, bool chaos)
    : impl_(std::make_unique<Impl>()) {
    if (lag < 0 || lag > 500 || !std::isfinite(loss) || loss < 0 || loss > 50)
        throw std::runtime_error("Invalid transport impairment");
    impl_->init(server, port, lag, loss, chaos);
}
Endpoint::~Endpoint() = default;
std::vector<uint32_t> Endpoint::peers() const {
    std::vector<uint32_t> out;
    for (auto c : impl_->connections) {
        SteamNetConnectionInfo_t info{};
        if (SteamNetworkingSockets()->GetConnectionInfo(c, &info) &&
            info.m_eState == k_ESteamNetworkingConnectionState_Connected)
            out.push_back(c);
    }
    return out;
}
std::vector<Packet> Endpoint::poll() {
    SteamNetworkingSockets()->RunCallbacks();
    std::vector<Packet> out;
    for (auto peer : peers()) {
        std::array<SteamNetworkingMessage_t *, 32> messages{};
        int n = SteamNetworkingSockets()->ReceiveMessagesOnConnection(peer, messages.data(),
                                                                      static_cast<int>(messages.size()));
        if (n < 0)
            continue;
        for (int i = 0; i < n; ++i) {
            auto *msg = messages[static_cast<size_t>(i)];
            if (msg->m_cbSize > 0 && msg->m_cbSize <= static_cast<int>(max_message_bytes)) {
                auto *begin = static_cast<uint8_t *>(msg->m_pData);
                out.push_back({peer, {begin, begin + msg->m_cbSize}});
            } else
                disconnect(peer);
            msg->Release();
        }
    }
    return out;
}
bool Endpoint::send(uint32_t peer, std::span<const uint8_t> data, bool reliable) {
    if (data.empty() || data.size() > max_message_bytes)
        throw std::runtime_error("Network message exceeds protocol limit");
    return SteamNetworkingSockets()->SendMessageToConnection(
               peer, data.data(), static_cast<uint32_t>(data.size()),
               reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable,
               nullptr) == k_EResultOK;
}
void Endpoint::disconnect(uint32_t peer) {
    SteamNetworkingSockets()->CloseConnection(peer, 1, "Protocol violation or disconnect", false);
    std::erase(impl_->connections, peer);
}
int Endpoint::ping(uint32_t peer) const {
    SteamNetConnectionRealTimeStatus_t status{};
    if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer, &status, 0, nullptr) != k_EResultOK)
        return -1;
    return status.m_nPing;
}
} // namespace med
