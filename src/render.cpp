#include "render.hpp"

#include <algorithm>
#include <cmath>

#include "meshgen.hpp"
#include "raymath.h"
#include "rlgl.h"

namespace {

constexpr float kHalf = Game::N / 2.0f;
constexpr float kOff = (Game::N - 1) / 2.0f;
constexpr int kMaxPts = 128;      // столько точек видит шейдер травы
constexpr int kShadowRes = 4096;  // сторона карты теней
constexpr int kShadowSlot = 10;   // текстурный слот под карту теней
const Vector3 kLightDir = {-0.48f, -0.66f, -0.58f};  // единичный вектор: солнце справа-спереди, невысоко

// ---------- шейдеры ----------

const char* kLitVS = R"(#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
uniform mat4 lightVP;
uniform float time;
uniform float sway;
out vec3 fragPos;
out vec3 fragNormal;
out vec3 fragTint;
out vec4 fragLight;
void main() {
    // деревья качаются: чем выше вершина, тем сильнее
    vec3 p = vertexPosition;
    float ph = matModel[3].x * 0.7 + matModel[3].z * 0.9;
    p.xz += sway * p.y * p.y * vec2(sin(time * 1.3 + ph), cos(time * 1.05 + ph * 1.3));
    vec4 world = matModel * vec4(p, 1.0);
    fragPos = world.xyz;
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    fragTint = vertexColor.rgb;
    fragLight = lightVP * world;
    gl_Position = mvp * vec4(p, 1.0);
}
)";

// Общая для обоих шейдеров проверка «в тени ли точка» со сглаживанием края
#define SHADOW_GLSL \
    "uniform sampler2D shadowMap;\n" \
    "float shadowAt(vec4 lp, float bias) {\n" \
    "    vec3 c = lp.xyz / lp.w * 0.5 + 0.5;\n" \
    "    if (c.x < 0.0 || c.x > 1.0 || c.y < 0.0 || c.y > 1.0 || c.z > 1.0) return 1.0;\n" \
    "    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));\n" \
    "    float lit = 0.0;\n" \
    "    for (int x = -1; x <= 1; x++)\n" \
    "        for (int y = -1; y <= 1; y++)\n" \
    "            lit += (c.z - bias > texture(shadowMap, c.xy + vec2(x, y) * texel).r) ? 0.0 : 1.0;\n" \
    "    return lit / 9.0;\n" \
    "}\n"

const char* kLitFS = "#version 330\n" SHADOW_GLSL R"(
in vec3 fragPos;
in vec3 fragNormal;
in vec3 fragTint;
in vec4 fragLight;
uniform vec4 colDiffuse;
uniform vec3 viewPos;
uniform vec3 lightDir;
uniform float specStrength;
out vec4 finalColor;
void main() {
    vec3 n = normalize(fragNormal);
    vec3 l = normalize(-lightDir);
    vec3 v = normalize(viewPos - fragPos);
    float ndl = dot(n, l);
    float diff = max(ndl, 0.0);
    float lit = shadowAt(fragLight, mix(0.0014, 0.0004, clamp(ndl, 0.0, 1.0)));
    float spec = pow(max(dot(n, normalize(l + v)), 0.0), 40.0) * specStrength;
    // небо светит сверху голубым, земля снизу — тёплым зелёным
    vec3 ambient = mix(vec3(0.22, 0.26, 0.18), vec3(0.46, 0.52, 0.60), 0.5 + 0.5 * n.y);
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0) * 0.18;
    vec3 base = colDiffuse.rgb * fragTint;
    vec3 col = base * (ambient + diff * lit * vec3(0.85, 0.80, 0.68)) + spec * lit + rim * base;
    finalColor = vec4(col, 1.0);
}
)";

const char* kGrassVS = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;   // y — высота вдоль травинки: 0 у корня, 1 на кончике
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 lightVP;
uniform float time;
uniform vec2 bodyPts[128];
uniform int bodyCount;
out vec3 fragColor;
out vec4 fragLight;
void main() {
    vec3 p = vertexPosition;
    float h = vertexTexCoord.y;

    float wind = sin(time * 1.6 + p.x * 0.9 + p.z * 0.6) * 0.6
               + sin(time * 2.7 + p.x * 2.3 - p.z * 1.7) * 0.3;

    // змейка, бонусы и звери приминают и раздвигают траву вокруг себя
    float press = 0.0;
    vec2 push = vec2(0.0);
    for (int i = 0; i < bodyCount; i++) {
        vec2 d = p.xz - bodyPts[i];
        float dist = length(d);
        float k = 1.0 - smoothstep(0.22, 0.72, dist);
        press = max(press, k);
        push += d / max(dist, 0.05) * k;
    }
    float pl = length(push);
    if (pl > 1.0) push /= pl;

    p.xz += (vec2(0.07, 0.04) * wind + push * 0.20) * h * h;
    p.y *= 1.0 - 0.55 * press;

    float shade = mix(0.45, 1.05, h) * (1.0 - 0.25 * press);
    fragColor = mix(vertexColor.rgb, vertexColor.rgb * vec3(1.18, 1.20, 0.85), h) * shade;
    fragLight = lightVP * vec4(p, 1.0);
    gl_Position = mvp * vec4(p, 1.0);
}
)";

