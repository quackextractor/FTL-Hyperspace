#include "Netplay.h"
#include "Global.h"
#include <csignal>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

void Netplay::Encode(std::vector<uint8_t>& out, MsgType type, const std::vector<uint8_t>& payload)
{
    uint32_t len = payload.size();
    out.insert(out.end(), (uint8_t*)&len, (uint8_t*)&len + 4);
    out.push_back(type);
    out.insert(out.end(), payload.begin(), payload.end());
}

bool Netplay::Decode(std::vector<uint8_t>& buf, MsgType& type, std::vector<uint8_t>& payload)
{
    if (buf.size() < 5) return false;
    uint32_t len;
    memcpy(&len, buf.data(), 4);
    if (buf.size() < 5 + len) return false;
    type = (MsgType)buf[4];
    payload.assign(buf.begin() + 5, buf.begin() + 5 + len);
    buf.erase(buf.begin(), buf.begin() + 5 + len);
    return true;
}

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

namespace
{
    enum Mode { OFF, HOST, JOIN };
    Mode mode = OFF;
    bool configRead = false;
    std::string joinAddr;
    int port = 7777;

    SOCKET listener = INVALID_SOCKET;
    SOCKET peer = INVALID_SOCKET;
    std::vector<uint8_t> inbox, outbox;
    uint64_t lastHello = 0, lastDelta = 0, lastConnect = 0;
    bool wantFull = true; // client: still need a FULL from the host

    const int DELTA_MS = 100;

    // Own log per mode: FTL_HS.log is shared (and truncated) when two instances run from one folder
    FILE* logFile = nullptr;
    void Log(const char* fmt, ...)
    {
        if (!logFile) return;
        va_list va;
        va_start(va, fmt);
        vfprintf(logFile, fmt, va);
        va_end(va);
        fflush(logFile);
    }

