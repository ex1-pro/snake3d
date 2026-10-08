#pragma once
#include <random>
#include <vector>

#include "game.hpp"
#include "raylib.h"

// Где сейчас голова на экране и куда смотрит (нужно камере)
struct SnakePose {
    Vector3 head;
    Vector3 forward;
};

// Вся 3D-отрисовка: трава, забор, деревья, животные, бонусы, змейка и тени.
// Создавать после InitWindow, уничтожать до CloseWindow.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void update(float dt);  // животные за забором живут своей жизнью

    // alpha — какая доля текущего тика уже прошла (0..1): по ней змейка
    // плавно перетекает из прошлой клетки в текущую
    SnakePose pose(const Game& g, float alpha) const;

    // Рисует всю сцену. Вызывать внутри BeginDrawing, но вне BeginMode3D
    void render(const Game& g, const Camera3D& cam, float alpha, float time, bool dead);

private:
    struct Draw {
        const Mesh* mesh;
        Matrix m;
        Color c;
        float spec;   // сила блика
        float sway;   // качание на ветру (для деревьев)
        bool caster;  // отбрасывает ли тень
    };
    struct Animal {
        bool fox = false;
        float theta = 0, d = 11, wantD = 11;  // положение на кольце вокруг поля
        float yaw = 0, phase = 0, timer = 0, speed = 2;
        int sign = 1;
        bool moving = false;
        Color coat{};
    };
    struct Tongue {
        bool out = false;
        Vector3 root{}, fork{}, left{}, right{};
    };

    void add(const Mesh& m, Matrix t, Color c, float spec = 0.1f, float sway = 0.0f, bool caster = true);
    void buildScenery();
    void buildFrame(const Game& g, float alpha, float time, bool dead);
    void addSnake(const Game& g, float alpha, float time, bool dead);
    void addPickup(Item type, Vector3 pos, float scale, float time);
    void addAnimal(const Animal& a);
    void drawList(bool depthPass);

    Mesh sphere_{}, cube_{}, star_{}, fieldGrass_{}, wildGrass_{};
    std::vector<Mesh> trees_, bushes_, rocks_;
    std::vector<float> treeSway_;
    std::vector<Draw> props_;  // неподвижные деревья, кусты, камни
    std::vector<Draw> frame_;  // всё, что рисуется в этом кадре
    std::vector<Animal> animals_;
    std::vector<Vector2> fieldPts_, wildPts_;  // кто приминает траву
    Tongue tongue_;
    std::mt19937 rng_{2026};

    Material litMat_{}, grassMat_{};
    RenderTexture2D shadow_{};
    int locView_ = -1, locLight_ = -1, locSpec_ = -1, locSway_ = -1, locLitTime_ = -1, locLitVP_ = -1, locLitMap_ = -1;
    int locTime_ = -1, locPts_ = -1, locCount_ = -1, locGrassVP_ = -1, locGrassMap_ = -1;
};
