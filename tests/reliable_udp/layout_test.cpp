#include "lib_network/ReliableUDP.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace
{
template <size_t N> bool expect_bytes(const char *name, const void *object, const std::array<uint8_t, N> &expected)
{
    std::array<uint8_t, N> actual{};
    std::memcpy(actual.data(), object, N);
    if (actual == expected)
        return true;

    std::cerr << name << " wire layout differs from the existing Linux/x86 protocol\n";
    std::cerr << "expected:";
    for (const auto value : expected)
        std::cerr << ' ' << std::hex << static_cast<unsigned>(value);
    std::cerr << "\nactual  :";
    for (const auto value : actual)
        std::cerr << ' ' << std::hex << static_cast<unsigned>(value);
    std::cerr << std::dec << '\n';
    return false;
}
} // namespace

int main()
{
    TRXUnit::Header header{};
    header.frame_seq = 0x1234;
    header.group_seq = 0x5678;
    header.group_len = 0x9abc;
    header.group_num = 0xde;
    header.units_idx = 0x0a;
    header.units_num = 0x0b;
    header.units_len = 0x123;
    header.conn_uuid = 0x456;
    header.check_sum = 0x78;

    const std::array<uint8_t, 12> expected_header = {
        0x34, 0x12, 0x78, 0x56, 0xbc, 0x9a, 0xde, 0xba, 0x23, 0x61, 0x45, 0x78,
    };

    TRXProbe probe{};
    probe.magic = 0xa5;
    probe.type = 1;
    probe.seq = 0x23;
    probe.pad = 0x45;
    probe.t1_ms = 0x12345678;
    probe.t2_delta_ms = static_cast<int32_t>(0x89abcdefu);
    const std::array<uint8_t, 12> expected_probe = {
        0xa5, 0x01, 0x23, 0x45, 0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x89,
    };

    const bool ok = expect_bytes("TRXUnit::Header", &header, expected_header) &&
                    expect_bytes("TRXProbe", &probe, expected_probe);
    if (!ok)
        return 1;

    std::cout << "ReliableUDP protocol layout matches the existing wire format\n";
    return 0;
}
