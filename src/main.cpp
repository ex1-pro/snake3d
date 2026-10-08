#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "game.hpp"
#include "raylib.h"
#include "raymath.h"
#include "render.hpp"
#include "rlgl.h"

namespace {

enum class Phase { Menu, Playing, Paused, Over };

constexpr int kModes = (int)Mode::Count;

// Системный шрифт с кириллицей; если его нет — встроенный шрифт и английские надписи
struct Ui {
    Font font{};
    bool ru = false;

    Ui() {
        std::vector<int> cps;
        for (int c = 32; c < 127; ++c) cps.push_back(c);
        for (int c = 0x400; c < 0x460; ++c) cps.push_back(c);
        for (int c : {0x2014, 0x00D7, 0x00B7}) cps.push_back(c);  // — × ·
        for (const char* path : {"C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf"}) {
            if (!FileExists(path)) continue;
            font = LoadFontEx(path, 48, cps.data(), (int)cps.size());
            SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
            ru = true;
            return;
        }
        font = GetFontDefault();
    }
    ~Ui() {
        if (ru) UnloadFont(font);
    }

    const char* tr(const char* russian, const char* english) const { return ru ? russian : english; }

    float width(const char* s, float size) const { return MeasureTextEx(font, s, size, 1).x; }
    void text(const char* s, float x, float y, float size, Color c) const {
        DrawTextEx(font, s, {x + 2, y + 2}, size, 1, Fade(BLACK, 0.55f));
        DrawTextEx(font, s, {x, y}, size, 1, c);
    }
    void centered(const char* s, float y, float size, Color c) const {
        text(s, (GetScreenWidth() - width(s, size)) / 2, y, size, c);
    }
};

// Лучший счёт для каждого режима хранится в файле рядом с программой
struct Scores {
    int best[kModes] = {};
    std::string path = std::string(GetApplicationDirectory()) + "snake3d_scores.txt";

