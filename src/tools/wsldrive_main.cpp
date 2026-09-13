// wsldrive: client CLI. For now a measurement tool for the transport + agent
// layer; the WinFsp mount front-end lands on top of RemoteRoot.
//
//   wsldrive fetch --connect tcp://127.0.0.1:7788 [--watch] [--read <path>] [--lookups N]
//   wsldrive fetch --listen hv://7788          (wait for the WSL2 agent to dial in)
#include "agent/client.hpp"
#include "core/auth_token.hpp"
#include "net/frame_channel.hpp"
#include "net/socket.hpp"

#ifdef _WIN32
// wide.hpp sits under _WIN32, not WSLDRIVE_HAVE_WINFSP: `doctor` needs it and
// exists in every Windows build, including one made without the WinFsp SDK.
#include "platform/win/wide.hpp"

#include <windows.h>
#endif
#ifdef WSLDRIVE_HAVE_MOUNT
#include "mount/fuse_mount.hpp"
#include <atomic>
#include <filesystem>
#endif
#ifdef WSLDRIVE_HAVE_WINFSP
#include "platform/win/wsl_launch.hpp"
#endif
#if defined(WSLDRIVE_HAVE_MOUNT) && !defined(_WIN32)
#include "platform/linux/win_launch.hpp"
#include <csignal>
#endif

#include <algorithm>
#include <optional>
#include <chrono>
#include <cstdio>
#include <memory>
#include "core/version.hpp"

#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

void print_version(const char* who) { std::printf("%s %s\n", who, std::string(wsld::kVersionString).c_str()); }

#ifdef _WIN32
// Runs a command with no window and returns its exit code (or a non-zero
// sentinel if it could not be started). doctor only ever needs pass/fail.
DWORD run_quiet(std::wstring cmd) {
  cmd.push_back(L'\0');
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  DWORD code = 1;
  if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    ::WaitForSingleObject(pi.hProcess, 15000);
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
  }
  return code;
}

// Starts a command without waiting for it. The caller kills it via the returned
// handle; nullptr means it could not be started.
HANDLE spawn_quiet(std::wstring cmd) {
  cmd.push_back(L'\0');
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return nullptr;
  ::CloseHandle(pi.hThread);
  return pi.hProcess;
}
std::wstring widen(std::string_view s) { return wsld::platform::win::to_wide(s); }

