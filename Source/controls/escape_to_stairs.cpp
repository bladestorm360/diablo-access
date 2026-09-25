/**
 * @file controls/escape_to_stairs.cpp
 *
 * Ctrl+R: drop everything and walk to the nearest way up, keeping away from monsters.
 *
 * A blind player cannot use rooms and corners to break up a swarm, so the way out of one is the
 * stairs. The route is planned again at every step, since the monsters move too, and the engine is
 * only ever given the next tile: its own path search goes by distance alone and would cut past them.
 */
#include "controls/escape_to_stairs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "controls/accessibility_keys.hpp"
#include "controls/plrctrls.h"
#include "controls/tracker.hpp"
#include "engine/path.h"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "monster.h"
#include "msg.h"
#include "objects.h"
#include "player.h"
#include "utils/is_of.hpp"
#include "utils/language.h"
#include "utils/navigation_speech.hpp"
#include "utils/screen_reader.hpp"
#include "utils/walk_path_speech.hpp"

namespace devilution {

namespace {

/*
 * A step costs 10, plus more for every monster near the tile it lands on. A monster next to the
 * tile adds 80, so the route walks up to eight steps further to pass it at a distance, and a
 * monster two or three tiles away adds less.
 */
constexpr int StepCost = 10;
constexpr std::array<int, 4> DangerByDistance { 0, 80, 25, 8 };
/** Opening a closed door on the way costs a moment. */
constexpr int ClosedDoorCost = 30;
constexpr int Unreached = std::numeric_limits<int>::max();

/** The trigger being run to, or -1 when there is no escape under way. */
int EscapeTrigger = -1;
int EscapeLevelKey = 0;
std::optional<Point> LastWaypoint;
/** A door the escape is opening. If it is still shut once the player is done, it counts as a wall. */
std::optional<Point> DoorBeingOpened;
std::vector<Point> DoorsThatStayShut;
bool SaidBlocked = false;

std::array<std::array<int, MAXDUNY>, MAXDUNX> Danger;
std::array<std::array<int, MAXDUNY>, MAXDUNX> RouteCost;
/** The PathDirs index of the step that reached each tile. */
std::array<std::array<int8_t, MAXDUNY>, MAXDUNX> RouteStep;

/** Tells levels apart: taking the stairs changes it, which ends the escape. */
int CurrentLevelKey()
{
	return setlevel ? -1 - static_cast<int>(setlvlnum) : currlevel;
}

bool IsWayUp(const TriggerStruct &trigger)
{
	if (setlevel)
		return trigger._tmsg == WM_DIABRTNLVL;
	return IsAnyOf(trigger._tmsg, WM_DIABPREVLVL, WM_DIABTWARPUP);
}

std::optional<int> TriggerAt(Point position)
{
	for (int i = 0; i < numtrigs; i++) {
		if (trigs[i].position == position)
			return i;
	}
	return std::nullopt;
}

bool IsClosedDoor(Point position)
{
	const Object *object = FindObjectAtPosition(position);
	return object != nullptr && object->isDoor() && object->_oVar4 == DOOR_CLOSED;
}

void MapDanger()
{
	for (auto &column : Danger)
		column.fill(0);

	const int reach = static_cast<int>(DangerByDistance.size());
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (monster.isInvalid || monster.hasNoLife() || monster.isPlayerMinion() || CanTalkToMonst(monster))
			continue;
		const Point tile = monster.position.tile;
		const Point future = monster.position.future;
		for (int dx = -reach; dx <= reach; dx++) {
			for (int dy = -reach; dy <= reach; dy++) {
				const Point position = tile + Displacement { dx, dy };
				if (!InDungeonBounds(position))
					continue;
				// Measured from where the monster stands or from the tile it is stepping onto, whichever is closer.
				const int distance = std::min(position.WalkingDistance(tile), position.WalkingDistance(future));
				if (distance > 0 && distance < reach)
					Danger[position.x][position.y] += DangerByDistance[distance];
			}
		}
	}
}

/**
 * Whether the route may step onto a tile. Every trigger but a way up is a wall: a player who stopped
 * on the stairs down, blocked by a monster, would go down them.
 */
bool CanRouteOnto(const Player &player, Point position, bool ignoreMonsters)
{
	if (const std::optional<int> trigger = TriggerAt(position))
		return IsWayUp(trigs[*trigger]);
	if (std::find(DoorsThatStayShut.begin(), DoorsThatStayShut.end(), position) != DoorsThatStayShut.end())
		return false;
	// Closed doors are opened on the way.
	return ignoreMonsters ? PosOkPlayerIgnoreDoorsAndMonsters(player, position) : PosOkPlayerIgnoreDoors(player, position);
}

/** Costs every tile the player can reach from `start`, on the cheapest route there. */
void MapRoutes(const Player &player, Point start, bool ignoreMonsters)
{
	for (auto &column : RouteCost)
		column.fill(Unreached);

	struct Frontier {
		int cost;
		Point position;
		bool operator>(const Frontier &other) const { return cost > other.cost; }
	};
	std::priority_queue<Frontier, std::vector<Frontier>, std::greater<>> frontier;
	RouteCost[start.x][start.y] = 0;
	frontier.push({ 0, start });

	while (!frontier.empty()) {
		const Frontier node = frontier.top();
		frontier.pop();
		if (node.cost != RouteCost[node.position.x][node.position.y])
			continue; // A cheaper way here was found after this one was queued.
		if (node.position != start && TriggerAt(node.position))
			continue; // Stepping onto a way up leaves the level: nothing lies beyond it.

		for (int8_t dir = 0; dir < 8; dir++) {
			const Point next = node.position + PathDirs[dir];
			if (!InDungeonBounds(next) || !CanRouteOnto(player, next, ignoreMonsters) || !CanStep(node.position, next))
				continue;
			int cost = node.cost + StepCost + Danger[next.x][next.y];
			if (IsClosedDoor(next))
				cost += ClosedDoorCost;
			if (cost >= RouteCost[next.x][next.y])
				continue;
			RouteCost[next.x][next.y] = cost;
			RouteStep[next.x][next.y] = dir;
			frontier.push({ cost, next });
		}
	}
}

/** The way up with the cheapest route, after MapRoutes. */
std::optional<int> CheapestWayUp()
{
	std::optional<int> best;
	for (int i = 0; i < numtrigs; i++) {
		const Point position = trigs[i].position;
		if (!IsWayUp(trigs[i]) || RouteCost[position.x][position.y] == Unreached)
			continue;
		if (!best || RouteCost[position.x][position.y] < RouteCost[trigs[*best].position.x][trigs[*best].position.y])
			best = i;
	}
	return best;
}

bool LevelHasWayUp()
{
	for (int i = 0; i < numtrigs; i++) {
		if (IsWayUp(trigs[i]))
			return true;
	}
	return false;
}

/** The first tile of the route from `start` to `goal`, after MapRoutes, or nothing if the goal is out of reach. */
std::optional<Point> FirstStep(Point start, Point goal)
{
	if (RouteCost[goal.x][goal.y] == Unreached)
		return std::nullopt;
	Point position = goal;
	for (int guard = 0; guard < MAXDUNX * MAXDUNY; guard++) {
		const Point previous = position - PathDirs[RouteStep[position.x][position.y]];
		if (previous == start)
			return position;
		position = previous;
	}
	return std::nullopt;
}

} // namespace

