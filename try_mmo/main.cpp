#include "Log.h"
#include "game/Balance.h"
#include "game/GameServer.h"
#include "net/LagTransport.h"
#include "net/TcpTransport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>
#include <timeapi.h>

namespace
{
constexpr std::uint16_t kDefaultPort = 7777;

std::atomic<bool> g_stop{false};

BOOL WINAPI onConsoleSignal(DWORD)
{
    g_stop.store(true, std::memory_order_relaxed);
    return TRUE;
}

// 대기 시간이 기본 15.6ms 단위로 뭉개지지 않게 타이머 해상도를 1ms로 올린다.
class TimerResolution
{
public:
    TimerResolution() { timeBeginPeriod(1); }
    ~TimerResolution() { timeEndPeriod(1); }
};

struct Options
{
    std::uint16_t port = kDefaultPort;
    int lagRttMs = 0;
    trymmo::ZoneOptions zone{trymmo::balance::kPlayerMaxHp, trymmo::balance::kPlayerAttack,
                             trymmo::balance::kSlimeKillExp, false, trymmo::balance::kSlimeCount};
};

Options parseOptions(int argc, char* argv[])
{
    Options options;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string_view name = argv[i];
        if (name == "--port")
        {
            options.port = static_cast<std::uint16_t>(std::stoi(argv[i + 1]));
        }
        else if (name == "--lag-rtt-ms")
        {
            options.lagRttMs = std::stoi(argv[i + 1]);
        }
        else if (name == "--debug-player-hp")
        {
            options.zone.playerMaxHp = std::stoi(argv[i + 1]);
        }
        else if (name == "--debug-player-attack")
        {
            options.zone.playerAttack = std::stoi(argv[i + 1]);
        }
        else if (name == "--debug-kill-exp")
        {
            options.zone.slimeKillExp = std::stoi(argv[i + 1]);
        }
        else if (name == "--debug-slime-count")
        {
            options.zone.slimeCount = std::stoi(argv[i + 1]);
        }
        else if (name == "--debug-slime-cluster")
        {
            options.zone.clusterSlimes = std::stoi(argv[i + 1]) != 0;
        }
    }
    return options;
}
}

int main(int argc, char* argv[])
{
    try
    {
        const Options options = parseOptions(argc, argv);
        const TimerResolution timerResolution;
        SetConsoleCtrlHandler(onConsoleSignal, TRUE);

        trymmo::TcpTransport tcp(options.port);
        trymmo::ITransport* transport = &tcp;

        std::optional<trymmo::LagTransport> lag;
        if (options.lagRttMs > 0)
        {
            lag.emplace(tcp, std::chrono::milliseconds(options.lagRttMs / 2));
            transport = &*lag;
            trymmo::logInfo("artificial lag enabled, rtt " + std::to_string(options.lagRttMs) + "ms");
        }

        trymmo::GameServer server(*transport, options.zone);
        trymmo::logInfo("listening on port " + std::to_string(options.port));
        server.run(g_stop);
        trymmo::logInfo("stopped");
    }
    catch (const std::exception& error)
    {
        trymmo::logWarn(std::string("fatal: ") + error.what());
        return 1;
    }
    return 0;
}