// Median round-trip time, in milliseconds, from inside the distro back to a
// listener on Windows over loopback. std::nullopt if it could not be measured.
//
// This one number decides whether Direction B needs the Hyper-V socket
// transport, and it cannot be inferred from the installed WSL version: a
// running WSL2 VM keeps the kernel it booted with, so a machine can carry the
// improvement for months without it taking effect. Two machines on identical
// WSL versions can differ, which is why this measures rather than checks.
//
// Uses bash's /dev/tcp rather than our own binaries, so it works before
// anything has been staged into the distro. The distro end is a plain echo
// (`cat <&3 >&3`); all the timing happens here.
std::optional<double> probe_loopback_rtt(const std::string& distro) {
  auto listener = wsld::net::Listener::bind(*wsld::net::Endpoint::parse("tcp://127.0.0.1:0"));
  if (!listener) return std::nullopt;
  // Endpoint::port is a uint32_t; keep it that way rather than narrowing to
  // uint16_t just to format it (MSVC 14.51 treats that conversion as an error
  // under /W4 /WX, and there is no reason to convert at all).
  const std::uint32_t port = listener->local().port;
  const std::wstring d = distro.empty() ? L"" : L" -d " + widen(distro);
  HANDLE echo = spawn_quiet(L"wsl.exe" + d + L" -e bash -lc \"exec 3<>/dev/tcp/127.0.0.1/" +
                            std::to_wstring(port) + L"; cat <&3 >&3\"");
  std::optional<double> result;
  if (auto peer = listener->accept(std::chrono::seconds(10))) {
    std::vector<double> rtt;
    std::byte out{std::byte{42}};
    std::byte in{};
    for (int i = 0; i < 20; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      if (!peer->send_all(std::span<const std::byte>(&out, 1))) break;
      if (!peer->recv_exact(std::span<std::byte>(&in, 1))) break;
      rtt.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    if (rtt.size() >= 5) {
      std::sort(rtt.begin(), rtt.end());
      result = rtt[rtt.size() / 2];
    }
    peer->close();
  }
  if (echo != nullptr) {
    ::TerminateProcess(echo, 0);
    ::CloseHandle(echo);
  }
  listener->close();
  return result;
}

// Below this, plain TCP is as good as the Hyper-V socket transport for
// Direction B. The slow path was seconds per round-trip, not milliseconds, so
// the threshold does not have to be delicate.
constexpr double kFastLoopbackMs = 5.0;

#endif

void usage() {
  std::fprintf(stderr, "wsldrive %s\n", std::string(wsld::kVersionString).c_str());
  std::fputs(
      "usage:\n"
      "  wsldrive fetch (--connect <endpoint> | --listen <endpoint>) [--watch] [--read <path>] [--lookups N]\n"
#ifdef _WIN32
      "  wsldrive doctor [--distro X] [--hvsocket] [--port N] [--probe-transport] [--pause]\n"
      "  wsldrive probe-transport [--distro X]              (loopback round-trip, ms; for scripts)\n"
      "                                                       (check the WinFsp + WSL environment)\n"
#endif
#ifdef WSLDRIVE_HAVE_MOUNT
      "  wsldrive mount <mountpoint> --connect <endpoint> [--writeback] [--no-prefetch] [--no-inotify]\n"
      "                                                       (attach to an agent)\n"
#endif
#if defined(WSLDRIVE_HAVE_MOUNT) && !defined(_WIN32)
      "  wsldrive mount <dir> --win-root <winpath> --win-agent <wsldrived.exe> [--hvsocket [--vm-guid G]]\n"
      "                                                       (Direction B: serve a Windows path into WSL)\n"
#endif
#ifdef WSLDRIVE_HAVE_WINFSP
      "  wsldrive mount <drive> --distro <name> --wsl-root <path> [--agent <path>] [--port N]\n"
      "                                                       (auto-launch the agent in the distro)\n"
      "     add --hvsocket [--vm-guid <guid>]                 (use the Hyper-V socket transport)\n"
#endif
      "  wsldrive --version\n"
      ,
      stderr);
}

double ms(std::chrono::nanoseconds d) { return std::chrono::duration<double, std::milli>(d).count(); }

#ifdef WSLDRIVE_HAVE_MOUNT
std::atomic<bool> g_mount_stop{false};
#ifdef _WIN32
BOOL WINAPI mount_ctrl_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    g_mount_stop.store(true);
    return TRUE;  // handled: don't let the default handler kill us before cleanup
  }
  return FALSE;
}
#else
extern "C" void mount_signal_handler(int) { g_mount_stop.store(true); }
#endif
void install_mount_signal_handler() {
#ifdef _WIN32
  ::SetConsoleCtrlHandler(mount_ctrl_handler, TRUE);
#else
  std::signal(SIGINT, mount_signal_handler);
  std::signal(SIGTERM, mount_signal_handler);
#endif
}
#endif

}  // namespace