void StopEscapeToStairs()
{
	EscapeTrigger = -1;
	LastWaypoint = std::nullopt;
	DoorBeingOpened = std::nullopt;
	DoorsThatStayShut.clear();
	SaidBlocked = false;
}

void EscapeToStairsKeyPressed()
{
	if (MyPlayer == nullptr)
		return;
	if (EscapeTrigger >= 0) {
		StopEscapeToStairs();
		NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, MyPlayer->position.future);
		SpeakText(_("Escape cancelled."), true);
		return;
	}
	if (!CanPlayerTakeAction() || InGameMenu())
		return;
	if (leveltype == DTYPE_TOWN) {
		SpeakText(_("Already in town."), true);
		return;
	}

	// Drop the tracker's walk, and the attack or cast a held key would carry on with.
	ResetAutoWalkTracker();
	StopActionKeyHolds();

	Player &player = *MyPlayer;
	const Point start = player.position.future;
	MapDanger();
	MapRoutes(player, start, /*ignoreMonsters=*/false);
	std::optional<int> trigger = CheapestWayUp();
	const bool blocked = !trigger;
	if (blocked) {
		MapRoutes(player, start, /*ignoreMonsters=*/true);
		trigger = CheapestWayUp();
	}
	if (!trigger) {
		SpeakText(LevelHasWayUp() ? _("No path to the stairs.") : _("No way up on this level."), true);
		return;
	}

	StopEscapeToStairs();
	EscapeTrigger = *trigger;
	EscapeLevelKey = CurrentLevelKey();
	SaidBlocked = blocked;

	std::string message = fmt::format(fmt::runtime(_("Running to {:s}.")), TriggerLabelForSpeech(trigs[EscapeTrigger]));
	if (blocked)
		message += fmt::format(" {:s}", _("Blocked by monsters."));
	SpeakText(message, true);

	// Give the first order now, not a tick later.
	UpdateEscapeToStairs();
}

