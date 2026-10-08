#include "meshgen.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "game.hpp"
#include "raymath.h"

namespace {

constexpr float kHalf = Game::N / 2.0f;

struct Rng {
    std::mt19937 gen;
    explicit Rng(unsigned seed) : gen(seed) {}
    float operator()(float a, float b) { return a + (b - a) * (gen() / 4294967296.0f); }
};

Color mul(Color c, float k) {
    auto ch = [k](unsigned char v) { return (unsigned char)Clamp(v * k, 0.0f, 255.0f); };
    return {ch(c.r), ch(c.g), ch(c.b), 255};
}

template <typename T>
T* copyToMesh(const std::vector<T>& v) {
    T* out = (T*)MemAlloc((unsigned int)(v.size() * sizeof(T)));
    std::memcpy(out, v.data(), v.size() * sizeof(T));
    return out;
}

// ---------- трава ----------

struct BladeBuf {
    std::vector<float> pos, uv;
    std::vector<unsigned char> col;

    void vert(Vector3 p, float h, Color c) {
        pos.insert(pos.end(), {p.x, p.y, p.z});
        uv.insert(uv.end(), {0.0f, h});
        col.insert(col.end(), {c.r, c.g, c.b, 255});
    }
    // Травинка: сужающаяся полоска из трёх треугольников, наклонённая на lean
    void blade(Vector2 base, float yaw, float height, float width, float lean, Color c) {
        const Vector2 side = {std::cos(yaw), std::sin(yaw)};
        const Vector2 fwd = {-side.y, side.x};
        auto pt = [&](float along, float out, float h) {
            return Vector3{base.x + side.x * along + fwd.x * out, height * h, base.y + side.y * along + fwd.y * out};
        };
        const Vector3 b0 = pt(-width * 0.5f, 0, 0), b1 = pt(width * 0.5f, 0, 0);
        const Vector3 m0 = pt(-width * 0.32f, lean * 0.35f, 0.55f), m1 = pt(width * 0.32f, lean * 0.35f, 0.55f);
        const Vector3 tip = pt(0, lean, 1.0f);
        vert(b0, 0, c); vert(b1, 0, c); vert(m1, 0.55f, c);
        vert(b0, 0, c); vert(m1, 0.55f, c); vert(m0, 0.55f, c);
        vert(m0, 0.55f, c); vert(m1, 0.55f, c); vert(tip, 1, c);
    }
    Mesh upload() const {
        Mesh mesh{};
        mesh.vertexCount = (int)(pos.size() / 3);
        mesh.triangleCount = mesh.vertexCount / 3;
        mesh.vertices = copyToMesh(pos);
        mesh.texcoords = copyToMesh(uv);
        mesh.colors = copyToMesh(col);
        UploadMesh(&mesh, false);
        return mesh;
    }
};

// ---------- объёмные модели ----------

struct Solid {
    std::vector<float> pos, nrm;
    std::vector<unsigned char> col;

