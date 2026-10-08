// Oracle check: HLE sceVu0MulMatrix (PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/VU.cpp)
// versus the guest libvu0 routine at 0x2757A0, using the full-state snapshots in
// work/explore/snap/.
//
// The guest routine at 0x2757A0 keeps the four rows of its second argument (a1) in VU0 registers and,
// for each row of its third argument (a2), accumulates MULA/MADDA over those rows and stores the result row:
// => out[i][j] = sum_k a2[i][k] * a1[k][j]      i.e. out = a2 x a1 (row-major storage)
//
// HLE: mulVuMatrix(m0=a1, m1=a2, out) computes out[i][j] = sum_k a1[i][k]*a2[k][j] = a1 x a2.
//
// Build: clang++ -std=c++20 -O2 -ffp-contract=off -o /tmp/x mulmatrix_order_check.cpp
// Run from the project root (the parent of game/); the snapshot paths below are relative to it.
// The two helper functions below are copied from PS2Recomp (GPL-3.0) ps2xRuntime/src/lib/Kernel/Stubs/VU.cpp.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <fstream>
#include <algorithm>
#include <iterator>

static std::vector<uint8_t> loadRam(const char *path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void rdM(const std::vector<uint8_t> &r, uint32_t a, float (&m)[16]) { std::memcpy(m, &r[a], 64); }

// ---- verbatim copies of the HLE helpers (VU.cpp anonymous namespace) ----
static void mulVuMatrix(const float (&lhs)[16], const float (&rhs)[16], float (&out)[16])
{
    std::fill(std::begin(out), std::end(out), 0.0f);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                out[4 * i + j] += rhs[4 * k + j] * lhs[4 * i + k];
}
static void rigidInverse(const float (&in)[16], float (&out)[16])
{
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
            out[4 * row + col] = in[4 * col + row];
        out[4 * row + 3] = 0.0f;
    }
    const float tx = in[12], ty = in[13], tz = in[14];
    for (int col = 0; col < 3; ++col)
        out[12 + col] = -((tx * in[4 * col]) + (ty * in[4 * col + 1]) + (tz * in[4 * col + 2]));
    out[15] = in[15];
}
// ---- guest-order product (what 0x2757A0 computes): out = a2 x a1 ----
static void guestMulMatrix(const float (&a1)[16], const float (&a2)[16], float (&out)[16])
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float acc = a1[0 * 4 + j] * a2[4 * i + 0];
            acc = acc + a1[1 * 4 + j] * a2[4 * i + 1];
            acc = acc + a1[2 * 4 + j] * a2[4 * i + 2];
            out[4 * i + j] = acc + a1[3 * 4 + j] * a2[4 * i + 3];
        }
}
static void pr(const char *n, const float (&m)[16])
{
    std::printf("%s\n", n);
    for (int i = 0; i < 4; ++i)
        std::printf("   %14.7g %14.7g %14.7g %14.7g\n", m[4 * i], m[4 * i + 1], m[4 * i + 2], m[4 * i + 3]);
}
// Max distance in ULPs between corresponding lanes (sign-magnitude aware).
static int64_t maxUlp(const float (&a)[16], const float (&b)[16])
{
    int64_t worst = 0;
    for (int i = 0; i < 16; ++i)
    {
        int32_t x, y;
        std::memcpy(&x, &a[i], 4);
        std::memcpy(&y, &b[i], 4);
        const int64_t ox = x < 0 ? (int64_t)INT32_MIN - x : x;
        const int64_t oy = y < 0 ? (int64_t)INT32_MIN - y : y;
        worst = std::max<int64_t>(worst, ox > oy ? ox - oy : oy - ox);
    }
    return worst;
}
static int cmpApprox(const float (&a)[16], const float (&b)[16])
{
    int diff = 0;
    for (int i = 0; i < 16; ++i)
    {
        const float tol = 1e-4f * std::max(1.0f, std::max(std::fabs(a[i]), std::fabs(b[i])));
        if (std::fabs(a[i] - b[i]) > tol) ++diff;
    }
    return diff;
}
static void xform(const float (&v)[4], const float (&m)[16], float (&o)[4])
{
    for (int j = 0; j < 4; ++j)
        o[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
}

int main()
{
    int failures = 0;
    // ------------------------------------------------------------------
    // Part 1: 220458 invocation #200 (a0=s2=0x164e350). Its stack frame is intact
    // at exit (HLE stubs use no guest stack). sp = 0x1fffdd0 - 0x250.
    //   P  = sp+0x90 (projection, row2 w=1, row3 w=0)
    //   P2 = sp+0x50
    //   camera world W = sp+0x160 ; V = InversMatrix(W) then neg column 1 (0x2205A4..0x2205C0)
    //   s2+0x00 = MulMatrix(a1=P , a2=V) ; s2+0x40 = MulMatrix(a1=P2, a2=V)
    // ------------------------------------------------------------------
    {
        auto r = loadRam("work/explore/snap/220458_200_exit.ram");
        if (r.size() < 0x2000000) { std::printf("cannot read 220458 snapshot\n"); return 2; }
        const uint32_t s2 = 0x164e350, sp = 0x1fffdd0 - 0x250;
        float P[16], P2[16], W[16], V[16], st0[16], st40[16];
        rdM(r, sp + 0x90, P); rdM(r, sp + 0x50, P2); rdM(r, sp + 0x160, W);
        rdM(r, s2 + 0x00, st0); rdM(r, s2 + 0x40, st40);
        rigidInverse(W, V);
        V[1] = -V[1]; V[5] = -V[5]; V[9] = -V[9]; V[13] = -V[13];
        float hle0[16], hle40[16], g0[16], g40[16];
        mulVuMatrix(P, V, hle0); mulVuMatrix(P2, V, hle40);
        guestMulMatrix(P, V, g0); guestMulMatrix(P2, V, g40);
        pr("[220458] V (reconstructed view)", V);
        pr("[220458] stored s2+0x00", st0); pr("[220458] HLE P x V", hle0); pr("[220458] guest-order V x P", g0);
        pr("[220458] stored s2+0x40", st40); pr("[220458] HLE P2 x V", hle40); pr("[220458] guest-order V x P2", g40);
        // V is reconstructed (rigidInverse of the stored camera-world matrix), so allow a
        // couple of ULPs of reconstruction noise; the wrong order differs by orders of magnitude.
        const int64_t u0 = maxUlp(st0, hle0), u40 = maxUlp(st40, hle40);
        const int g0d = cmpApprox(st0, g0), g40d = cmpApprox(st40, g40);
        std::printf("[220458] stored s2+0x00 vs HLE(PxV): max %lld ulp; vs guest order V x P: %d/16 lanes differ (rel 1e-4)\n", (long long)u0, g0d);
        std::printf("[220458] stored s2+0x40 vs HLE(P2xV): max %lld ulp; vs guest order V x P2: %d/16 lanes differ (rel 1e-4)\n", (long long)u40, g40d);
        if (u0 > 4 || u40 > 4 || g0d == 0 || g40d == 0) { std::printf("  -> unexpected: stored output does not match the HLE order\n"); ++failures; }
    }
    // ------------------------------------------------------------------
    // Part 2: race camera, 21f540 #200 exit (s2 = 0x1823EF0). Stack frame was reused,
    // so recover V from the HLE output S = P2 x V with P2 = [[a,0,0,0],[0,a,0,0],[0,0,c,1],[0,0,d,0]]
    //   a = s3[0x10]/512 (s3=0x1824210, s3[0x10]=500), c = *(gp-0x7E4C), d = *(gp-0x7E48)
    //   S0=aV0, S1=aV1, S2=cV2+V3, S3=dV2.
    // Then project the car origin (car struct 0x1820470 + 0x90) through both orders and
    // the screen matrix at s2+0x80.
    // ------------------------------------------------------------------
    {
        auto r = loadRam("work/explore/snap/21f540_200_exit.ram");
        if (r.size() < 0x2000000) { std::printf("cannot read 21f540 snapshot\n"); return 2; }
        const uint32_t s2 = 0x1823EF0, gp = 0x3dd7f0;
        float S[16], Scr[16], c, d, a;
        rdM(r, s2 + 0x40, S); rdM(r, s2 + 0x80, Scr);
        std::memcpy(&c, &r[gp - 0x7E4C], 4); std::memcpy(&d, &r[gp - 0x7E48], 4);
        float s3_10; std::memcpy(&s3_10, &r[0x1824210 + 0x10], 4);
        a = s3_10 * (1.0f / 512.0f);
        float P2[16] = {a, 0, 0, 0, 0, a, 0, 0, 0, 0, c, 1, 0, 0, d, 0};
        float V[16];
        for (int j = 0; j < 4; ++j)
        {
            V[0 + j] = S[0 + j] / a;
            V[4 + j] = S[4 + j] / a;
            V[8 + j] = S[12 + j] / d;
            V[12 + j] = S[8 + j] - c * V[8 + j];
        }
        float hle[16], good[16];
        mulVuMatrix(P2, V, hle);
        guestMulMatrix(P2, V, good);
        std::printf("\n[race] a=%g c=%.8g d=%.8g\n", a, c, d);
        pr("[race] recovered V", V);
        pr("[race] stored 0x1823F30 (HLE order)", S);
        std::printf("[race] HLE(P2xV) re-derivation differs from stored in %d/16 lanes (approx)\n", cmpApprox(S, hle));
        pr("[race] correct guest order V x P2", good);
        std::printf("[race] NOTE: V itself was built upstream (21EAC8 -> 208738 -> MulMatrix) with the same reversed\n"
                    "       product, so the camera position is also wrong; the projection below is informational only.\n"
                    "       End-to-end proof is the relinked runner (frames in work/explore/mmfix_f vs work/explore/f).\n");
        float car[4];
        std::memcpy(car, &r[0x1820470 + 0x90], 16);
        car[3] = 1.0f;
        for (int pass = 0; pass < 2; ++pass)
        {
            const float(&M)[16] = pass ? good : hle;
            float clip[4], scr[4];
            xform(car, M, clip);
            xform(clip, Scr, scr);
            const float sx = scr[0] / scr[3], sy = scr[1] / scr[3], sz = scr[2] / scr[3];
            std::printf("[race] %s: car(%g,%g,%g) -> clip(%g,%g,%g,%g) -> screen x=%g y=%g z=%g  %s\n",
                        pass ? "GUEST ORDER" : "HLE ORDER  ", car[0], car[1], car[2], clip[0], clip[1], clip[2], clip[3], sx, sy, sz,
                        (std::fabs(sx - 2048) < 320 && std::fabs(sy - 2048) < 224 && scr[3] > 0) ? "ON SCREEN" : "off screen / invalid");
        }
    }
    std::printf("\n%s\n", failures ? "FAIL" : "OK: stored camera matrices are the HLE (reversed-order) products");
    return failures ? 1 : 0;
}
