#include "game.hpp"

#include <algorithm>
#include <cstdlib>

Game::Game() : rng_(std::random_device{}()) { reset({}); }

void Game::reset(const Settings& s) {
    cfg_ = s;
    const int c = N / 2;
    snake_ = {{c, c}, {c - 1, c}, {c - 2, c}};
    prevTail_ = snake_.back();
    turns_.clear();
    pickups_.clear();
    walls_.clear();
    dir_ = {1, 0};
    alive_ = true;
    ate_ = false;
    score_ = eaten_ = 0;
    slow_ = double_ = round_ = 0.0f;
    bonusTimer_ = 7.0f;
    for (int i = 0; i < cfg_.apples; ++i) spawn(Item::Apple);
}

void Game::queueDir(Cell d) {
    const Cell last = turns_.empty() ? dir_ : turns_.back();
    if (d == last || d == -last) return;  // разворот на 180° запрещён
    if (turns_.size() < 3) turns_.push_back(d);
}

void Game::queueTurn(int side) {
    const Cell last = turns_.empty() ? dir_ : turns_.back();
    const Cell right = {-last.z, last.x};
    queueDir(side > 0 ? right : -right);
}

float Game::stepDelay() const {
    static const float base[3] = {0.21f, 0.16f, 0.115f};
    const float b = base[std::clamp(cfg_.speed, 0, 2)];
    const float d = std::max(b * 0.55f, b - 0.003f * eaten_);
    return slow_ > 0.0f ? d * 1.6f : d;
}

bool Game::blocked(Cell p) const {
    if (std::find(snake_.begin(), snake_.end(), p) != snake_.end()) return true;
    if (std::find(walls_.begin(), walls_.end(), p) != walls_.end()) return true;
    return std::any_of(pickups_.begin(), pickups_.end(), [&](const Pickup& k) { return k.at == p; });
}

bool Game::freeCell(Cell& out, int minHeadDist) {
    std::vector<Cell> free;
    const Cell h = snake_.front();
    for (int z = 0; z < N; ++z)
        for (int x = 0; x < N; ++x) {
            const Cell c = {x, z};
            if (std::abs(x - h.x) + std::abs(z - h.z) < minHeadDist) continue;
            if (!blocked(c)) free.push_back(c);
        }
    if (free.empty()) return false;
    out = free[std::uniform_int_distribution<size_t>(0, free.size() - 1)(rng_)];
    return true;
}

bool Game::spawn(Item type, float ttl) {
    Cell at;
    if (!freeCell(at, 0)) return false;
    pickups_.push_back({at, type, ttl > 0.0f, ttl, 0.0f});
    return true;
}

// Камень не появляется вплотную к голове, чтобы от него можно было увернуться
void Game::addWall() {
    Cell at;
    if (freeCell(at, 4)) walls_.push_back(at);
}

void Game::debugPopulate() {
    spawn(Item::Golden, 60.0f);
    spawn(Item::Ice, 60.0f);
    spawn(Item::Star, 60.0f);
    spawn(Item::Mushroom, 60.0f);
    for (int i = 0; i < 4; ++i) addWall();
}

void Game::update(float dt) {
    slow_ = std::max(0.0f, slow_ - dt);
    double_ = std::max(0.0f, double_ - dt);
    round_ = std::max(0.0f, round_ - dt);

    for (Pickup& p : pickups_) {
        p.age += dt;
        if (p.timed) p.ttl -= dt;
    }
    pickups_.erase(std::remove_if(pickups_.begin(), pickups_.end(), [](const Pickup& p) { return p.timed && p.ttl <= 0.0f; }),
                   pickups_.end());

    // раз в 8–13 секунд на поле появляется случайный бонус
    bonusTimer_ -= dt;
    const bool bonusOnField = std::any_of(pickups_.begin(), pickups_.end(), [](const Pickup& p) { return p.timed; });
    if (bonusTimer_ <= 0.0f && !bonusOnField && round_ <= 0.0f) {
        const int roll = std::uniform_int_distribution<int>(0, 99)(rng_);
        Item type = roll < 40 ? Item::Golden : roll < 62 ? Item::Ice : roll < 85 ? Item::Star : Item::Mushroom;
        if (type == Item::Mushroom && snake_.size() < 7) type = Item::Golden;
        spawn(type, 7.0f);
        bonusTimer_ = std::uniform_real_distribution<float>(8.0f, 13.0f)(rng_);
    }
}

void Game::step() {
    if (!alive_) return;
    if (!turns_.empty()) {
        dir_ = turns_.front();
        turns_.pop_front();
    }

    Cell head = snake_.front() + dir_;
    if (wraps()) {
        head = {(head.x + N) % N, (head.z + N) % N};
    } else if (head.x < 0 || head.z < 0 || head.x >= N || head.z >= N) {
        alive_ = false;
        return;
    }
    if (std::find(walls_.begin(), walls_.end(), head) != walls_.end()) {
        alive_ = false;
        return;
    }

    const auto pick = std::find_if(pickups_.begin(), pickups_.end(), [&](const Pickup& p) { return p.at == head; });
    const bool found = pick != pickups_.end();
    const bool grows = found && (pick->type == Item::Apple || pick->type == Item::Golden);

    if (cfg_.mode != Mode::Peaceful) {
        // хвост в этот тик освободится, если змейка не растёт
        const auto bodyEnd = grows ? snake_.end() : snake_.end() - 1;
        if (std::find(snake_.begin(), bodyEnd, head) != bodyEnd) {
            alive_ = false;
            return;
        }
    }

    snake_.push_front(head);
    if (grows) {
        prevTail_ = snake_.back();  // хвост остаётся на месте
    } else {
        prevTail_ = snake_.back();
        snake_.pop_back();
    }

    ate_ = found;
    if (!found) return;

    const Item type = pick->type;
    eatenAt_ = head;
    eatenType_ = type;
    pickups_.erase(pick);
    const int mult = double_ > 0.0f ? 2 : 1;

    switch (type) {
        case Item::Apple:
            ++eaten_;
            score_ += 10 * mult;
            spawn(Item::Apple);
            if (cfg_.mode == Mode::Walls) addWall();
            if (eaten_ % kApplesPerRound == 0) {  // бонусный раунд: россыпь золотых яблок
                round_ = 8.0f;
                for (int i = 0; i < 6; ++i) spawn(Item::Golden, round_);
            }
            break;
        case Item::Golden:
            score_ += 30 * mult;
            break;
        case Item::Ice:
            slow_ = 6.0f;
            break;
        case Item::Star:
            double_ = 8.0f;
            break;
        case Item::Mushroom:
            for (int i = 0; i < 3 && snake_.size() > 3; ++i) snake_.pop_back();
            prevTail_ = snake_.back();
            break;
    }
}