    void vert(Vector3 p, Vector3 n, Color c) {
        pos.insert(pos.end(), {p.x, p.y, p.z});
        nrm.insert(nrm.end(), {n.x, n.y, n.z});
        col.insert(col.end(), {c.r, c.g, c.b, 255});
    }
    // треугольник с плоской нормалью
    void tri(Vector3 a, Vector3 b, Vector3 c, Color ca, Color cb, Color cc) {
        const Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a)));
        vert(a, n, ca); vert(b, n, cb); vert(c, n, cc);
    }
    // усечённый конус от a (радиус ra) до b (радиус rb): стволы и ветки
    void tube(Vector3 a, Vector3 b, float ra, float rb, int sides, Color c) {
        const Vector3 axis = Vector3Normalize(Vector3Subtract(b, a));
        const Vector3 ref = std::fabs(axis.y) < 0.99f ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
        const Vector3 u = Vector3Normalize(Vector3CrossProduct(axis, ref));
        const Vector3 w = Vector3CrossProduct(axis, u);
        auto dir = [&](int i) {
            const float t = 2.0f * PI * i / sides;
            return Vector3Add(Vector3Scale(u, std::cos(t)), Vector3Scale(w, std::sin(t)));
        };
        for (int i = 0; i < sides; ++i) {
            const Vector3 d0 = dir(i), d1 = dir(i + 1);
            const Vector3 a0 = Vector3Add(a, Vector3Scale(d0, ra)), a1 = Vector3Add(a, Vector3Scale(d1, ra));
            const Vector3 b0 = Vector3Add(b, Vector3Scale(d0, rb)), b1 = Vector3Add(b, Vector3Scale(d1, rb));
            vert(a0, d0, c); vert(a1, d1, c); vert(b1, d1, c);
            vert(a0, d0, c); vert(b1, d1, c); vert(b0, d0, c);
        }
    }
    // неровный шар: крона, куст, камень. Снизу темнее — как будто листва затеняет сама себя
    void blob(Vector3 c, Vector3 rad, Color col, float rough, float seed) {
        const int R = 8, S = 12;
        struct V {
            Vector3 p, n;
            Color c;
        };
        auto at = [&](int i, int j) {
            const float th = PI * i / R, ph = 2.0f * PI * j / S;
            const Vector3 d = {std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
            const float f = 1.0f + rough * (std::sin(d.x * 3.1f + seed) + std::sin(d.y * 4.3f + seed * 1.7f) +
                                            std::sin(d.z * 3.7f + seed * 2.3f)) / 3.0f;
            const float shade = 0.68f + 0.42f * (d.y * 0.5f + 0.5f) + 0.07f * std::sin(d.x * 6.0f + d.z * 5.0f + seed);
            return V{{c.x + d.x * rad.x * f, c.y + d.y * rad.y * f, c.z + d.z * rad.z * f},
                     Vector3Normalize({d.x / rad.x, d.y / rad.y, d.z / rad.z}),
                     mul(col, shade)};
        };
        for (int i = 0; i < R; ++i)
            for (int j = 0; j < S; ++j) {
                const V p = at(i, j), pj = at(i, j + 1), pi = at(i + 1, j), pij = at(i + 1, j + 1);
                vert(p.p, p.n, p.c); vert(pj.p, pj.n, pj.c); vert(pij.p, pij.n, pij.c);
                vert(p.p, p.n, p.c); vert(pij.p, pij.n, pij.c); vert(pi.p, pi.n, pi.c);
            }
    }
    // ярус ели: конус с неровным, слегка провисающим краем
    void cone(Vector3 base, float r, float h, int sides, Color rim, Color apex, float seed) {
        auto edge = [&](int k) {
            const float t = 2.0f * PI * k / sides + seed;
            const float rr = r * (1.0f + 0.14f * std::sin(k * 2.39f + seed * 3.0f));
            return Vector3{base.x + std::cos(t) * rr, base.y - 0.10f * (1.0f + std::sin(k * 1.7f + seed)), base.z + std::sin(t) * rr};
        };
        const Vector3 top = {base.x, base.y + h, base.z};
        const Color under = mul(rim, 0.55f);
        for (int k = 0; k < sides; ++k) {
            const Vector3 e0 = edge(k), e1 = edge((k + 1) % sides);
            tri(e0, top, e1, rim, apex, rim);
            tri(base, e0, e1, under, under, under);
        }
    }
    Mesh upload() const {
        Mesh mesh{};
        mesh.vertexCount = (int)(pos.size() / 3);
        mesh.triangleCount = mesh.vertexCount / 3;
        mesh.vertices = copyToMesh(pos);
        mesh.normals = copyToMesh(nrm);
        mesh.colors = copyToMesh(col);
        UploadMesh(&mesh, false);
        return mesh;
    }
};

}  // namespace

Mesh genFieldGrass() {
    Rng rnd(20261008);
    BladeBuf buf;
    for (int cz = 0; cz < Game::N; ++cz)
        for (int cx = 0; cx < Game::N; ++cx)
            for (int i = 0; i < 80; ++i) {
                const Vector2 base = {cx - kHalf + rnd(0, 1), cz - kHalf + rnd(0, 1)};
                // клетки в шахматном порядке — как полосы на стриженом газоне
                Color c = ((cx + cz) & 1) ? Color{124, 200, 66, 255} : Color{58, 128, 46, 255};
                if (rnd(0, 1) < 0.04f) c = {158, 176, 74, 255};
                buf.blade(base, rnd(0, 6.283f), rnd(0.16f, 0.34f), rnd(0.045f, 0.075f), rnd(0.04f, 0.14f), mul(c, rnd(0.9f, 1.1f)));
            }
    return buf.upload();
}