const char* kGrassFS = "#version 330\n" SHADOW_GLSL R"(
in vec3 fragColor;
in vec4 fragLight;
out vec4 finalColor;
void main() {
    float lit = shadowAt(fragLight, 0.0006);
    finalColor = vec4(fragColor * mix(vec3(0.46, 0.52, 0.62), vec3(1.0), lit), 1.0);
}
)";

// ---------- математика ----------

float smooth01(float a, float b, float x) {
    const float t = Clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

Vector2 cellPos(Cell c) { return {c.x - kOff, c.z - kOff}; }

float wrapCoord(float v) { return v - Game::N * std::floor((v + kHalf) / Game::N); }

Matrix placed(Vector3 pos, Vector3 scale) {
    return MatrixMultiply(MatrixScale(scale.x, scale.y, scale.z), MatrixTranslate(pos.x, pos.y, pos.z));
}

// Матрица из трёх осей (уже умноженных на масштаб) и позиции
Matrix fromAxes(Vector3 x, Vector3 y, Vector3 z, Vector3 pos) {
    Matrix m = MatrixIdentity();
    m.m0 = x.x; m.m1 = x.y; m.m2 = x.z;
    m.m4 = y.x; m.m5 = y.y; m.m6 = y.z;
    m.m8 = z.x; m.m9 = z.y; m.m10 = z.z;
    m.m12 = pos.x; m.m13 = pos.y; m.m14 = pos.z;
    return m;
}

// Путь змейки: ломаная по центрам клеток от головы к хвосту. Параметр s —
// расстояние в клетках от текущей клетки головы; тело занимает отрезок [s0, s1].
// Координаты «развёрнуты»: при переходе через край поля путь не рвётся, а
// продолжается за границу — на экран точки возвращает wrapCoord.
struct Path {
    std::vector<Vector2> p;
    float s0 = 0, s1 = 0;

    Vector2 raw(float s) const {
        const int n = (int)p.size();
        s = Clamp(s, 0.0f, (float)(n - 1));
        const int i = std::min((int)s, n - 2);
        return Vector2Lerp(p[i], p[i + 1], s - i);
    }
    // усреднение по соседним точкам скругляет прямые углы
    Vector2 at(float s) const {
        static const float w[7] = {1, 2, 3, 4, 3, 2, 1};
        Vector2 acc = {0, 0};
        for (int k = 0; k < 7; ++k) {
            const Vector2 q = raw(Clamp(s + (k - 3) * 0.14f, s0, s1));
            acc = Vector2Add(acc, Vector2Scale(q, w[k]));
        }
        return Vector2Scale(acc, 1.0f / 16.0f);
    }
    // единичный вектор в сторону головы
    Vector2 tangent(float s, Vector2 fallback) const {
        const Vector2 d = Vector2Subtract(at(s - 0.12f), at(s + 0.12f));
        return Vector2Length(d) > 1e-4f ? Vector2Normalize(d) : fallback;
    }
};

Path makePath(const Game& g, float alpha) {
    auto shortest = [](int d) { return d > Game::N / 2 ? d - Game::N : d < -Game::N / 2 ? d + Game::N : d; };
    Path path;
    Cell prev = g.snake().front();
    Vector2 cur = cellPos(prev);
    path.p.push_back(cur);
    auto extend = [&](Cell c) {
        cur.x += (float)shortest(c.x - prev.x);
        cur.y += (float)shortest(c.z - prev.z);
        prev = c;
        path.p.push_back(cur);
    };
    for (size_t i = 1; i < g.snake().size(); ++i) extend(g.snake()[i]);
    extend(g.prevTail());
    path.s0 = 1.0f - alpha;
    path.s1 = (float)(g.snake().size() - 1) + (1.0f - alpha);
    return path;
}

// Точка на «скруглённом квадрате» вокруг поля: по нему бегают животные
Vector2 ringPos(float theta, float d) {
    const float c = std::cos(theta), s = std::sin(theta);
    const float k = d / std::pow(c * c * c * c + s * s * s * s, 0.25f);
    return {c * k, s * k};
}

RenderTexture2D loadShadowMap(int size) {
    RenderTexture2D target{};
    target.id = rlLoadFramebuffer();
    target.texture.width = size;
    target.texture.height = size;
    if (target.id > 0) {
        rlEnableFramebuffer(target.id);
        target.depth.id = rlLoadTextureDepth(size, size, false);
        target.depth.width = size;
        target.depth.height = size;
        target.depth.format = 19;  // DEPTH_COMPONENT_24BIT
        target.depth.mipmaps = 1;
        rlFramebufferAttach(target.id, target.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
        rlDisableFramebuffer();
    }
    return target;
}

}  // namespace

// ---------- создание ----------

Renderer::Renderer() {
    sphere_ = GenMeshSphere(1.0f, 12, 18);
    cube_ = GenMeshCube(1.0f, 1.0f, 1.0f);
    star_ = genStar();
    fieldGrass_ = genFieldGrass();
    wildGrass_ = genWildGrass();

    litMat_ = LoadMaterialDefault();
    litMat_.shader = LoadShaderFromMemory(kLitVS, kLitFS);
    locView_ = GetShaderLocation(litMat_.shader, "viewPos");
    locLight_ = GetShaderLocation(litMat_.shader, "lightDir");
    locSpec_ = GetShaderLocation(litMat_.shader, "specStrength");
    locSway_ = GetShaderLocation(litMat_.shader, "sway");
    locLitTime_ = GetShaderLocation(litMat_.shader, "time");
    locLitVP_ = GetShaderLocation(litMat_.shader, "lightVP");
    locLitMap_ = GetShaderLocation(litMat_.shader, "shadowMap");

    grassMat_ = LoadMaterialDefault();
    grassMat_.shader = LoadShaderFromMemory(kGrassVS, kGrassFS);
    locTime_ = GetShaderLocation(grassMat_.shader, "time");
    locPts_ = GetShaderLocation(grassMat_.shader, "bodyPts");
    locCount_ = GetShaderLocation(grassMat_.shader, "bodyCount");
    locGrassVP_ = GetShaderLocation(grassMat_.shader, "lightVP");
    locGrassMap_ = GetShaderLocation(grassMat_.shader, "shadowMap");

    shadow_ = loadShadowMap(kShadowRes);
    buildScenery();
}

Renderer::~Renderer() {
    for (Mesh* m : {&sphere_, &cube_, &star_, &fieldGrass_, &wildGrass_}) UnloadMesh(*m);
    for (auto* list : {&trees_, &bushes_, &rocks_})
        for (Mesh& m : *list) UnloadMesh(m);
    UnloadMaterial(litMat_);  // выгружает и шейдер
    UnloadMaterial(grassMat_);
    rlUnloadFramebuffer(shadow_.id);  // вместе с текстурой глубины
}

void Renderer::buildScenery() {
    // по два варианта каждого дерева, чтобы лес не выглядел штампованным
    for (unsigned s = 0; s < 3; ++s) { trees_.push_back(genOak(11 + s)); treeSway_.push_back(0.0022f); }
    for (unsigned s = 0; s < 2; ++s) { trees_.push_back(genBirch(31 + s)); treeSway_.push_back(0.0038f); }
    for (unsigned s = 0; s < 2; ++s) { trees_.push_back(genPine(51 + s)); treeSway_.push_back(0.0014f); }
    for (unsigned s = 0; s < 3; ++s) bushes_.push_back(genBush(71 + s));
    for (unsigned s = 0; s < 3; ++s) rocks_.push_back(genRock(91 + s));

    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    auto rnd = [&](float a, float b) { return a + (b - a) * u01(rng_); };
    std::vector<Vector2> taken;
    auto farFromOthers = [&](Vector2 p, float minDist) {
        return std::none_of(taken.begin(), taken.end(), [&](Vector2 q) { return Vector2Distance(p, q) < minDist; });
    };
    auto place = [&](const Mesh& mesh, Vector2 p, float scale, float sway, bool caster) {
        const Matrix m = MatrixMultiply(MatrixMultiply(MatrixScale(scale, scale, scale), MatrixRotateY(rnd(0, 6.283f))),
                                        MatrixTranslate(p.x, 0, p.y));
        props_.push_back({&mesh, m, WHITE, 0.03f, sway, caster});
        taken.push_back(p);
    };

    // ближние деревья — по бокам и сзади; перед камерой оставлена поляна
    for (int tries = 0, n = 0; tries < 2000 && n < 18; ++tries) {
        const Vector2 p = {rnd(-20, 20), rnd(-20, 20)};
        const float cheb = std::max(std::fabs(p.x), std::fabs(p.y));
        if (cheb < 15.0f || (p.y > 9.0f && std::fabs(p.x) < 15.0f) || !farFromOthers(p, 3.2f)) continue;
        const size_t k = (size_t)rnd(0, (float)trees_.size() - 0.01f);
        place(trees_[k], p, rnd(0.85f, 1.2f), treeSway_[k], true);
        ++n;
    }
    // дальний лес — фон; он за пределами карты теней
    for (int tries = 0, n = 0; tries < 4000 && n < 60; ++tries) {
        const Vector2 p = {rnd(-46, 46), rnd(-46, 46)};
        const float cheb = std::max(std::fabs(p.x), std::fabs(p.y));
        if (cheb < 24.0f || !farFromOthers(p, 3.6f)) continue;
        const size_t k = (size_t)rnd(0, (float)trees_.size() - 0.01f);
        place(trees_[k], p, rnd(1.1f, 1.7f), treeSway_[k], false);
        ++n;
    }
    for (int tries = 0, n = 0; tries < 2000 && n < 22; ++tries) {
        const Vector2 p = {rnd(-15, 15), rnd(-15, 15)};
        const float cheb = std::max(std::fabs(p.x), std::fabs(p.y));
        if (cheb < 9.6f || !farFromOthers(p, 1.6f)) continue;
        if (n % 3 == 2) place(rocks_[n % rocks_.size()], p, rnd(0.8f, 1.4f), 0.0f, true);
        else place(bushes_[n % bushes_.size()], p, rnd(0.8f, 1.3f), 0.012f, true);
        ++n;
    }

    // три кролика и лиса
    const Color coats[3] = {{202, 190, 172, 255}, {150, 120, 94, 255}, {236, 232, 224, 255}};
    for (int i = 0; i < 3; ++i) {
        Animal a;
        a.theta = rnd(0, 6.283f);
        a.d = a.wantD = rnd(10.3f, 13.0f);
        a.speed = 2.7f;
        a.timer = rnd(0.5f, 3.0f);
        a.coat = coats[i];
        animals_.push_back(a);
    }
    Animal fox;
    fox.fox = true;
    fox.theta = rnd(0, 6.283f);
    fox.d = fox.wantD = 11.8f;
    fox.speed = 1.9f;
    fox.moving = true;
    fox.timer = 8.0f;
    fox.coat = {214, 110, 40, 255};
    animals_.push_back(fox);
    for (Animal& a : animals_) {
        const Vector2 p0 = ringPos(a.theta, a.d), p1 = ringPos(a.theta + 0.02f, a.d);
        a.yaw = std::atan2(p1.y - p0.y, p1.x - p0.x);
    }
}

// ---------- животные ----------

void Renderer::update(float dt) {
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    auto rnd = [&](float a, float b) { return a + (b - a) * u01(rng_); };

    for (Animal& a : animals_) {
        a.timer -= dt;
        if (a.timer <= 0.0f) {
            a.moving = !a.moving;
            if (a.fox) {
                a.timer = a.moving ? rnd(7.0f, 13.0f) : rnd(1.2f, 2.8f);
            } else {
                a.timer = a.moving ? rnd(1.2f, 3.5f) : rnd(1.5f, 5.0f);
                if (a.moving) a.sign = u01(rng_) < 0.5f ? -1 : 1;
            }
            if (a.moving) a.wantD = rnd(10.3f, 13.2f);
        }
        // кролик не замирает в воздухе: допрыгивает начатый прыжок
        const bool midHop = !a.fox && (a.phase - std::floor(a.phase)) > 0.06f;
        if (a.moving || midHop) {
            a.theta += a.sign * a.speed * dt / a.d;
            a.d += (a.wantD - a.d) * std::fmin(1.0f, dt * 0.8f);
            a.phase += dt * (a.fox ? 2.4f : 2.8f);
            const Vector2 p0 = ringPos(a.theta, a.d), p1 = ringPos(a.theta + a.sign * 0.02f, a.d);
            const float want = std::atan2(p1.y - p0.y, p1.x - p0.x);
            const float diff = std::remainder(want - a.yaw, 2.0f * PI);
            a.yaw += diff * std::fmin(1.0f, dt * 8.0f);
        } else if (!a.fox) {
            a.phase = std::floor(a.phase);
        }
    }
}

void Renderer::addAnimal(const Animal& a) {
    const Vector2 p = ringPos(a.theta, a.d);
    const Vector3 fwd = {std::cos(a.yaw), 0, std::sin(a.yaw)}, up = {0, 1, 0}, right = {-fwd.z, 0, fwd.x};
    const float cycle = a.phase - std::floor(a.phase);
    const float hop = a.fox ? 0.02f * std::sin(a.phase * 4.0f * PI) : 0.30f * std::sin(cycle * PI);
    const Vector3 base = {p.x, hop, p.y};
    // off и size заданы в осях зверя: вперёд, вверх, вправо
    auto part = [&](const Mesh& mesh, Vector3 off, Vector3 size, Color c, float spec = 0.05f) {
        const Vector3 pos = Vector3Add(base, Vector3Add(Vector3Scale(fwd, off.x), Vector3Add(Vector3Scale(up, off.y), Vector3Scale(right, off.z))));
        add(mesh, fromAxes(Vector3Scale(fwd, size.x), Vector3Scale(up, size.y), Vector3Scale(right, size.z), pos), c, spec);
    };
    const Color black = {18, 16, 16, 255}, white = {244, 242, 236, 255};
    wildPts_.push_back(p);

    if (!a.fox) {
        const float stretch = 1.0f + 0.25f * std::sin(cycle * PI);  // в прыжке кролик вытягивается
        const Color fur = a.coat, dark = ColorLerp(fur, BLACK, 0.2f);
        part(sphere_, {0.0f, 0.24f, 0}, {0.30f * stretch, 0.22f, 0.20f}, fur);
        part(sphere_, {-0.14f, 0.23f, 0}, {0.20f, 0.20f, 0.21f}, fur);
        part(sphere_, {0.27f * stretch, 0.40f, 0}, {0.15f, 0.15f, 0.14f}, fur);
        part(sphere_, {-0.33f * stretch, 0.27f, 0}, {0.075f, 0.075f, 0.075f}, white);
        part(sphere_, {0.415f * stretch, 0.39f, 0}, {0.022f, 0.02f, 0.03f}, {226, 150, 150, 255});
        for (int s = -1; s <= 1; s += 2) {
            part(sphere_, {0.21f * stretch, 0.66f, s * 0.065f}, {0.04f, 0.17f, 0.055f}, dark);
            part(sphere_, {0.36f * stretch, 0.44f, s * 0.10f}, {0.025f, 0.028f, 0.025f}, black, 1.0f);
            part(sphere_, {0.20f * stretch, 0.05f, s * 0.09f}, {0.07f, 0.05f, 0.05f}, dark);
            part(sphere_, {-0.14f * stretch, 0.05f, s * 0.13f}, {0.12f, 0.05f, 0.06f}, dark);
        }
        return;
    }

    const Color fur = a.coat, cream = {240, 226, 204, 255}, leg = {58, 40, 30, 255};
    const float wag = a.moving ? 0.05f * std::sin(a.phase * 2.0f * PI) : 0.02f * std::sin(a.phase * 2.0f * PI);
    part(sphere_, {0.0f, 0.46f, 0}, {0.42f, 0.19f, 0.17f}, fur);
    part(sphere_, {0.28f, 0.41f, 0}, {0.17f, 0.15f, 0.14f}, cream);
    part(sphere_, {0.52f, 0.62f, 0}, {0.18f, 0.16f, 0.17f}, fur);
    part(sphere_, {0.68f, 0.57f, 0}, {0.13f, 0.07f, 0.075f}, cream);
    part(sphere_, {0.80f, 0.58f, 0}, {0.03f, 0.03f, 0.03f}, black, 0.8f);
    part(sphere_, {-0.62f, 0.50f, wag}, {0.30f, 0.11f, 0.11f}, fur);
    part(sphere_, {-0.87f, 0.53f, wag * 1.6f}, {0.10f, 0.085f, 0.085f}, white);
    for (int s = -1; s <= 1; s += 2) {
        part(sphere_, {0.47f, 0.80f, s * 0.10f}, {0.05f, 0.11f, 0.06f}, leg);
        part(sphere_, {0.62f, 0.66f, s * 0.11f}, {0.025f, 0.028f, 0.025f}, black, 1.0f);
        // рысь: ноги по диагонали идут вместе
        for (int f = -1; f <= 1; f += 2) {
            const float swing = a.moving ? std::sin(a.phase * 2.0f * PI + (s * f > 0 ? 0.0f : PI)) : 0.0f;
            part(cube_, {f * 0.27f + swing * 0.11f, 0.18f + std::fmax(0.0f, swing) * 0.04f, s * 0.10f}, {0.08f, 0.36f, 0.08f}, leg);
        }
    }
}

// ---------- сборка кадра ----------

void Renderer::add(const Mesh& m, Matrix t, Color c, float spec, float sway, bool caster) {
    frame_.push_back({&m, t, c, spec, sway, caster});
}

SnakePose Renderer::pose(const Game& g, float alpha) const {
    const Path path = makePath(g, alpha);
    const Vector2 h = path.at(path.s0);
    const Vector2 f = path.tangent(path.s0, {(float)g.dir().x, (float)g.dir().z});
    return {{wrapCoord(h.x), 0.3f, wrapCoord(h.y)}, {f.x, 0.0f, f.y}};
}

void Renderer::addPickup(Item type, Vector3 pos, float scale, float time) {
    if (scale <= 0.01f) return;
    const float s = scale;
    auto applePart = [&](Color body, float spec) {
        add(sphere_, placed(pos, {0.30f * s, 0.27f * s, 0.30f * s}), body, spec);
        add(cube_, placed({pos.x, pos.y + 0.31f * s, pos.z}, {0.04f * s, 0.16f * s, 0.04f * s}), {96, 62, 34, 255}, 0.0f);
        add(sphere_, placed({pos.x + 0.11f * s, pos.y + 0.34f * s, pos.z}, {0.12f * s, 0.02f * s, 0.06f * s}), {70, 160, 60, 255}, 0.2f);
    };
    switch (type) {
        case Item::Apple:
            applePart({214, 40, 46, 255}, 0.7f);
            break;
        case Item::Golden:
            applePart({255, 196, 36, 255}, 0.9f);
            break;
        case Item::Ice: {  // кристалл: куб, поставленный на угол
            Matrix m = MatrixMultiply(MatrixScale(0.34f * s, 0.34f * s, 0.34f * s), MatrixRotateX(PI / 4.0f));
            m = MatrixMultiply(m, MatrixRotateZ(0.6155f));
            m = MatrixMultiply(m, MatrixRotateY(time * 1.6f));
            add(cube_, MatrixMultiply(m, MatrixTranslate(pos.x, pos.y + 0.08f, pos.z)), {120, 206, 255, 255}, 0.5f);
            break;
        }
        case Item::Star: {
            Matrix m = MatrixMultiply(MatrixScale(0.34f * s, 0.34f * s, 0.34f * s), MatrixRotateY(time * 2.2f));
            add(star_, MatrixMultiply(m, MatrixTranslate(pos.x, pos.y + 0.10f, pos.z)), {255, 220, 60, 255}, 1.2f);
            break;
        }
        case Item::Mushroom: {
            const float y = pos.y - 0.40f;  // гриб стоит на земле, а не парит
            add(sphere_, placed({pos.x, y + 0.18f * s, pos.z}, {0.10f * s, 0.18f * s, 0.10f * s}), {238, 228, 206, 255}, 0.1f);
            add(sphere_, placed({pos.x, y + 0.38f * s, pos.z}, {0.28f * s, 0.15f * s, 0.28f * s}), {168, 72, 204, 255}, 0.5f);
            for (int i = 0; i < 4; ++i) {
                const float a = i * 1.571f + 0.5f;
                add(sphere_, placed({pos.x + std::cos(a) * 0.15f * s, y + 0.47f * s, pos.z + std::sin(a) * 0.15f * s},
                                    {0.045f * s, 0.03f * s, 0.045f * s}),
                    {250, 244, 250, 255}, 0.2f);
            }
            break;
        }
    }
}

void Renderer::addSnake(const Game& g, float alpha, float time, bool dead) {
    const Path path = makePath(g, alpha);
    const Vector2 dirF = {(float)g.dir().x, (float)g.dir().z};
    const float ds = 0.07f;
    const int count = std::max(2, (int)((path.s1 - path.s0) / ds) + 1);
    const int stride = std::max(4, count / std::max(1, kMaxPts - (int)fieldPts_.size() - 1) + 1);

    // цвет шкуры зависит от действующего бонуса
    auto skin = [&](Color c) {
        if (dead) return ColorLerp(c, {120, 120, 120, 255}, 0.65f);
        if (g.doubleLeft() > 0.0f) c = ColorLerp(c, {255, 204, 50, 255}, 0.45f);
        if (g.slowLeft() > 0.0f) c = ColorLerp(c, {110, 190, 255, 255}, 0.40f);
        return c;
    };

    // тело: шарики вдоль сглаженного пути, от хвоста к голове
    for (int k = count - 1; k >= 0; --k) {
        const float t = (float)k / (count - 1);
        const float s = path.s0 + (path.s1 - path.s0) * t;
        Vector2 c = path.at(s);
        // извивание: волна бежит от головы к хвосту
        const Vector2 tan = path.tangent(s, dirF);
        const float u = s - path.s0;
        const float wave = dead ? 0.0f : std::sin(u * 2.6f - time * 7.0f) * 0.055f * smooth01(0.3f, 1.4f, u);
        c = Vector2Add(c, Vector2Scale({-tan.y, tan.x}, wave));
        c = {wrapCoord(c.x), wrapCoord(c.y)};
        const float r = 0.31f * (1.0f - 0.78f * smooth01(0.5f, 1.0f, t));

        // полосы двух зелёных с жёлтой каймой
        const int band = k % 13;
        const Color col = band < 6 ? Color{66, 168, 70, 255} : band < 11 ? Color{34, 112, 54, 255} : Color{214, 210, 92, 255};
        add(sphere_, placed({c.x, r * 0.88f, c.y}, {r, r * 0.88f, r}), skin(col), 0.35f);
        if (k % stride == 0 && (int)fieldPts_.size() < kMaxPts) fieldPts_.push_back(c);
    }

    // голова
    const Vector2 hf = path.tangent(path.s0, dirF);
    const Vector3 fwd = {hf.x, 0, hf.y}, up = {0, 1, 0}, right = {-hf.y, 0, hf.x};
    const Vector2 hc = path.at(path.s0);
    const Vector3 head = Vector3Add({wrapCoord(hc.x), 0.30f, wrapCoord(hc.y)}, Vector3Scale(fwd, 0.14f));
    auto at = [&](float f, float r, float u) {
        return Vector3Add(head, Vector3Add(Vector3Scale(fwd, f), Vector3Add(Vector3Scale(right, r), Vector3Scale(up, u))));
    };
    add(sphere_, fromAxes(Vector3Scale(fwd, 0.46f), Vector3Scale(up, 0.27f), Vector3Scale(right, 0.35f), head),
        skin({78, 186, 76, 255}), 0.4f);
    for (int s = -1; s <= 1; s += 2) {
        add(sphere_, placed(at(0.20f, s * 0.20f, 0.14f), {0.09f, 0.09f, 0.09f}), {245, 245, 235, 255}, 0.8f);
        add(sphere_, placed(at(0.25f, s * 0.235f, 0.155f), {0.05f, 0.055f, 0.05f}), {15, 15, 20, 255}, 1.0f);
        add(sphere_, placed(at(0.43f, s * 0.07f, 0.06f), {0.018f, 0.018f, 0.018f}), {20, 60, 30, 255}, 0.0f);
    }

    // язык высовывается раз в пару секунд
    const float flick = std::fmod(time, 1.9f);
    tongue_.out = !dead && flick < 0.45f;
    if (tongue_.out) {
        const float ext = std::sin(flick / 0.45f * PI) * 0.34f;
        tongue_.root = at(0.42f, 0, -0.06f);
        tongue_.fork = at(0.42f + ext, 0, -0.06f);
        tongue_.left = at(0.42f + ext + 0.11f, -0.07f, -0.05f);
        tongue_.right = at(0.42f + ext + 0.11f, 0.07f, -0.05f);
    }
}

void Renderer::buildFrame(const Game& g, float alpha, float time, bool dead) {
    frame_.clear();
    fieldPts_.clear();
    wildPts_.clear();

    add(cube_, placed({0, -0.1f, 0}, {220, 0.2f, 220}), {66, 112, 48, 255}, 0.0f, 0.0f, false);  // земля
    frame_.insert(frame_.end(), props_.begin(), props_.end());

    // забор; в режимах без границ от него остаются только угловые столбы
    const float edge = kHalf + 0.38f, len = Game::N + 1.06f;
    const Color wood = {150, 104, 60, 255}, post = {112, 76, 44, 255};
    for (int s = -1; s <= 1; s += 2) {
        if (!g.wraps()) {
            add(cube_, placed({0, 0.17f, s * edge}, {len, 0.3f, 0.3f}), wood);
            add(cube_, placed({s * edge, 0.17f, 0}, {0.3f, 0.3f, len}), wood);
        }
        for (int i = -2; i <= 2; ++i) {
            const bool corner = i == -2 || i == 2;
            if (g.wraps() && !corner) continue;
            const float t = i * (edge / 2.0f);
            add(cube_, placed({t, 0.3f, s * edge}, {0.42f, 0.6f, 0.42f}), post);
            if (!corner) add(cube_, placed({s * edge, 0.3f, t}, {0.42f, 0.6f, 0.42f}), post);
        }
    }

    // камни режима «Стены»
    for (Cell w : g.walls()) {
        const Vector2 p = cellPos(w);
        const float tone = 0.88f + 0.06f * ((w.x * 7 + w.z * 13) % 5);
        const Color stone = ColorBrightness({132, 132, 140, 255}, tone - 1.0f);
        add(cube_, placed({p.x, 0.30f, p.y}, {0.92f, 0.62f, 0.92f}), stone, 0.15f);
        add(cube_, placed({p.x, 0.65f, p.y}, {0.74f, 0.10f, 0.74f}), ColorBrightness(stone, 0.12f), 0.15f);
    }

    // яблоки и бонусы
    for (const Pickup& k : g.pickups()) {
        const Vector2 p = cellPos(k.at);
        float scale = smooth01(0.0f, 0.3f, k.age);
        if (k.timed && k.ttl < 2.0f) scale *= 0.78f + 0.22f * std::sin(time * 16.0f);  // скоро исчезнет
        addPickup(k.type, {p.x, 0.40f + 0.05f * std::sin(time * 3.0f + p.x + p.y), p.y}, scale, time);
        if (scale > 0.5f) fieldPts_.push_back(p);
    }
    // съеденное исчезает, когда до него доползает голова
    if (g.ate() && alpha < 0.6f) {
        const Vector2 p = cellPos(g.eatenAt());
        addPickup(g.eatenType(), {p.x, 0.40f, p.y}, 1.0f - smooth01(0.2f, 0.6f, alpha), time);
    }

    addSnake(g, alpha, time, dead);
    for (const Animal& a : animals_) addAnimal(a);
}

// ---------- отрисовка ----------

void Renderer::drawList(bool depthPass) {
    for (const Draw& d : frame_) {
        if (depthPass && !d.caster) continue;
        SetShaderValue(litMat_.shader, locSpec_, &d.spec, SHADER_UNIFORM_FLOAT);
        SetShaderValue(litMat_.shader, locSway_, &d.sway, SHADER_UNIFORM_FLOAT);
        litMat_.maps[MATERIAL_MAP_DIFFUSE].color = d.c;
        DrawMesh(*d.mesh, litMat_, d.m);
    }
}

void Renderer::render(const Game& g, const Camera3D& cam, float alpha, float time, bool dead) {
    buildFrame(g, alpha, time, dead);
    SetShaderValue(litMat_.shader, locLitTime_, &time, SHADER_UNIFORM_FLOAT);

    // ---- проход 1: сцена глазами солнца, в карту теней пишется только глубина ----
    Camera3D sun{};
    sun.position = Vector3Scale(kLightDir, -60.0f);
    sun.target = {0, 0, 0};
    sun.up = {0, 1, 0};
    sun.fovy = 62.0f;  // для ортографической камеры это размер кадра в метрах
    sun.projection = CAMERA_ORTHOGRAPHIC;

    rlActiveTextureSlot(kShadowSlot);  // пока в карту пишем, читать из неё нельзя
    rlDisableTexture();
    rlActiveTextureSlot(0);
    rlSetClipPlanes(1.0, 120.0);
    BeginTextureMode(shadow_);
    ClearBackground(WHITE);
    BeginMode3D(sun);
    const Matrix lightVP = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    drawList(true);
    EndMode3D();
    EndTextureMode();
    rlSetClipPlanes(0.1, 400.0);

    // ---- проход 2: обычная картинка, шейдеры сверяются с картой теней ----
    int slot = kShadowSlot;
    for (const Material* mat : {&litMat_, &grassMat_}) {
        const bool lit = mat == &litMat_;
        SetShaderValueMatrix(mat->shader, lit ? locLitVP_ : locGrassVP_, lightVP);
        rlEnableShader(mat->shader.id);
        rlSetUniform(lit ? locLitMap_ : locGrassMap_, &slot, SHADER_UNIFORM_INT, 1);
    }
    rlActiveTextureSlot(kShadowSlot);
    rlEnableTexture(shadow_.depth.id);
    rlActiveTextureSlot(0);

    SetShaderValue(litMat_.shader, locView_, &cam.position, SHADER_UNIFORM_VEC3);
    SetShaderValue(litMat_.shader, locLight_, &kLightDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(grassMat_.shader, locTime_, &time, SHADER_UNIFORM_FLOAT);

    BeginMode3D(cam);
    drawList(false);

    rlDisableBackfaceCulling();
    auto drawGrass = [&](const Mesh& mesh, const std::vector<Vector2>& pts) {
        const int n = std::min((int)pts.size(), kMaxPts);
        if (n > 0) SetShaderValueV(grassMat_.shader, locPts_, pts.data(), SHADER_UNIFORM_VEC2, n);
        SetShaderValue(grassMat_.shader, locCount_, &n, SHADER_UNIFORM_INT);
        DrawMesh(mesh, grassMat_, MatrixIdentity());
    };
    drawGrass(fieldGrass_, fieldPts_);
    drawGrass(wildGrass_, wildPts_);
    rlEnableBackfaceCulling();

    if (tongue_.out) {
        const Color red = {226, 54, 78, 255};
        DrawCylinderEx(tongue_.root, tongue_.fork, 0.022f, 0.016f, 6, red);
        DrawCylinderEx(tongue_.fork, tongue_.left, 0.014f, 0.004f, 5, red);
        DrawCylinderEx(tongue_.fork, tongue_.right, 0.014f, 0.004f, 5, red);
    }
    EndMode3D();
}