void UpdateEscapeToStairs()
{
	if (EscapeTrigger < 0)
		return;
	// Taking the stairs changes the level, which is how every escape that works ends.
	if (MyPlayer == nullptr || leveltype == DTYPE_TOWN || CurrentLevelKey() != EscapeLevelKey
	    || EscapeTrigger >= numtrigs || !IsWayUp(trigs[EscapeTrigger]) || !CanPlayerTakeAction()) {
		StopEscapeToStairs();
		return;
	}
	if (InGameMenu())
		return;

	Player &player = *MyPlayer;
	const Point goal = trigs[EscapeTrigger].position;
	const Point start = player.position.future;
	if (start == goal)
		return; // The stairs take the player once this step ends.

	if (DoorBeingOpened) {
		if (player.destAction == ACTION_OPERATE)
			return;
		if (IsClosedDoor(*DoorBeingOpened))
			DoorsThatStayShut.push_back(*DoorBeingOpened);
		DoorBeingOpened = std::nullopt;
	}

	MapDanger();
	MapRoutes(player, start, /*ignoreMonsters=*/false);
	const std::optional<Point> next = FirstStep(start, goal);
	if (!next) {
		MapRoutes(player, start, /*ignoreMonsters=*/true);
		if (!FirstStep(start, goal)) {
			StopEscapeToStairs();
			SpeakText(_("No path to the stairs."), true);
			return;
		}
		// Keep trying: the monsters in the way may move.
		if (!SaidBlocked)
			SpeakText(_("Blocked by monsters."), true);
		SaidBlocked = true;
		return;
	}
	SaidBlocked = false;

	if (IsClosedDoor(*next)) {
		DoorBeingOpened = *next;
		LastWaypoint = std::nullopt;
		NetSendCmdLoc(MyPlayerId, true, CMD_OPOBJXY, *next);
		return;
	}

	// One tile at a time, so the engine cannot take a shorter way past a monster. A new order at the
	// start of each step keeps the walk going without a pause.
	const bool stalled = player._pmode == PM_STAND && player.walkpath[0] == WALK_NONE && player.destAction == ACTION_NONE;
	if (LastWaypoint == next && !stalled)
		return;
	LastWaypoint = next;
	NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, *next);
}

} // namespace devilution