Mesh genWildGrass() {
    Rng rnd(4242);
    BladeBuf buf;
    const int ring = 13;
    for (int cz = -ring; cz < Game::N + ring; ++cz)
        for (int cx = -ring; cx < Game::N + ring; ++cx) {
            if (cx >= 0 && cz >= 0 && cx < Game::N && cz < Game::N) continue;
            const int out = std::max({-cx, -cz, cx - Game::N + 1, cz - Game::N + 1});  // на сколько клеток от поля
            const int count = out <= 4 ? 34 : 16;
            for (int i = 0; i < count; ++i) {
                const Vector2 base = {cx - kHalf + rnd(0, 1), cz - kHalf + rnd(0, 1)};
                if (std::max(std::fabs(base.x), std::fabs(base.y)) < kHalf + 0.75f) continue;  // место под забор
                const float tall = out <= 4 ? 1.0f : 1.25f;
                buf.blade(base, rnd(0, 6.283f), rnd(0.28f, 0.62f) * tall, rnd(0.05f, 0.09f) * tall, rnd(0.06f, 0.24f),
                          mul({56, 118, 44, 255}, rnd(0.85f, 1.15f)));
            }
        }
    return buf.upload();
}

Mesh genOak(unsigned seed) {
    Rng rnd(seed);
    Solid s;
    const Color bark = {104, 76, 52, 255};
    static const Color greens[4] = {{70, 140, 52, 255}, {88, 156, 58, 255}, {58, 124, 60, 255}, {112, 160, 54, 255}};
    const Color leaf = greens[seed % 4];

    const float H = rnd(2.0f, 2.7f);
    const Vector3 p0 = {0, -0.2f, 0};
    const Vector3 p1 = {rnd(-0.12f, 0.12f), H * 0.5f, rnd(-0.12f, 0.12f)};
    const Vector3 p2 = {p1.x + rnd(-0.15f, 0.15f), H, p1.z + rnd(-0.15f, 0.15f)};
    s.tube(p0, {0, 0.3f, 0}, 0.46f, 0.31f, 9, bark);  // расширение у корней
    s.tube(p0, p1, 0.31f, 0.24f, 9, bark);
    s.tube(p1, p2, 0.24f, 0.17f, 9, bark);
    s.tube(p2, {p2.x, H + 1.0f, p2.z}, 0.17f, 0.07f, 7, bark);

    const int branches = 4;
    const float a0 = rnd(0, 6.283f);
    for (int b = 0; b < branches; ++b) {
        const float ang = a0 + b * 6.283f / branches + rnd(-0.3f, 0.3f);
        const Vector3 from = Vector3Lerp(p1, p2, rnd(0.6f, 1.0f));
        const float reach = rnd(0.9f, 1.3f);
        const Vector3 to = {p2.x + std::cos(ang) * reach, H + rnd(0.4f, 0.9f), p2.z + std::sin(ang) * reach};
        s.tube(from, to, 0.11f, 0.05f, 6, bark);
        const float r = rnd(0.85f, 1.15f);
        s.blob({to.x, to.y + 0.2f, to.z}, {r, r * 0.8f, r}, mul(leaf, rnd(0.9f, 1.1f)), 0.18f, rnd(0, 10));
    }
    const float tr = rnd(1.2f, 1.45f);
    s.blob({p2.x, H + 1.25f, p2.z}, {tr, tr * 0.85f, tr}, leaf, 0.18f, rnd(0, 10));
    for (int i = 0; i < 3; ++i) {
        const float r = rnd(0.7f, 1.0f);
        s.blob({p2.x + rnd(-0.9f, 0.9f), H + rnd(0.7f, 1.9f), p2.z + rnd(-0.9f, 0.9f)}, {r, r * 0.85f, r},
               mul(leaf, rnd(0.88f, 1.12f)), 0.2f, rnd(0, 10));
    }
    return s.upload();
}

