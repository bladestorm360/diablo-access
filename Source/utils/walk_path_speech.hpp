/**
 * @file utils/walk_path_speech.hpp
 *
 * Walk-path helpers, PosOk variants, and BFS pathfinding for accessibility speech.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <function_ref.hpp>

#include "engine/displacement.hpp"
#include "engine/point.hpp"

namespace devilution {

struct Player;

// Walk direction helpers
Point NextPositionForWalkDirection(Point position, int8_t walkDir);
Point PositionAfterWalkPathSteps(Point start, const int8_t *path, int steps);
int8_t OppositeWalkDirection(int8_t walkDir);

// PosOk variants for pathfinding
bool PosOkPlayerIgnoreDoors(const Player &player, Point position);
bool IsTileWalkableForTrackerPath(Point position, bool ignoreDoors, bool ignoreBreakables);
bool PosOkPlayerIgnoreMonsters(const Player &player, Point position);
bool PosOkPlayerIgnoreDoorsAndMonsters(const Player &player, Point position);
bool PosOkPlayerIgnoreDoorsMonstersAndBreakables(const Player &player, Point position);

// BFS pathfinding for speech
std::optional<std::vector<int8_t>> FindKeyboardWalkPathForSpeech(const Player &player, Point startPosition, Point destinationPosition, bool allowDestinationNonWalkable = false);
std::optional<std::vector<int8_t>> FindKeyboardWalkPathToClosestReachableForSpeech(const Player &player, Point startPosition, Point destinationPosition, Point &closestPosition);

/**
 * Shortest walk from `startPosition` to the nearest tile where `isGoal` holds, over tiles `posOk`
 * accepts. With `preferAxisSteps` a route of axis steps alone wins when there is one, as spoken
 * directions want. Steps head for `heading` when routes are equally long.
 */
std::optional<std::vector<int8_t>> FindWalkPathToGoal(const Player &player, Point startPosition, Point heading, tl::function_ref<bool(Point)> isGoal, bool (*posOk)(const Player &, Point), bool preferAxisSteps);

// Speech formatting
void AppendKeyboardWalkPathForSpeech(std::string &message, const std::vector<int8_t> &path);
void AppendDirectionalFallback(std::string &message, const Displacement &delta);

} // namespace devilution