int main(int argc, char** argv) {
  // Answered before anything else, so it still works on a machine where the
  // rest of the environment (WinFsp, a distro, a reachable agent) is not set
  // up - which is exactly when someone needs to know which build they have.
  if (argc >= 2) {
    const std::string_view first = argv[1];
    if (first == "--version" || first == "-V" || first == "version") {
      print_version("wsldrive");
      return 0;
    }
  }
  const std::string_view command = argc >= 2 ? std::string_view(argv[1]) : std::string_view{};

#ifdef _WIN32
  // Machine-readable form of doctor's transport probe, for the installer: prints
  // the median round-trip in milliseconds and nothing else, exits 0 when it is
  // fast enough for plain TCP and 1 when it is not (2 if it could not measure).
  // The installer picks the Direction B transport from this rather than from a
  // WSL version number, which would be wrong on any VM that has not restarted
  // since its kernel updated.
  if (command == "probe-transport") {
    std::string distro;
    for (int i = 2; i < argc; ++i) {
      const std::string_view a = argv[i];
      if (a == "--distro" && i + 1 < argc) distro = argv[++i];
      else {
        std::fprintf(stderr, "probe-transport: unknown option %.*s\n", static_cast<int>(a.size()), a.data());
        return 2;
      }
    }
    const auto rtt = probe_loopback_rtt(distro);
    if (!rtt) return 2;
    std::printf("%.3f\n", *rtt);
    return *rtt < kFastLoopbackMs ? 0 : 1;
  }

  if (command == "doctor") {
    // doctor [--distro X] [--hvsocket] [--port N] [--pause]
    std::string want_distro;
    int want_port = 0;
    bool check_hv = false;
    bool probe_transport = false;
    bool pause_at_end = false;
    for (int i = 2; i < argc; ++i) {
      const std::string_view a = argv[i];
      if (a == "--distro" && i + 1 < argc) want_distro = argv[++i];
      else if (a == "--port" && i + 1 < argc) want_port = std::atoi(argv[++i]);
      else if (a == "--hvsocket") check_hv = true;
      else if (a == "--probe-transport") probe_transport = true;
      else if (a == "--pause") pause_at_end = true;   // the installer runs doctor in a console that closes
      else {
        std::fprintf(stderr, "doctor: unknown option %.*s\n", static_cast<int>(a.size()), a.data());
        return 2;
      }
    }
    int problems = 0;
    // WinFsp: present at build time?
#ifdef WSLDRIVE_HAVE_WINFSP
    std::printf("[ok]   wsldrive was built with WinFsp support (mount available)\n");
#else
    std::printf("[warn] wsldrive was built without WinFsp; the mount command is unavailable\n");
    ++problems;
#endif
    // WinFsp runtime: registry InstallDir.
    {
      HKEY key{};
      wchar_t dir[MAX_PATH]{};
      DWORD size = sizeof(dir);
      bool found = false;
      if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\WinFsp", 0, KEY_READ | KEY_WOW64_32KEY, &key) ==
          ERROR_SUCCESS) {
        found = ::RegQueryValueExW(key, L"InstallDir", nullptr, nullptr, reinterpret_cast<LPBYTE>(dir), &size) ==
                ERROR_SUCCESS;
        ::RegCloseKey(key);
      }
      if (found)
        std::printf("[ok]   WinFsp runtime installed\n");
      else {
        std::printf("[fail] WinFsp runtime not found (install from https://winfsp.dev)\n");
        ++problems;
      }
    }
    // wsl.exe on PATH.
    {
      wchar_t path[MAX_PATH]{};
      if (::SearchPathW(nullptr, L"wsl.exe", nullptr, MAX_PATH, path, nullptr) > 0)
        std::printf("[ok]   wsl.exe found on PATH\n");
      else {
        std::printf("[fail] wsl.exe not found on PATH\n");
        ++problems;
      }
    }
    // WSL actually runs: `wsl.exe -e true`.
    {
      std::wstring cmd = L"wsl.exe -e true";
      cmd.push_back(L'\0');
      STARTUPINFOW si{};
      si.cb = sizeof(si);
      PROCESS_INFORMATION pi{};
      DWORD code = 1;
      if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ::WaitForSingleObject(pi.hProcess, 10000);
        ::GetExitCodeProcess(pi.hProcess, &code);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
      }
      if (code == 0)
        std::printf("[ok]   WSL is functional (ran a command in the default distro)\n");
      else {
        std::printf("[fail] could not run a command via WSL (exit %lu)\n", code);
        ++problems;
      }
    }
    // The named distro, not just the default one. `mount --distro X` fails for
    // its own reasons if X is not there, and doctor was not checking it.
    if (!want_distro.empty()) {
      if (run_quiet(L"wsl.exe -d " + widen(want_distro) + L" -e true") == 0)
        std::printf("[ok]   distro '%s' runs commands\n", want_distro.c_str());
      else {
        std::printf("[fail] distro '%s' did not run a command (is the name right? `wsl -l -v`)\n",
                    want_distro.c_str());
        ++problems;
      }
    }

    // The agent staged into the distro by the installer. Without it, a mount
    // that auto-launches the agent fails at launch rather than at configure.
    {
      const std::wstring d = want_distro.empty() ? L"" : L" -d " + widen(want_distro);
      const std::wstring where = want_distro.empty() ? L"the default distro" : widen(want_distro);
      if (run_quiet(L"wsl.exe" + d + L" -e sh -c \"test -x \\\"$HOME/.local/bin/wsldrived\\\"\"") == 0)
        std::printf("[ok]   agent staged in %ls (~/.local/bin/wsldrived)\n", where.c_str());
      else
        std::printf("[warn] no agent staged in %ls; `mount --agent <path>` or re-run the installer\n",
                    where.c_str());  // not a failure: --agent can point elsewhere
    }

    // Hyper-V socket service GUIDs. Direction B needs these registered, and the
    // docs send people to doctor to find out - which it could not answer.
    if (check_hv) {
      const int first = want_port != 0 ? want_port : 5700;
      int registered = 0;
      for (int p = first; p < first + 10; ++p) {
        wchar_t sub[160]{};
        _snwprintf_s(sub, _TRUNCATE,
                     L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Virtualization\\GuestCommunicationServices\\"
                     L"%08X-facb-11e6-bd58-64006a7986d3",
                     static_cast<unsigned>(p));
        HKEY k{};
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &k) == ERROR_SUCCESS) {
          ++registered;
          ::RegCloseKey(k);
        }
      }
      if (registered == 10)
        std::printf("[ok]   hvsocket service GUIDs registered for ports %d-%d\n", first, first + 9);
      else {
        std::printf("[fail] only %d/10 hvsocket GUIDs registered for ports %d-%d\n"
                    "       run scripts\\register-hvsocket.ps1 elevated, then `wsl --shutdown`\n",
                    registered, first, first + 9);
        ++problems;
      }
    }

    // Is the loopback port actually free? A stale agent holding it turns into a
    // confusing mount failure later.
    if (want_port != 0) {
      wsld::net::init_network();
      auto ep = wsld::net::Endpoint::parse("tcp://127.0.0.1:" + std::to_string(want_port));
      bool free_port = false;
      if (ep) {
        auto l = wsld::net::Listener::bind(*ep);
        if (l) {
          free_port = true;
          l->close();
        }
      }
      if (free_port) {
        std::printf("[ok]   TCP port %d is free\n", want_port);
      } else {
        std::printf("[fail] TCP port %d is in use (a previous agent still running?)\n", want_port);
        ++problems;
      }
    }

    // How long is a round-trip from inside the distro back to Windows over
    // loopback? That one number decides whether Direction B needs the Hyper-V
    // socket transport, and it cannot be inferred from the installed WSL
    // version: a running WSL2 VM keeps the kernel it booted with, so a machine
    // can carry the improvement for months without it taking effect.
    //
    // Measured with bash's /dev/tcp rather than our own binaries, so it works
    // before anything has been staged into the distro. The distro end is a
    // plain echo (`cat <&3 >&3`); all the timing happens here.
    if (probe_transport) {
      if (const auto rtt = probe_loopback_rtt(want_distro)) {
        std::printf("[ok]   loopback round-trip from the distro: %.3f ms\n", *rtt);
        if (*rtt < kFastLoopbackMs)
          std::printf("       fast enough for Direction B over plain TCP; hvsocket is not required here\n");
        else
          std::printf("       slow: Direction B needs the Hyper-V socket transport on this machine\n");
      } else {
        std::printf("[warn] transport probe: could not measure (no bash /dev/tcp in the distro?)\n");
      }
    }

    struct PauseAtExit {
      bool on;
      ~PauseAtExit() {
        if (!on) return;
        std::printf("\nPress Enter to close...");
        std::fflush(stdout);
        (void)std::getchar();
      }
    } pause_guard{pause_at_end};

    std::printf("\n%s\n", problems == 0 ? "All checks passed. Mount with: wsldrive mount W: --distro <name> --wsl-root <path>"
                                        : "Some checks failed; resolve the items above before mounting.");
    return problems == 0 ? 0 : 1;
  }