    void LogAddress(const char* label, DWORD addr)
    {
        HMODULE m;
        char name[MAX_PATH] = "?";
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m)) return;
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        Log("%s %s+0x%lx\n", label, base ? base + 1 : name, addr - (DWORD)m);
    }

    // Diagnostics: logs the faulting address and code-looking stack values as module+offset
    LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep)
    {
        Log("CRASH code 0x%08lx\n", ep->ExceptionRecord->ExceptionCode);
        LogAddress("  at", (DWORD)ep->ExceptionRecord->ExceptionAddress);
        DWORD* sp = (DWORD*)ep->ContextRecord->Esp;
        for (int i = 0; i < 512; i++) LogAddress("  stack", sp[i]);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void ReadConfig()
    {
        configRead = true;
        // NETPLAY env var overrides the file, so two instances can run from one folder
        const char* env = getenv("NETPLAY");
        std::istringstream envCfg(env ? env : "");
        std::ifstream file;
        if (!env) file.open("netplay.txt");
        std::istream& f = env ? (std::istream&)envCfg : file;
        std::string word;
        if (!(f >> word)) return;
        if (word == "host") { mode = HOST; f >> port; }
        else if (word == "join") { mode = JOIN; f >> joinAddr >> port; }
        if (mode == OFF) return;
        logFile = fopen(mode == HOST ? "netplay_host.log" : "netplay_join.log", "w");
        SetUnhandledExceptionFilter(OnCrash);
        signal(SIGABRT, [](int) { Log("abort() called (failed assert?)\n"); });
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        Log("%s %s:%d\n", mode == HOST ? "hosting on" : "joining", joinAddr.c_str(), port);
    }

    void SetNonBlocking(SOCKET s)
    {
        u_long on = 1;
        ioctlsocket(s, FIONBIO, &on);
    }

    void Drop()
    {
        if (peer != INVALID_SOCKET) closesocket(peer);
        peer = INVALID_SOCKET;
        inbox.clear();
        outbox.clear();
        wantFull = true;
        Log("disconnected\n");
    }

    void Send(Netplay::MsgType type, const std::vector<uint8_t>& payload)
    {
        if (peer != INVALID_SOCKET) Netplay::Encode(outbox, type, payload);
    }

    std::vector<uint8_t> ReadFile(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    void WriteFile(const std::string& path, const std::vector<uint8_t>& bytes)
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write((const char*)bytes.data(), bytes.size());
    }

    // Per-mode name: host and client may share a user folder when testing on one PC
    std::string TempPath() { return FileHelper::getUserFolder() + (mode == HOST ? "netplay_host.tmp" : "netplay_join.tmp"); }

    // shortcut: state goes through a temp file because FileHelper's in-memory fd needs the
    // saveFileData global's address; switch to a ZHL variable sig once deltas get bigger.
    std::vector<uint8_t> ExportDelta(ShipManager* sm, bool paused)
    {
        int fd = FileHelper::createBinaryFile(TempPath());
        FileHelper::writeInt(fd, paused);
        FileHelper::writeInt(fd, sm->ship.hullIntegrity.first);
        FileHelper::writeInt(fd, sm->vSystemList.size());
        for (auto sys : sm->vSystemList) sys->SaveState(fd);
        FileHelper::writeInt(fd, sm->vCrewList.size());
        for (auto crew : sm->vCrewList) crew->SaveState(fd);
        FileHelper::writeInt(fd, sm->ship.vDoorList.size());
        for (auto door : sm->ship.vDoorList) door->SaveState(fd);
        FileHelper::writeInt(fd, sm->shieldSystem != nullptr);
        if (sm->shieldSystem) sm->shieldSystem->Save(fd);
        sm->ExportBattleState(fd);
        FileHelper::closeBinaryFile(fd);
        return ReadFile(TempPath());
    }

    // Returns false when the ship layout no longer matches and a FULL resync is needed.
    bool ImportDelta(ShipManager* sm, CommandGui* gui, const std::vector<uint8_t>& bytes)
    {
        static int applied = 0;
        bool trace = applied++ < 2; // step markers for the first deltas only
        auto step = [&](const char* s) { if (trace) Log("delta: %s\n", s); };
        WriteFile(TempPath(), bytes);
        int fd = FileHelper::readBinaryFile(TempPath());
        bool ok = true;
        step("pause/hull");
        gui->SetPaused(FileHelper::readInteger(fd), false);
        sm->ship.hullIntegrity.first = FileHelper::readInteger(fd);
        step("systems");
        if ((size_t)FileHelper::readInteger(fd) != sm->vSystemList.size()) ok = false;
        if (ok) for (auto sys : sm->vSystemList) sys->LoadState(fd);
        step("crew");
        if (ok && (size_t)FileHelper::readInteger(fd) != sm->vCrewList.size()) ok = false;
        if (ok) for (auto crew : sm->vCrewList) crew->LoadState(fd);
        // Hyperspace's CrewMember::LoadState recreates crew powers; crew boxes hold pointers to the old ones.
        // shortcut: rebuilds the crew UI every delta, switch to a diff-based crew sync if it flickers.
        if (ok)
        {
            gui->crewControl.ClearCrewBoxes();
            gui->crewControl.UpdateCrewBoxes();
        }
        step("doors");
        if (ok && (size_t)FileHelper::readInteger(fd) != sm->ship.vDoorList.size()) ok = false;
        if (ok) for (auto door : sm->ship.vDoorList) door->LoadState(fd);
        step("shields");
        if (ok && FileHelper::readInteger(fd) != (sm->shieldSystem != nullptr)) ok = false;
        if (ok && sm->shieldSystem) sm->shieldSystem->Load(fd);
        step("battle state");
        if (ok) sm->ImportBattleState(fd);
        FileHelper::closeBinaryFile(fd);
        step(ok ? "done" : "layout mismatch, resync");
        return ok;
    }

    void HandleMessage(Netplay::MsgType type, const std::vector<uint8_t>& payload)
    {
        WorldManager* world = G_->GetWorld();
        CommandGui* gui = G_->GetCApp()->gui;
        switch (type)
        {
        case Netplay::HELLO:
            if (mode == HOST && world->bStartedGame && gui->CanSave())
            {
                world->SaveGame();
                Send(Netplay::FULL, ReadFile(FileHelper::getSaveFile()));
            }
            break;
        case Netplay::FULL:
            if (mode == JOIN)
            {
                Log("loading %u bytes of state\n", (unsigned)payload.size());
                WriteFile(TempPath(), payload);
                world->LoadGame(TempPath());
                Log("LoadGame returned\n");
                G_->GetCApp()->menu.Close();
                wantFull = false;
                Log("joined, %u bytes of state\n", (unsigned)payload.size());
            }
            break;
        case Netplay::DELTA:
            if (mode == JOIN && !wantFull && world->bStartedGame)
            {
                if (!ImportDelta(world->playerShip->shipManager, gui, payload)) wantFull = true;
            }
            break;
        }
    }

    void PumpSocket()
    {
        char buf[65536];
        int n;
        while ((n = recv(peer, buf, sizeof buf, 0)) > 0) inbox.insert(inbox.end(), buf, buf + n);
        if (n == 0 || (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) { Drop(); return; }
        while (!outbox.empty())
        {
            n = send(peer, (const char*)outbox.data(), outbox.size(), 0);
            if (n > 0) outbox.erase(outbox.begin(), outbox.begin() + n);
            else if (WSAGetLastError() != WSAEWOULDBLOCK) { Drop(); return; }
            else break;
        }
        Netplay::MsgType type;
        std::vector<uint8_t> payload;
        while (peer != INVALID_SOCKET && Netplay::Decode(inbox, type, payload)) HandleMessage(type, payload);
    }

    void Connect(uint64_t now)
    {
        if (mode == HOST)
        {
            if (listener == INVALID_SOCKET)
            {
                listener = socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in addr = {};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(port);
                if (bind(listener, (sockaddr*)&addr, sizeof addr) != 0 || listen(listener, 1) != 0)
                {
                    Log("listen failed %d\n", WSAGetLastError());
                    closesocket(listener);
                    listener = INVALID_SOCKET;
                    mode = OFF;
                    return;
                }
                SetNonBlocking(listener);
            }
            peer = accept(listener, nullptr, nullptr);
            if (peer != INVALID_SOCKET) { SetNonBlocking(peer); Log("client connected\n"); }
        }
        else if (now - lastConnect > 2000)
        {
            lastConnect = now;
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            inet_pton(AF_INET, joinAddr.c_str(), &addr.sin_addr);
            SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
            if (connect(s, (sockaddr*)&addr, sizeof addr) == 0) { peer = s; SetNonBlocking(s); Log("connected to host\n"); }
            else closesocket(s);
        }
    }
}

void Netplay::Poll()
{
    if (!configRead) ReadConfig();
    if (mode == OFF) return;
    uint64_t now = GetTickCount64();
    if (peer == INVALID_SOCKET) { Connect(now); return; }

    WorldManager* world = G_->GetWorld();
    if (mode == JOIN && wantFull && now - lastHello > 2000) { lastHello = now; Send(HELLO, {}); }
    if (mode == HOST && world->bStartedGame && now - lastDelta > DELTA_MS)
    {
        lastDelta = now;
        Send(DELTA, ExportDelta(world->playerShip->shipManager, G_->GetCApp()->gui->IsPaused()));
    }
    PumpSocket();
}

HOOK_METHOD(CApp, OnLoop, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnLoop -> Begin (Netplay.cpp)\n")
    super();
    Netplay::Poll();
}
#else
void Netplay::Poll() {}
#endif
