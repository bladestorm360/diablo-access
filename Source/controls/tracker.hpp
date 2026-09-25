/**
 * @file controls/tracker.hpp
 *
 * Tracker system for accessibility: target cycling, pathfinding, and auto-walk.
 */
#pragma once

#include <cstdint>

#ifdef BUILD_TESTING
#include <optional>
#include <string>
#include <vector>

#include "engine/point.hpp"
#endif

namespace devilution {

enum class TrackerTargetCategory : uint8_t {
	Items,
	Chests,
	Doors,
	Shrines,
	Objects,
	Breakables,
	Monsters,
	DeadBodies,
	Npcs,
	Players,
	DungeonEntrances,
	Stairs,
	QuestLocations,
	Portals,
};

void TrackerPageUpKeyPressed();
void TrackerPageDownKeyPressed();
void TrackerHomeKeyPressed();
void UpdateAutoWalkTracker();
void ResetAutoWalkTracker();

#ifdef BUILD_TESTING
/** A target as PageUp and PageDown list it. */
struct TrackerTargetForTesting {
	TrackerTargetCategory category;
	int id;
	std::string name;
};

/** Every target the tracker lists on the current level, in every category. */
std::vector<TrackerTargetForTesting> ListTrackerTargetsForTesting();

/** What Home says with this target selected. */
std::string DescribeTrackerTargetForTesting(TrackerTargetCategory category, int id);

/** One step of Shift+Home: where the player is sent next, or nothing when the walk ends and `stopMessage` says why. */
std::optional<Point> PlanAutoWalkStepForTesting(TrackerTargetCategory category, int id, bool starting, std::string &stopMessage);
#endif

} // namespace devilution
