#pragma once
#include "raylib.h"

// Все модели строятся кодом — игре не нужны внешние файлы.
// Каждая функция возвращает уже загруженный в видеопамять меш.

Mesh genFieldGrass();  // газон игрового поля, клетки в шахматном порядке
Mesh genWildGrass();   // высокая трава за забором

Mesh genOak(unsigned seed);
Mesh genBirch(unsigned seed);
Mesh genPine(unsigned seed);
Mesh genBush(unsigned seed);
Mesh genRock(unsigned seed);
Mesh genStar();  // пятиконечная звезда радиуса 1 в плоскости XY
