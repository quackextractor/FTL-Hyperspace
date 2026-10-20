#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Host-authoritative co-op over plain TCP (meant for Tailscale IPs).
// Config: "netplay.txt" next to FTLGame.exe, one line: "host 7777" or "join 100.x.y.z 7777".
namespace Netplay
{
    enum MsgType : uint8_t { HELLO = 1, FULL = 2, DELTA = 3 };

    // Frame = u32 little-endian payload length, u8 type, payload.
    void Encode(std::vector<uint8_t>& out, MsgType type, const std::vector<uint8_t>& payload);
    // Pops one complete frame from the front of buf; false if incomplete.
    bool Decode(std::vector<uint8_t>& buf, MsgType& type, std::vector<uint8_t>& payload);

    void Poll(); // called every CApp::OnLoop
}
