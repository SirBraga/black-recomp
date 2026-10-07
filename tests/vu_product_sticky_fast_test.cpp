#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

struct Result { uint8_t flags; uint32_t bits; };

static Result reference(float left, float right)
{
    float product = left * right;
    const long double exact = static_cast<long double>(left) * static_cast<long double>(right);
    const bool negative = std::signbit(exact);
    const long double magnitude = std::fabs(exact);
    uint8_t flags = negative ? 2u : 0u;
    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0L)
    {
        flags |= 1u;
        product = std::bit_cast<float>(bits);
    }
    else if (magnitude > static_cast<long double>(std::numeric_limits<float>::max()))
    {
        flags |= 8u;
        bits |= 0x7f7fffffu;
        product = std::bit_cast<float>(bits);
    }
    else if (magnitude < static_cast<long double>(std::numeric_limits<float>::min()))
    {
        flags |= 5u;
        product = std::bit_cast<float>(bits);
    }
    return {flags, std::bit_cast<uint32_t>(product)};
}

static Result fast(float left, float right)
{
    const float product = left * right;
    const uint32_t bits = std::bit_cast<uint32_t>(product);
    const uint32_t exponent = (bits >> 23u) & 0xffu;
    if (exponent > 1u && exponent < 254u)
        return {static_cast<uint8_t>((bits >> 31u) * 2u), bits};
    return reference(left, right);
}

int main()
{
    constexpr uint32_t edges[] = {
        0u, 0x80000000u, 1u, 0x80000001u, 0x007fffffu, 0x00800000u,
        0x3f800000u, 0xbf800000u, 0x7f7fffffu, 0xff7fffffu,
        0x7f800000u, 0xff800000u, 0x7fc00000u
    };
    for (uint32_t a : edges)
        for (uint32_t b : edges)
        {
            const auto x = reference(std::bit_cast<float>(a), std::bit_cast<float>(b));
            const auto y = fast(std::bit_cast<float>(a), std::bit_cast<float>(b));
            if (x.flags != y.flags || x.bits != y.bits)
                return 1;
        }
    uint32_t state = 0x6d2b79f5u;
    auto randomBits = [&]() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };
    for (unsigned i = 0; i < 1000000u; ++i)
    {
        const float a = std::bit_cast<float>(randomBits());
        const float b = std::bit_cast<float>(randomBits());
        const auto x = reference(a, b);
        const auto y = fast(a, b);
        if (x.flags != y.flags || x.bits != y.bits)
        {
            std::fprintf(stderr, "mismatch at %u\n", i);
            return 1;
        }
    }
    std::puts("VU product flags: edge matrix and 1,000,000 random pairs passed");
}