Mesh genBirch(unsigned seed) {
    Rng rnd(seed);
    Solid s;
    const Color bark = {232, 228, 214, 255}, mark = {52, 50, 48, 255};
    const Color leaf = (seed & 1) ? Color{124, 178, 72, 255} : Color{142, 186, 66, 255};

    const float H = rnd(3.2f, 3.9f);
    const Vector3 p0 = {0, -0.2f, 0};
    const Vector3 p1 = {rnd(-0.15f, 0.15f), H * 0.5f, rnd(-0.15f, 0.15f)};
    const Vector3 p2 = {p1.x + rnd(-0.2f, 0.2f), H + 0.6f, p1.z + rnd(-0.2f, 0.2f)};
    s.tube(p0, p1, 0.14f, 0.11f, 8, bark);
    s.tube(p1, p2, 0.11f, 0.05f, 8, bark);
    // тёмные поперечные полоски на коре
    for (int i = 0; i < 9; ++i) {
        const float t = rnd(0.05f, 0.95f);
        const Vector3 c = Vector3Lerp(p0, p1, t);
        const float r = 0.14f - 0.03f * t + 0.006f;
        s.tube(c, {c.x, c.y + rnd(0.03f, 0.07f), c.z}, r, r, 8, mark);
    }
    for (int i = 0; i < 8; ++i) {
        const float t = rnd(0.1f, 1.0f);
        const Vector3 c = Vector3Lerp(p1, p2, t);
        const float r = rnd(0.5f, 0.78f) * (1.15f - 0.4f * t);
        const float away = rnd(0.0f, 0.55f), ang = rnd(0, 6.283f);
        s.blob({c.x + std::cos(ang) * away, c.y + rnd(-0.1f, 0.3f), c.z + std::sin(ang) * away}, {r, r * 1.25f, r},
               mul(leaf, rnd(0.88f, 1.12f)), 0.2f, rnd(0, 10));
    }
    return s.upload();
}

Mesh genPine(unsigned seed) {
    Rng rnd(seed);
    Solid s;
    const Color bark = {84, 60, 44, 255};
    const Color dark = (seed & 1) ? Color{30, 88, 58, 255} : Color{38, 100, 56, 255};

    s.tube({0, -0.2f, 0}, {0, 1.8f, 0}, 0.24f, 0.16f, 8, bark);
    const int tiers = 5;
    for (int i = 0; i < tiers; ++i) {
        const float y = 1.0f + i * rnd(0.82f, 0.94f);
        const float r = 1.7f - i * 0.31f;
        const float lift = 1.0f + 0.10f * i;  // верхние ярусы светлее
        s.cone({rnd(-0.05f, 0.05f), y, rnd(-0.05f, 0.05f)}, r, 1.55f, 11, mul(dark, 0.85f * lift), mul(dark, 1.25f * lift),
               rnd(0, 6.283f));
    }
    return s.upload();
}

Mesh genBush(unsigned seed) {
    Rng rnd(seed);
    Solid s;
    const Color leaf = (seed & 1) ? Color{64, 132, 56, 255} : Color{84, 148, 60, 255};
    for (int i = 0; i < 4; ++i) {
        const float r = rnd(0.32f, 0.52f);
        s.blob({rnd(-0.35f, 0.35f), r * 0.75f, rnd(-0.35f, 0.35f)}, {r, r * 0.85f, r}, mul(leaf, rnd(0.9f, 1.1f)), 0.2f, rnd(0, 10));
    }
    return s.upload();
}

Mesh genRock(unsigned seed) {
    Rng rnd(seed);
    Solid s;
    const float r = rnd(0.28f, 0.5f);
    s.blob({0, r * 0.3f, 0}, {r, r * rnd(0.5f, 0.7f), r * rnd(0.7f, 1.0f)}, mul({134, 132, 136, 255}, rnd(0.85f, 1.1f)), 0.3f,
           rnd(0, 10));
    return s.upload();
}

Mesh genStar() {
    Solid s;
    const float depth = 0.28f;
    const Vector3 front = {0, 0, depth}, back = {0, 0, -depth};
    auto rim = [](int k) {
        const float t = PI / 2.0f + k * PI / 5.0f, r = (k % 2 == 0) ? 1.0f : 0.45f;
        return Vector3{std::cos(t) * r, std::sin(t) * r, 0};
    };
    for (int k = 0; k < 10; ++k) {
        const Vector3 a = rim(k), b = rim((k + 1) % 10);
        s.tri(front, a, b, WHITE, WHITE, WHITE);
        s.tri(back, b, a, WHITE, WHITE, WHITE);
    }
    return s.upload();
}