    Scores() {
        std::ifstream in(path);
        for (int& b : best)
            if (!(in >> b)) b = 0;
    }
    // true, если это новый рекорд
    bool submit(Mode m, int score) {
        if (score <= best[(int)m]) return false;
        best[(int)m] = score;
        std::ofstream out(path);
        for (int b : best) out << b << ' ';
        return true;
    }
};

const char* modeName(const Ui& ui, Mode m) {
    switch (m) {
        case Mode::Classic: return ui.tr("Классика", "Classic");
        case Mode::Borderless: return ui.tr("Без границ", "Borderless");
        case Mode::Walls: return ui.tr("Стены", "Walls");
        default: return ui.tr("Мирный", "Peaceful");
    }
}

const char* modeHint(const Ui& ui, Mode m) {
    switch (m) {
        case Mode::Classic: return ui.tr("Забор и собственный хвост смертельны", "The fence and your own tail are deadly");
        case Mode::Borderless: return ui.tr("Вышел за край — появился с другой стороны", "Leave one side, come back on the other");
        case Mode::Walls: return ui.tr("После каждого яблока на поле вырастает камень", "Every apple adds a stone to the field");
        default: return ui.tr("Умереть нельзя — играй в своё удовольствие", "You cannot die, just enjoy the garden");
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Служебные ключи для проверки картинки:
    // --shot <файл.png> сыграть пару секунд, сохранить кадр и выйти;
    // --chase, --mode <n>, --menu, --populate уточняют, что именно снять
    const char* shotName = nullptr;
    bool overview = true, shotMenu = false, populate = false;
    Settings cfg;
    for (int i = 1; i < argc; ++i) {
        if (TextIsEqual(argv[i], "--shot") && i + 1 < argc) shotName = argv[++i];
        else if (TextIsEqual(argv[i], "--chase")) overview = false;
        else if (TextIsEqual(argv[i], "--menu")) shotMenu = true;
        else if (TextIsEqual(argv[i], "--populate")) populate = true;
        else if (TextIsEqual(argv[i], "--mode") && i + 1 < argc) cfg.mode = (Mode)Clamp((float)TextToInteger(argv[++i]), 0, kModes - 1);
    }

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 800, "Snake 3D");
    SetWindowMinSize(960, 600);
    SetTargetFPS(60);
    {
        Renderer renderer;
        Ui ui;
        Scores scores;
        Game game;
        game.reset(cfg);
        Phase phase = Phase::Menu;
        float acc = 0.0f, animTime = 0.0f, orbit = 0.6f;
        Vector3 camPos{}, camTarget{}, lastHead{};
        bool camSnap = true, newRecord = false;
        int frame = 0, menuRow = 0;

        auto start = [&] {
            game.reset(cfg);
            if (populate) game.debugPopulate();
            game.step();  // первый тик сразу: змейка трогается с места без рывка
            acc = 0.0f;
            newRecord = false;
            phase = Phase::Playing;
        };
        auto toMenu = [&] {
            scores.submit(cfg.mode, game.score());
            game.reset(cfg);
            phase = Phase::Menu;
        };

        while (!WindowShouldClose()) {
            const float dt = std::fmin(GetFrameTime(), 0.05f);
            ++frame;

            // ---- ввод ----
            const bool l = IsKeyPressed(KEY_A) || IsKeyPressed(KEY_LEFT);
            const bool r = IsKeyPressed(KEY_D) || IsKeyPressed(KEY_RIGHT);
            const bool u = IsKeyPressed(KEY_W) || IsKeyPressed(KEY_UP);
            const bool d = IsKeyPressed(KEY_S) || IsKeyPressed(KEY_DOWN);
            const bool go = IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER);

            if (phase == Phase::Menu) {
                if (u) menuRow = (menuRow + 2) % 3;
                if (d) menuRow = (menuRow + 1) % 3;
                const int step = (r ? 1 : 0) - (l ? 1 : 0);
                if (step != 0) {
                    if (menuRow == 0) cfg.mode = (Mode)(((int)cfg.mode + step + kModes) % kModes);
                    if (menuRow == 1) cfg.apples = cfg.apples == 1 ? (step > 0 ? 3 : 5) : cfg.apples == 3 ? (step > 0 ? 5 : 1) : (step > 0 ? 1 : 3);
                    if (menuRow == 2) cfg.speed = (cfg.speed + step + 3) % 3;
                    game.reset(cfg);  // поле за меню сразу показывает выбранный режим
                }
                if (go) start();
            } else if (phase == Phase::Playing) {
                if (overview) {  // вид сверху: клавиши = стороны экрана
                    if (l) game.queueDir({-1, 0});
                    if (r) game.queueDir({1, 0});
                    if (u) game.queueDir({0, -1});
                    if (d) game.queueDir({0, 1});
                } else {  // камера за змейкой: поворот относительно неё
                    if (l) game.queueTurn(-1);
                    if (r) game.queueTurn(1);
                }
                if (go || (!IsWindowFocused() && !shotName)) phase = Phase::Paused;
            } else if (phase == Phase::Paused) {
                if (go) phase = Phase::Playing;
                if (IsKeyPressed(KEY_M)) toMenu();
            } else {
                if (go) start();
                if (IsKeyPressed(KEY_M)) toMenu();
            }
            if (IsKeyPressed(KEY_C)) overview = !overview;

            if (shotName && !shotMenu) {
                if (frame == 2) start();
                if (frame == 45) game.queueDir({0, 1});
                if (frame == 85) game.queueDir({-1, 0});
            }

            // ---- обновление ----
            if (phase == Phase::Playing) {
                game.update(dt);
                acc += dt;
                while (acc >= game.stepDelay()) {
                    acc -= game.stepDelay();
                    game.step();
                    if (!game.alive()) {
                        newRecord = scores.submit(cfg.mode, game.score());
                        phase = Phase::Over;
                        break;
                    }
                }
            }
            if (phase == Phase::Playing || phase == Phase::Menu) animTime += dt;
            renderer.update(dt);
            orbit += dt * 0.2f;

            const bool moving = phase == Phase::Playing || phase == Phase::Paused;
            const float alpha = moving ? Clamp(acc / game.stepDelay(), 0.0f, 1.0f) : 1.0f;
            const SnakePose pose = renderer.pose(game, alpha);

            // ---- камера ----
            Vector3 wantPos, wantTarget;
            if (phase == Phase::Menu) {
                wantPos = {std::cos(orbit) * 20.0f, 11.0f, std::sin(orbit) * 20.0f};
                wantTarget = {0, 0, 0};
            } else if (overview) {
                wantTarget = {pose.head.x * 0.10f, 0.0f, 1.3f + pose.head.z * 0.10f};
                wantPos = {wantTarget.x, 17.0f, wantTarget.z + 12.9f};
            } else {
                wantPos = Vector3Add(Vector3Subtract(pose.head, Vector3Scale(pose.forward, 4.6f)), {0, 3.1f, 0});
                wantTarget = Vector3Add(pose.head, Vector3Scale(pose.forward, 2.6f));
                // змейка вынырнула с другого края поля — камера переносится сразу, а не летит через всё поле
                if (Vector3Distance(pose.head, lastHead) > 4.0f) camSnap = true;
            }
            lastHead = pose.head;
            const float k = camSnap ? 1.0f : 1.0f - std::exp(-5.0f * dt);
            camSnap = false;
            camPos = Vector3Lerp(camPos, wantPos, k);
            camTarget = Vector3Lerp(camTarget, wantTarget, k);

            Camera3D camera{};
            camera.position = camPos;
            camera.target = camTarget;
            camera.up = {0, 1, 0};
            camera.fovy = 45.0f;
            camera.projection = CAMERA_PERSPECTIVE;

            // ---- отрисовка ----
            BeginDrawing();
            ClearBackground({150, 200, 240, 255});
            const int sw = GetScreenWidth(), sh = GetScreenHeight();
            DrawRectangleGradientV(0, 0, sw, sh, {104, 168, 232, 255}, {206, 230, 244, 255});
            renderer.render(game, camera, alpha, animTime, phase == Phase::Over);

            // ---- интерфейс ----
            const Color soft = {220, 235, 220, 255}, gold = {255, 214, 80, 255}, ice = {150, 214, 255, 255};
            if (phase != Phase::Menu) {
                DrawRectangleRounded({14, 12, 300, 132}, 0.18f, 8, Fade(BLACK, 0.38f));
                ui.text(TextFormat(ui.tr("Счёт: %d", "Score: %d"), game.score()), 28, 18, 36, RAYWHITE);
                ui.text(TextFormat(ui.tr("Длина: %d   Рекорд: %d", "Length: %d   Best: %d"), (int)game.snake().size(),
                                   scores.best[(int)cfg.mode]),
                        28, 60, 22, soft);
                const int toRound = Game::kApplesPerRound - game.applesEaten() % Game::kApplesPerRound;
                ui.text(TextFormat(ui.tr("%s · до бонусного раунда: %d", "%s · bonus round in: %d"), modeName(ui, cfg.mode), toRound),
                        28, 90, 20, soft);
                ui.text(TextFormat(ui.tr("Яблок съедено: %d", "Apples eaten: %d"), game.applesEaten()), 28, 114, 20, soft);

                // действующие бонусы — справа сверху
                float y = 14;
                auto pill = [&](const char* label, float left, Color c) {
                    const char* s = TextFormat("%s  %.1f", label, left);
                    const float w = ui.width(s, 26) + 32;
                    DrawRectangleRounded({sw - w - 14, y, w, 40}, 0.5f, 8, Fade(BLACK, 0.42f));
                    ui.text(s, sw - w + 2, y + 7, 26, c);
                    y += 48;
                };
                if (game.doubleLeft() > 0) pill(ui.tr("Очки ×2", "Score x2"), game.doubleLeft(), gold);
                if (game.slowLeft() > 0) pill(ui.tr("Замедление", "Slow motion"), game.slowLeft(), ice);
                if (game.roundLeft() > 0 && phase == Phase::Playing) {
                    const float pulse = 1.0f + 0.05f * std::sin(animTime * 9.0f);
                    ui.centered(TextFormat(ui.tr("БОНУСНЫЙ РАУНД!  %.1f", "BONUS ROUND!  %.1f"), game.roundLeft()), 20, 46 * pulse, gold);
                }

                ui.text(overview ? ui.tr("Стрелки / WASD — движение    C — камера    Пробел — пауза",
                                         "Arrows / WASD: move    C: camera    SPACE: pause")
                                 : ui.tr("A/D или влево/вправо — поворот    C — камера    Пробел — пауза",
                                         "A/D or Left/Right: turn    C: camera    SPACE: pause"),
                        18, sh - 34.0f, 22, {240, 245, 240, 255});
            }

            const float cy = sh / 2.0f;
            if (phase == Phase::Menu) {
                const float pw = 760, ph = 520, px = (sw - pw) / 2, py = (sh - ph) / 2;
                DrawRectangleRounded({px, py, pw, ph}, 0.06f, 12, Fade(BLACK, 0.58f));
                ui.centered(ui.tr("ЗМЕЙКА 3D", "SNAKE 3D"), py + 22, 72, {130, 235, 120, 255});

                const char* speeds[3] = {ui.tr("Медленно", "Slow"), ui.tr("Нормально", "Normal"), ui.tr("Быстро", "Fast")};
                const char* labels[3] = {ui.tr("Режим", "Mode"), ui.tr("Яблок на поле", "Apples"), ui.tr("Скорость", "Speed")};
                const char* values[3] = {modeName(ui, cfg.mode), TextFormat("%d", cfg.apples), speeds[cfg.speed]};
                for (int i = 0; i < 3; ++i) {
                    const float ry = py + 124 + i * 56.0f;
                    const bool on = i == menuRow;
                    if (on) DrawRectangleRounded({px + 40, ry - 6, pw - 80, 48}, 0.3f, 8, Fade({130, 235, 120, 255}, 0.22f));
                    ui.text(labels[i], px + 64, ry, 32, on ? RAYWHITE : soft);
                    const char* v = TextFormat(on ? "<  %s  >" : "%s", values[i]);
                    ui.text(v, px + pw - 64 - ui.width(v, 32), ry, 32, on ? gold : RAYWHITE);
                }
                ui.centered(modeHint(ui, cfg.mode), py + 298, 24, soft);
                ui.centered(TextFormat(ui.tr("Рекорд в этом режиме: %d", "Best in this mode: %d"), scores.best[(int)cfg.mode]), py + 330, 24, gold);

                ui.centered(ui.tr("Бонусы: золотое яблоко +30 · льдинка замедляет", "Bonuses: golden apple +30 · ice slows time"),
                            py + 376, 21, soft);
                ui.centered(ui.tr("звезда удваивает очки · гриб укорачивает змейку", "star doubles the score · mushroom shrinks the snake"),
                            py + 402, 21, soft);
                ui.centered(ui.tr("Каждые 8 яблок — бонусный раунд с россыпью золотых", "Every 8 apples: a bonus round of golden apples"),
                            py + 428, 21, soft);
                ui.centered(ui.tr("Вверх/вниз — выбор    Влево/вправо — изменить    Пробел — играть",
                                  "Up/Down: select    Left/Right: change    SPACE: play"),
                            py + 472, 22, RAYWHITE);
            } else if (phase == Phase::Paused) {
                DrawRectangle(0, (int)cy - 100, sw, 200, Fade(BLACK, 0.45f));
                ui.centered(ui.tr("ПАУЗА", "PAUSED"), cy - 78, 72, RAYWHITE);
                ui.centered(ui.tr("Пробел — продолжить    M — меню", "SPACE: continue    M: menu"), cy + 26, 30, LIGHTGRAY);
            } else if (phase == Phase::Over) {
                DrawRectangle(0, (int)cy - 110, sw, 220, Fade(BLACK, 0.45f));
                ui.centered(ui.tr("ИГРА ОКОНЧЕНА", "GAME OVER"), cy - 98, 64, {255, 120, 120, 255});
                ui.centered(TextFormat(ui.tr("Счёт: %d", "Score: %d"), game.score()), cy - 26, 36, RAYWHITE);
                if (newRecord) ui.centered(ui.tr("Новый рекорд!", "New record!"), cy + 16, 30, gold);
                ui.centered(ui.tr("Пробел — ещё раз    M — меню", "SPACE: play again    M: menu"), cy + 60, 28, LIGHTGRAY);
            }

            if (shotName && frame == (shotMenu ? 40 : 125)) {
                rlDrawRenderBatchActive();
                TakeScreenshot(shotName);
                EndDrawing();
                break;
            }
            EndDrawing();
        }
    }
    CloseWindow();
    return 0;
}