#endif

#ifdef WSLDRIVE_HAVE_MOUNT
  if (command == "mount") {
    if (argc < 3) {
      usage();
      return 2;
    }
    const std::string mountpoint = argv[2];
    std::string connect, listen, distro, wsl_root, agent_path, win_root, win_agent;
    std::string auth_token = wsld::auth_token_from_env();  // inherited for --connect; generated for auto-launch
    std::uint32_t port = 51789;
    bool have_distro = false;
    bool writeback = false;
    bool hvsocket = false;
    bool prefetch = true;
    bool notify_changes = true;
    std::string vm_guid;
    for (int i = 3; i < argc; ++i) {
      const std::string_view a = argv[i];
      auto val = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
      if (a == "--connect")
        connect = val();
      else if (a == "--listen")
        listen = val();
      else if (a == "--hvsocket")
        hvsocket = true;
      else if (a == "--vm-guid")
        vm_guid = val();
      else if (a == "--win-root")
        win_root = val();
      else if (a == "--win-agent")
        win_agent = val();
      else if (a == "--writeback")
        writeback = true;
      else if (a == "--no-prefetch")
        prefetch = false;
      else if (a == "--no-inotify")
        notify_changes = false;
      else if (a == "--distro") {
        distro = val();
        have_distro = true;
      } else if (a == "--wsl-root")
        wsl_root = val();
      else if (a == "--agent")
        agent_path = val();
      else if (a == "--port")
        port = static_cast<std::uint32_t>(std::stoul(val()));
      else {
        usage();
        return 2;
      }
    }

#ifdef WSLDRIVE_HAVE_WINFSP
    // Auto-launch mode (Windows only): start wsldrived in the distro, then connect.
    wsld::platform::WslAgent agent;
    if (have_distro) {
      if (wsl_root.empty()) {
        std::fprintf(stderr, "wsldrive: --wsl-root is required with --distro\n");
        return 2;
      }
      // At logon after a reboot the distro may not be up yet (WSL2 starts
      // lazily). Wait for it before launching the agent so the auto-start mount
      // does not lose a cold-boot race; a hard failure returns non-zero so the
      // scheduled task's retry can take over.
      std::printf("waiting for WSL (%s) to be ready...\n", distro.empty() ? "default distro" : distro.c_str());
      std::fflush(stdout);
      if (!wsld::platform::wait_wsl_ready(distro, std::chrono::seconds(90))) {
        std::fprintf(stderr, "wsldrive: distro '%s' is not available (not installed, or did not start within 90s)\n",
                     distro.empty() ? "(default)" : distro.c_str());
        return 1;
      }
      // WSL2 routes hvsocket host->guest: the agent listens on vsock and this
      // client connects to the guest VM over AF_HYPERV. Needs a registered vsock
      // port (scripts/register-hvsocket.ps1) and the VM's GUID.
      if (hvsocket && port == 51789) port = 5700;  // a registered vsock port by default
      // Per-mount shared secret: generated here, handed to the agent through the
      // environment (WSLENV forwards it into the distro, so it never appears on a
      // command line) and presented by this client in the Hello handshake.
      auth_token = wsld::generate_auth_token();
      if (auth_token.empty()) {
        std::fprintf(stderr, "wsldrive: could not generate an authentication token\n");
        return 1;
      }
      wsld::platform::WslAgentSpec spec{
          .distro = distro, .agent_path = agent_path, .wsl_root = wsl_root, .port = port, .token = auth_token};
      if (hvsocket) {
        std::string guid = vm_guid.empty() ? wsld::platform::discover_wsl_vm_guid() : vm_guid;
        if (guid.empty()) {
          std::fprintf(stderr,
                       "wsldrive: could not determine the WSL VM GUID for --hvsocket.\n"
                       "  Pass --vm-guid <guid> (from an elevated `hcsdiag list`), or run elevated.\n");
          return 2;
        }
        spec.listen = "vsock://any:" + std::to_string(port);
        connect = "hv://{" + guid + "}:" + std::to_string(port);
      }
      if (auto r = agent.start(spec); !r) {
        std::fprintf(stderr, "wsldrive: failed to launch agent in distro '%s'\n", distro.c_str());
        return 1;
      }
      if (!hvsocket) connect = "tcp://127.0.0.1:" + std::to_string(port);
      std::printf("launched agent in %s over %s, waiting for it to listen...\n",
                  distro.empty() ? "(default distro)" : distro.c_str(), hvsocket ? "hvsocket" : "tcp");
      std::fflush(stdout);
      // A hvsocket connect() blocks for a long OS timeout if the service is not
      // yet listening (unlike TCP's fast refusal), so let the agent bind first.
      if (hvsocket) std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    } else
#endif
        if (connect.empty() && listen.empty() && win_root.empty()) {
      usage();
      return 2;
    }

#ifndef _WIN32
    // Linux (Direction B): the FUSE mountpoint must exist as an empty directory.
    {
      std::error_code ec;
      std::filesystem::create_directories(mountpoint, ec);
    }
#endif

    (void)distro;
    (void)wsl_root;
    (void)agent_path;
    (void)port;
    (void)hvsocket;
    (void)vm_guid;
    (void)win_root;
    (void)win_agent;
    (void)have_distro;

#if defined(WSLDRIVE_HAVE_MOUNT) && !defined(_WIN32)
    // Direction B: this WSL client listens and auto-launches the Windows agent
    // (via interop) to serve a Windows path and dial back in.
    std::string win_agent_connect;
    wsld::platform::WinAgent win_proc;
    if (!win_root.empty()) {
      if (win_agent.empty()) {
        std::fprintf(stderr, "wsldrive: --win-agent <path-to-wsldrived.exe> is required with --win-root\n");
        return 2;
      }
      if (hvsocket && port == 51789) port = 5700;
      // Per-mount shared secret for the agent we are about to launch.
      auth_token = wsld::generate_auth_token();
      if (auth_token.empty()) {
        std::fprintf(stderr, "wsldrive: could not generate an authentication token\n");
        return 1;
      }
      if (hvsocket) {
        std::string guid = vm_guid.empty() ? wsld::platform::discover_wsl_vm_guid() : vm_guid;
        if (guid.empty()) {
          std::fprintf(stderr, "wsldrive: could not determine the WSL VM GUID; pass --vm-guid <guid>\n");
          return 2;
        }
        listen = "vsock://any:" + std::to_string(port);
        win_agent_connect = "hv://{" + guid + "}:" + std::to_string(port);
      } else {
        listen = "tcp://127.0.0.1:" + std::to_string(port);  // loopback only: the Windows agent dials in locally
        win_agent_connect = "tcp://127.0.0.1:" + std::to_string(port);
      }
    }
#endif

    std::string source;
    wsld::Result<wsld::net::Socket> sock = wsld::fail(wsld::Errc::ConnectionClosed);
    if (!listen.empty()) {
      // Wait for the agent to dial in (used when the agent cannot accept inbound
      // connections, e.g. a Windows agent reaching a WSL client past the firewall).
      auto lep = wsld::net::Endpoint::parse(listen);
      if (!lep) {
        std::fprintf(stderr, "wsldrive: bad endpoint '%s'\n", listen.c_str());
        return 2;
      }
      auto l = wsld::net::Listener::bind(*lep);
      if (!l) {
        std::fprintf(stderr, "wsldrive: listen on %s failed: %s\n", lep->to_string().c_str(),
                     wsld::to_string(l.error()));
        return 1;
      }
      source = l->local().to_string();
      std::printf("waiting for agent on %s ...\n", source.c_str());
      std::fflush(stdout);
#if defined(WSLDRIVE_HAVE_MOUNT) && !defined(_WIN32)
      // The listener is up, so launch the Windows agent to dial back in now (an
      // AF_HYPERV connect would block if it were started before we were listening).
      if (!win_agent_connect.empty()) {
        if (auto r = win_proc.start(
                {.exe = win_agent, .win_root = win_root, .connect = win_agent_connect, .token = auth_token});
            !r) {
          std::fprintf(stderr, "wsldrive: failed to launch Windows agent '%s'\n", win_agent.c_str());
          return 1;
        }
        std::printf("launched Windows agent serving %s over %s\n", win_root.c_str(), hvsocket ? "hvsocket" : "tcp");
        std::fflush(stdout);
      }
#endif
      while (!g_mount_stop.load()) {
        auto s = l->accept(std::chrono::milliseconds(200));
        if (s) {
          sock = std::move(*s);
          break;
        }
        if (s.error() != wsld::Errc::Timeout) break;
      }
    } else {
      auto ep = wsld::net::Endpoint::parse(connect);
      if (!ep) {
        std::fprintf(stderr, "wsldrive: bad endpoint '%s'\n", connect.c_str());
        return 2;
      }
      source = ep->to_string();
      // Poll for the agent to accept (auto-launch needs a moment; --connect is instant).
      // Generous window for a cold-boot distro that is still finishing startup.
      const int attempts = have_distro ? 300 : 1;
      for (int i = 0; i < attempts; ++i) {
        sock = wsld::net::connect(*ep);
        if (sock) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    if (!sock) {
      std::fprintf(stderr, "wsldrive: connect/accept failed: %s (os error %d)\n", wsld::to_string(sock.error()),
                   wsld::net::last_socket_error());
      return 1;
    }
    wsld::agent::RemoteRoot root(std::make_unique<wsld::net::FrameChannel>(std::move(*sock)));
    root.set_auth_token(auth_token);
    if (auto h = root.connect(); !h) {
      std::fprintf(stderr, "wsldrive: handshake failed: %s\n", wsld::to_string(h.error()));
      return 1;
    }
    if (auto r = root.fetch_snapshot(); !r) {
      std::fprintf(stderr, "wsldrive: snapshot failed: %s\n", wsld::to_string(r.error()));
      return 1;
    }
    const auto ts = root.with_tree([](const wsld::MetadataTree& t) { return t.stats(); });
    std::printf("mounting %s (%zu nodes) at %s ...\n", source.c_str(), ts.nodes, mountpoint.c_str());
    if (ts.dropped > 0)
      std::printf("  note: %zu entries dropped (names this filesystem cannot represent, e.g. a backslash)\n",
                  ts.dropped);
    if (prefetch) {
      const std::size_t dirs = root.warm_cache();  // background warm-up so first reads are hot
      if (dirs > 0) std::printf("prefetching %zu directories in the background...\n", dirs);
    }
    wsld::mount::FuseMount fm(root);
    if (auto r = fm.mount(mountpoint, writeback, notify_changes); !r) {
      std::fprintf(stderr, "wsldrive: mount failed: %s\n", wsld::to_string(r.error()));
      return 1;
    }
    install_mount_signal_handler();
    std::printf("mounted. Ctrl+C to unmount.\n");
    if (notify_changes && wsld::mount::FsNotifyBridge::supported())
      std::printf("far-side changes are delivered to local inotify watchers (--no-inotify turns this off)\n");
    std::fflush(stdout);
    std::uint64_t rescans_seen = 0, rescan_failures_seen = 0;
    while (fm.mounted() && root.connected() && !g_mount_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      // A rescan means the agent's watcher lost events and the mirror was
      // rebuilt from a fresh snapshot; worth a line in the log, since heavy
      // churn on the served tree is exactly when someone would look here.
      const auto st = root.stats();
      if (st.rescans != rescans_seen || st.rescan_failures != rescan_failures_seen) {
        rescans_seen = st.rescans;
        rescan_failures_seen = st.rescan_failures;
        std::fprintf(stderr, "wsldrive: watcher overflow on the agent; snapshot re-fetched (%llu ok, %llu failed)\n",
                     static_cast<unsigned long long>(st.rescans), static_cast<unsigned long long>(st.rescan_failures));
      }
    }
    // Always reported, not only on loss. Change delivery is invisible when it
    // works and equally invisible when it does not, so a count of what actually
    // went out is the only way to tell the two apart after the fact.
    if (const auto ns = fm.notify_stats(); ns.events != 0)
      std::printf("change notifications: %llu delivered, %llu failed, %llu dropped, %llu rescans (of %llu)\n",
                  static_cast<unsigned long long>(ns.delivered), static_cast<unsigned long long>(ns.failed),
                  static_cast<unsigned long long>(ns.dropped), static_cast<unsigned long long>(ns.rescans),
                  static_cast<unsigned long long>(ns.events));
    std::printf("unmounting...\n");
    std::fflush(stdout);
    fm.unmount();  // agent (if auto-launched) is stopped by its destructor on return
    return 0;
  }
#endif

  if (command != "fetch") {
    usage();
    return 2;
  }
  std::string connect, listen, read_path;
  bool watch = false;
  long lookups = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--connect")
      connect = next();
    else if (a == "--listen")
      listen = next();
    else if (a == "--watch")
      watch = true;
    else if (a == "--read")
      read_path = next();
    else if (a == "--lookups")
      lookups = std::stol(next());
    else {
      usage();
      return 2;
    }
  }
  if (connect.empty() == listen.empty()) {
    usage();
    return 2;
  }

  wsld::net::Socket sock;
  if (!connect.empty()) {
    auto ep = wsld::net::Endpoint::parse(connect);
    if (!ep) {
      std::fprintf(stderr, "wsldrive: bad endpoint '%s'\n", connect.c_str());
      return 2;
    }
    auto s = wsld::net::connect(*ep);
    if (!s) {
      std::fprintf(stderr, "wsldrive: connect failed: %s (os error %d)\n", wsld::to_string(s.error()),
                   wsld::net::last_socket_error());
      return 1;
    }
    sock = std::move(*s);
  } else {
    auto ep = wsld::net::Endpoint::parse(listen);
    if (!ep) {
      std::fprintf(stderr, "wsldrive: bad endpoint '%s'\n", listen.c_str());
      return 2;
    }
    auto l = wsld::net::Listener::bind(*ep);
    if (!l) {
      std::fprintf(stderr, "wsldrive: listen failed: %s (os error %d)\n", wsld::to_string(l.error()),
                   wsld::net::last_socket_error());
      return 1;
    }
    std::fprintf(stderr, "wsldrive: waiting for agent on %s\n", l->local().to_string().c_str());
    auto s = l->accept();
    if (!s) {
      std::fprintf(stderr, "wsldrive: accept failed\n");
      return 1;
    }
    sock = std::move(*s);
  }

  wsld::agent::RemoteRoot root(std::make_unique<wsld::net::FrameChannel>(std::move(sock)));
  root.set_auth_token(wsld::auth_token_from_env());
  auto hello = root.connect();
  if (!hello) {
    std::fprintf(stderr, "wsldrive: handshake failed: %s\n", wsld::to_string(hello.error()));
    return 1;
  }
  auto rtt = root.ping();
  std::printf("connected: protocol v%u, agent capabilities 0x%llx, ping %.3f ms\n", hello->protocol_version,
              static_cast<unsigned long long>(hello->capabilities), rtt ? ms(*rtt) : -1.0);

  if (auto r = root.fetch_snapshot(); !r) {
    std::fprintf(stderr, "wsldrive: snapshot failed: %s\n", wsld::to_string(r.error()));
    return 1;
  }
  const auto st = root.stats();
  const auto tree_stats = root.with_tree([](const wsld::MetadataTree& t) { return t.stats(); });
  std::printf("snapshot: %zu nodes, %zu distinct names (%zu bytes), %zu case collisions, %zu wire bytes, %.1f ms\n",
              tree_stats.nodes, tree_stats.names, tree_stats.name_bytes, tree_stats.case_collisions,
              st.snapshot_bytes, ms(st.last_snapshot_time));
  if (tree_stats.dropped > 0)
    std::printf("          %zu entries dropped (names this filesystem cannot represent, e.g. a backslash)\n",
                tree_stats.dropped);

  if (lookups > 0) {
    std::vector<std::string> paths;
    root.with_tree([&](const wsld::MetadataTree& t) {
      std::vector<wsld::NodeId> stack{t.root()};
      while (!stack.empty() && paths.size() < 200000) {
        const wsld::NodeId n = stack.back();
        stack.pop_back();
        t.for_each_child(n, [&](wsld::NodeId c) {
          if (t.node(c).is_dir())
            stack.push_back(c);
          else
            paths.push_back(t.path_of(c));
        });
      }
    });
    if (!paths.empty()) {
      std::mt19937 rng(42);
      std::vector<const std::string*> probes(4096);
      for (auto& p : probes) p = &paths[rng() % paths.size()];
      const auto t0 = std::chrono::steady_clock::now();
      std::size_t hits = 0;
      for (long i = 0; i < lookups; ++i)
        hits += root.with_tree([&](const wsld::MetadataTree& t) {
          return t.lookup(*probes[static_cast<std::size_t>(i) & 4095], wsld::LookupMode::CaseInsensitive).has_value();
        });
      const auto dt = std::chrono::steady_clock::now() - t0;
      std::printf("lookups: %ld case-insensitive path lookups under shared lock, %zu hits, %.1f ns each\n", lookups, hits,
                  static_cast<double>(dt.count()) / static_cast<double>(lookups));
    }
  }

  if (!read_path.empty()) {
    const auto t0 = std::chrono::steady_clock::now();
    auto data = root.read(read_path, 0, 16u << 20);
    const auto dt = std::chrono::steady_clock::now() - t0;
    if (!data)
      std::printf("read %s: %s\n", read_path.c_str(), wsld::to_string(data.error()));
    else
      std::printf("read %s: %zu bytes in %.2f ms (%.1f MiB/s)\n", read_path.c_str(), data->size(), ms(dt),
                  static_cast<double>(data->size()) / (1024.0 * 1024.0) / (ms(dt) / 1000.0));
  }

  if (watch) {
    std::printf("watching for invalidations (Ctrl+C to stop)...\n");
    root.set_invalidation_hook([&](const wsld::proto::InvalidationBatch& b) {
      const auto now = std::chrono::system_clock::now().time_since_epoch();
      std::printf("[gen %llu +%lld ms] %zu ops\n", static_cast<unsigned long long>(b.generation),
                  static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count() % 100000),
                  b.ops.size());
      for (const auto& op : b.ops) {
        const char* change = "";
        switch (op.change) {
          case wsld::ChangeKind::Created: change = " (created)"; break;
          case wsld::ChangeKind::Modified: change = " (modified)"; break;
          case wsld::ChangeKind::Removed: change = " (removed)"; break;
          case wsld::ChangeKind::MovedFrom: change = " (moved from)"; break;
          case wsld::ChangeKind::MovedTo: change = " (moved to)"; break;
          case wsld::ChangeKind::Unknown: break;
        }
        char cookie[32] = "";
        if (op.cookie != 0) std::snprintf(cookie, sizeof(cookie), " #%u", op.cookie);
        std::printf("   %s %s%s%s\n",
                    op.kind == wsld::InvalidationKind::Upsert   ? "upsert"
                    : op.kind == wsld::InvalidationKind::Remove ? "remove"
                                                                : "rescan",
                    op.path.c_str(), change, cookie);
      }
      std::fflush(stdout);
    });
    while (root.connected()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::printf("agent disconnected\n");
  }
  return 0;
}
