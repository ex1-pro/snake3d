#pragma once
#include <deque>
#include <random>
#include <vector>

// Клетка поля: змейка движется по плоскости XZ
struct Cell {
    int x = 0, z = 0;
};

inline bool operator==(Cell a, Cell b) { return a.x == b.x && a.z == b.z; }
inline Cell operator+(Cell a, Cell b) { return {a.x + b.x, a.z + b.z}; }
inline Cell operator-(Cell a) { return {-a.x, -a.z}; }

enum class Mode {
    Classic,     // стены убивают
    Borderless,  // вышел за край — появился с другой стороны
    Walls,       // после каждого яблока на поле вырастает камень
    Peaceful,    // умереть нельзя
    Count
};

enum class Item {
    Apple,     // +10, змейка растёт
    Golden,    // +30, живёт недолго
    Ice,       // замедляет игру
    Star,      // удваивает очки
    Mushroom,  // укорачивает змейку
};

struct Settings {
    Mode mode = Mode::Classic;
    int apples = 1;  // сколько яблок лежит одновременно: 1, 3 или 5
    int speed = 1;   // 0 медленно, 1 нормально, 2 быстро
};

struct Pickup {
    Cell at;
    Item type = Item::Apple;
    bool timed = false;  // исчезает, когда ttl дойдёт до нуля
    float ttl = 0.0f;
    float age = 0.0f;  // секунд с появления (для анимации)
};

class Game {
public:
    static constexpr int N = 16;                // сторона поля
    static constexpr int kApplesPerRound = 8;   // столько яблок до бонусного раунда

    Game();
    void reset(const Settings& s);
    void queueDir(Cell d);     // направление в координатах поля
    void queueTurn(int side);  // -1 влево, +1 вправо относительно движения
    void step();               // один игровой тик
    void update(float dt);     // таймеры бонусов; вызывать, пока идёт игра

    float stepDelay() const;  // секунд между тиками

    const Settings& settings() const { return cfg_; }
    bool wraps() const { return cfg_.mode == Mode::Borderless || cfg_.mode == Mode::Peaceful; }

    const std::deque<Cell>& snake() const { return snake_; }
    Cell prevTail() const { return prevTail_; }  // где был хвост до последнего тика
    Cell dir() const { return dir_; }
    bool alive() const { return alive_; }
    int score() const { return score_; }
    int applesEaten() const { return eaten_; }

    const std::vector<Pickup>& pickups() const { return pickups_; }
    const std::vector<Cell>& walls() const { return walls_; }

    bool ate() const { return ate_; }  // подобрала ли что-то на последнем тике
    Cell eatenAt() const { return eatenAt_; }
    Item eatenType() const { return eatenType_; }

    float slowLeft() const { return slow_; }
    float doubleLeft() const { return double_; }
    float roundLeft() const { return round_; }  // идёт бонусный раунд

    void debugPopulate();  // по одному бонусу каждого вида и пара камней — для снимков

private:
    bool blocked(Cell p) const;
    bool freeCell(Cell& out, int minHeadDist);
    bool spawn(Item type, float ttl = 0.0f);
    void addWall();

    Settings cfg_;
    std::deque<Cell> snake_;  // snake_.front() — голова
    std::deque<Cell> turns_;
    std::vector<Pickup> pickups_;
    std::vector<Cell> walls_;
    Cell dir_, prevTail_, eatenAt_;
    Item eatenType_ = Item::Apple;
    bool alive_ = true, ate_ = false;
    int score_ = 0, eaten_ = 0;
    float slow_ = 0, double_ = 0, round_ = 0, bonusTimer_ = 0;
    std::mt19937 rng_;
};
