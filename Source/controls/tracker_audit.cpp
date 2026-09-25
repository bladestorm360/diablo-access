/**
 * @file controls/tracker_audit.cpp
 *
 * Offline check of the tracker's routes (test builds only).
 *
 * The truth to check against comes from the game's own rules, not from the tracker: a tile can be
 * crossed once doors are opened and barrels broken, and a target is reached from the tiles the game
 * lets a player act from (IsPlayerAdjacentToObject for objects, one tile for items, the tile itself
 * for stairs). Monsters are ignored there, since they move and die.
 */
#include "controls/tracker_audit.hpp"

#ifdef BUILD_TESTING

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "controls/tracker.hpp"
#include "diablo.h"
#include "engine/assets.hpp"
#include "engine/path.h"
#include "game_mode.hpp"
#include "headless_mode.hpp"
#include "items.h"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "missiles.h"
#include "monster.h"
#include "msg.h"
#include "multi.h"
#include "objects.h"
#include "player.h"
#include "portal.h"
#include "quests.h"
#include "storm/storm_net.hpp"
#include "tables/itemdat.h"
#include "tables/misdat.h"
#include "tables/monstdat.h"
#include "tables/objdat.h"
#include "tables/playerdat.hpp"
#include "tables/spelldat.h"
#include "tables/textdat.h"
#include "towners.h"
#include "utils/quest_spots.hpp"

namespace devilution {

namespace {

constexpr size_t TileCount = MAXDUNX * MAXDUNY;

size_t IndexOf(Point position)
{
	return static_cast<size_t>(position.x) + static_cast<size_t>(position.y) * MAXDUNX;
}

std::string_view CategoryName(TrackerTargetCategory category)
{
	switch (category) {
	case TrackerTargetCategory::Items: return "items";
	case TrackerTargetCategory::Chests: return "chests";
	case TrackerTargetCategory::Doors: return "doors";
	case TrackerTargetCategory::Shrines: return "shrines";
	case TrackerTargetCategory::Objects: return "objects";
	case TrackerTargetCategory::Breakables: return "breakables";
	case TrackerTargetCategory::Monsters: return "monsters";
	case TrackerTargetCategory::DeadBodies: return "dead bodies";
	case TrackerTargetCategory::Npcs: return "npcs";
	case TrackerTargetCategory::Players: return "players";
	case TrackerTargetCategory::DungeonEntrances: return "exits";
	case TrackerTargetCategory::Stairs: return "stairs";
	case TrackerTargetCategory::QuestLocations: return "quest locations";
	case TrackerTargetCategory::Portals: return "portals";
	}
	return "?";
}

bool IsObjectCategory(TrackerTargetCategory category)
{
	return category == TrackerTargetCategory::Chests || category == TrackerTargetCategory::Doors || category == TrackerTargetCategory::Shrines
	    || category == TrackerTargetCategory::Objects || category == TrackerTargetCategory::Breakables;
}

bool IsClosedDoorTile(Point position)
{
	const Object *door = FindObjectAtPosition(position, /*considerLargeObjects=*/false);
	return door != nullptr && door->isDoor() && door->_oVar4 == DOOR_CLOSED;
}

/** Whether a player can cross the tile once its door is opened or its barrel broken. */
bool CanCross(Point position)
{
	if (!InDungeonBounds(position))
		return false;
	const Object *own = FindObjectAtPosition(position, /*considerLargeObjects=*/false);
	if (own != nullptr && own->isDoor())
		return true;
	const Object *object = FindObjectAtPosition(position);
	// A broken crux stays solid; broken barrels, urns and pods do not.
	if (object != nullptr && object->_oSolidFlag && !(object->IsBreakable() && !object->IsCrux()))
		return false;
	return IsTileNotSolid(position);
}

std::vector<bool> MapReachableTiles(Point start)
{
	std::vector<bool> reached(TileCount, false);
	std::queue<Point> queue;
	reached[IndexOf(start)] = true;
	queue.push(start);
	while (!queue.empty()) {
		const Point current = queue.front();
		queue.pop();
		for (const Displacement &step : PathDirs) {
			const Point next = current + step;
			if (!InDungeonBounds(next) || reached[IndexOf(next)] || !CanCross(next) || !CanStep(current, next))
				continue;
			reached[IndexOf(next)] = true;
			queue.push(next);
		}
	}
	return reached;
}

/** The game's rule for operating an object (player.cpp IsPlayerAdjacentToObject). */
bool CanOperateFrom(const Object &object, Point tile)
{
	const int x = std::abs(tile.x - object.position.x);
	int y = std::abs(tile.y - object.position.y);
	if (y > 1 && object.position.y >= 1 && FindObjectAtPosition(object.position + Direction::NorthEast) == &object)
		y = std::abs(tile.y - object.position.y + 1);
	return x <= 1 && y <= 1;
}

bool IsMagicCircle(const Object &object)
{
	return setlevel && setlvlnum == SL_VILEBETRAYER && (object._otype == OBJ_MCIRCLE1 || object._otype == OBJ_MCIRCLE2);
}

std::optional<Point> TargetTile(TrackerTargetCategory category, int id)
{
	switch (category) {
	case TrackerTargetCategory::Items:
		return Items[id].position;
	case TrackerTargetCategory::Chests:
	case TrackerTargetCategory::Doors:
	case TrackerTargetCategory::Shrines:
	case TrackerTargetCategory::Objects:
	case TrackerTargetCategory::Breakables:
		return Objects[id].position;
	case TrackerTargetCategory::DeadBodies:
		return Point { id % MAXDUNX, id / MAXDUNX };
	case TrackerTargetCategory::Npcs:
		return Towners[id].position;
	case TrackerTargetCategory::DungeonEntrances:
	case TrackerTargetCategory::Stairs:
	case TrackerTargetCategory::QuestLocations:
		if (id < 0) {
			const int encoded = -id - 1;
			return Point { encoded % MAXDUNX, encoded / MAXDUNX };
		}
		if (category == TrackerTargetCategory::QuestLocations) {
			if (id >= 10000) {
				if (const std::optional<QuestSpot> spot = FindQuestSpot(static_cast<QuestSpotId>(id - 10000)))
					return spot->position;
				return std::nullopt;
			}
			if (!setlevel)
				return Quests[id].position;
		}
		return trigs[id].position;
	case TrackerTargetCategory::Portals:
		return Portals[id].position;
	default:
		return std::nullopt;
	}
}

/** Tiles the player has to stand on to use the target, by the game's rules. */
std::vector<Point> PlacesToUseTarget(TrackerTargetCategory category, int id)
{
	std::vector<Point> places;
	const std::optional<Point> target = TargetTile(category, id);
	if (!target)
		return places;
	const auto addIf = [&](Point tile, bool ok) {
		if (ok && InDungeonBounds(tile) && CanCross(tile) && !IsClosedDoorTile(tile))
			places.push_back(tile);
	};

	if (IsObjectCategory(category)) {
		const Object &object = Objects[id];
		if (IsMagicCircle(object)) {
			addIf(object.position, true);
			return places;
		}
		for (int dy = -2; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				const Point tile = object.position + Displacement { dx, dy };
				addIf(tile, CanOperateFrom(object, tile));
			}
		}
		return places;
	}
	if (category == TrackerTargetCategory::QuestLocations && id >= 10000) {
		const std::optional<QuestSpot> spot = FindQuestSpot(static_cast<QuestSpotId>(id - 10000));
		const Rectangle &area = spot->area;
		for (int y = area.position.y - 1; y <= area.position.y + area.size.height; y++) {
			for (int x = area.position.x - 1; x <= area.position.x + area.size.width; x++)
				addIf({ x, y }, IsQuestSpotInRange(*spot, { x, y }));
		}
		return places;
	}
	switch (category) {
	case TrackerTargetCategory::Items:
	case TrackerTargetCategory::DeadBodies:
	case TrackerTargetCategory::Npcs:
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++)
				addIf(*target + Displacement { dx, dy }, true);
		}
		break;
	default:
		// Stairs, exits, portals and quest entrances are used by stepping onto them.
		addIf(*target, true);
		break;
	}
	return places;
}

std::string DescribeTarget(TrackerTargetCategory category, int id, std::string_view name)
{
	std::string text = fmt::format("level {} {} \"{}\"", currlevel, CategoryName(category), name);
	if (const std::optional<Point> tile = TargetTile(category, id))
		text += fmt::format(" at ({},{})", tile->x, tile->y);
	if (IsObjectCategory(category)) {
		const Object &object = Objects[id];
		text += fmt::format(" type {} solid={}", static_cast<int>(object._otype), object._oSolidFlag);
	}
	return text;
}

/** The tiles around a target, for reading a failure: # wall, D closed door, O solid object, B breakable, M monster, T the target, P player. */
std::string DrawSurroundings(Point center, int radius, const std::vector<bool> *reachable = nullptr)
{
	std::string text;
	for (int y = center.y - radius; y <= center.y + radius; y++) {
		text += "      ";
		for (int x = center.x - radius; x <= center.x + radius; x++) {
			const Point tile { x, y };
			char c = '.';
			if (!InDungeonBounds(tile)) {
				c = ' ';
			} else {
				const Object *object = FindObjectAtPosition(tile);
				if (tile == MyPlayer->position.tile)
					c = 'P';
				else if (tile == center)
					c = 'T';
				else if (IsClosedDoorTile(tile))
					c = 'D';
				else if (object != nullptr && object->_oSolidFlag)
					c = object->IsBreakable() ? 'B' : 'O';
				else if (IsTileSolid(tile))
					c = '#';
				else if (dMonster[x][y] != 0)
					c = 'M';
				else if (object != nullptr)
					c = 'o';
				else if (reachable != nullptr && (*reachable)[IndexOf(tile)])
					c = ',';
			}
			text += c;
		}
		text += '\n';
	}
	return text;
}

void PlacePlayer(Point position)
{
	Player &player = *MyPlayer;
	dPlayer[player.position.tile.x][player.position.tile.y] = 0;
	player.position.tile = position;
	player.position.future = position;
	player.position.old = position;
	dPlayer[position.x][position.y] = static_cast<int8_t>(MyPlayerId + 1);
}

/** What a player does about the thing Shift+Home says is in the way. False if there is none. */
bool ClearBlockerNextTo(Point position, std::string_view stopMessage)
{
	for (const Displacement &step : PathDirs) {
		const Point tile = position + step;
		if (!InDungeonBounds(tile))
			continue;
		if (stopMessage.find("door") != std::string_view::npos) {
			Object *door = FindObjectAtPosition(tile);
			if (door != nullptr && door->isDoor() && door->_oVar4 == DOOR_CLOSED) {
				// Players[1] is not MyPlayer, so the door opens here without a network message.
				SyncOpObject(Players[1], CMD_OPENDOOR, *door);
				return true;
			}
		} else if (stopMessage.find("monster") != std::string_view::npos) {
			if (dMonster[tile.x][tile.y] != 0) {
				Monsters[std::abs(dMonster[tile.x][tile.y]) - 1].hitPoints = 0;
				dMonster[tile.x][tile.y] = 0;
				return true;
			}
		} else if (stopMessage.find("breakable") != std::string_view::npos) {
			Object *object = FindObjectAtPosition(tile);
			if (object != nullptr && object->IsBreakable()) {
				object->_oBreak = -1;
				object->_oSolidFlag = object->IsCrux();
				object->selectionRegion = SelectionRegion::None;
				return true;
			}
		}
	}
	return false;
}

struct LevelSnapshot {
	std::vector<uint16_t> piece;
	std::vector<int8_t> special;
	std::vector<int16_t> monsterMap;
	std::vector<Object> objects;
	std::vector<int> monsterLife;

	void Take()
	{
		piece.assign(&dPiece[0][0], &dPiece[0][0] + TileCount);
		special.assign(&dSpecial[0][0], &dSpecial[0][0] + TileCount);
		monsterMap.assign(&dMonster[0][0], &dMonster[0][0] + TileCount);
		objects.assign(Objects, Objects + MAXOBJECTS);
		monsterLife.clear();
		for (const Monster &monster : Monsters)
			monsterLife.push_back(monster.hitPoints);
	}

	void Restore() const
	{
		std::copy(piece.begin(), piece.end(), &dPiece[0][0]);
		std::copy(special.begin(), special.end(), &dSpecial[0][0]);
		std::copy(monsterMap.begin(), monsterMap.end(), &dMonster[0][0]);
		std::copy(objects.begin(), objects.end(), Objects);
		for (size_t i = 0; i < MaxMonsters; i++)
			Monsters[i].hitPoints = monsterLife[i];
	}
};

struct SpokenStep {
	std::string_view word;
	Displacement step;
};

constexpr std::array<SpokenStep, 8> SpokenSteps { {
	{ "northwest", { -1, -1 } },
	{ "northeast", { 1, -1 } },
	{ "southeast", { 1, 1 } },
	{ "southwest", { -1, 1 } },
	{ "north", { 0, -1 } },
	{ "south", { 0, 1 } },
	{ "east", { 1, 0 } },
	{ "west", { -1, 0 } },
} };

/** Follows spoken directions such as "north 3, east 2". Empty if the words cannot be read or a step cannot be taken. */
std::optional<Point> FollowDirections(Point start, std::string_view directions, std::string &trouble)
{
	Point position = start;
	if (directions == "here")
		return position;
	while (!directions.empty()) {
		const size_t comma = directions.find(", ");
		const std::string_view part = directions.substr(0, comma);
		directions = comma == std::string_view::npos ? std::string_view {} : directions.substr(comma + 2);
		const size_t space = part.rfind(' ');
		if (space == std::string_view::npos) {
			trouble = fmt::format("unreadable \"{}\"", part);
			return std::nullopt;
		}
		const std::string_view word = part.substr(0, space);
		const int count = std::atoi(std::string(part.substr(space + 1)).c_str());
		const auto spoken = std::find_if(SpokenSteps.begin(), SpokenSteps.end(), [&](const SpokenStep &s) { return s.word == word; });
		if (spoken == SpokenSteps.end() || count <= 0) {
			trouble = fmt::format("unreadable \"{}\"", part);
			return std::nullopt;
		}
		for (int i = 0; i < count; i++) {
			const Point next = position + spoken->step;
			if (!CanCross(next) || !CanStep(position, next)) {
				trouble = fmt::format("\"{}\" walks into a wall at ({},{})", part, next.x, next.y);
				return std::nullopt;
			}
			position = next;
		}
	}
	return position;
}

bool IsNextToBlocker(Point position)
{
	for (const Displacement &step : PathDirs) {
		const Point tile = position + step;
		if (!InDungeonBounds(tile))
			continue;
		const Object *object = FindObjectAtPosition(tile);
		if (IsClosedDoorTile(tile) || (object != nullptr && object->isDoor() && object->_oVar4 == DOOR_CLOSED))
			return true;
		if (object != nullptr && object->_oSolidFlag && object->IsBreakable())
			return true;
		if (dMonster[tile.x][tile.y] != 0)
			return true;
	}
	return false;
}

bool IsOneStepFrom(const std::vector<Point> &places, Point tile)
{
	for (const Point place : places) {
		if (place == tile || (place.WalkingDistance(tile) == 1 && CanStep(tile, place)))
			return true;
	}
	return false;
}

/** What Home says, checked by following its directions from `start`. */
std::string CheckHome(const TrackerTargetForTesting &target, const std::vector<Point> &places, Point start)
{
	PlacePlayer(start);
	const std::string said = DescribeTrackerTargetForTesting(target.category, target.id);
	if (said.find("Can't find a path") != std::string::npos)
		return "Home: can't find a path";
	if (said.find("blocking the path") != std::string::npos)
		return IsNextToBlocker(start) ? "Home: blocked next to the player" : "Home: says blocked, nothing there";

	std::string_view directions = std::string_view(said).substr(said.rfind('\n') == std::string::npos ? 0 : said.rfind('\n') + 1);
	// "north 3, east 2, then a closed door" stops at what is in the way.
	const size_t then = directions.find(", then ");
	const bool endsAtBlocker = then != std::string_view::npos;
	if (endsAtBlocker)
		directions = directions.substr(0, then);

	std::string trouble;
	const std::optional<Point> end = FollowDirections(start, directions, trouble);
	if (!end)
		return fmt::format("Home: bad directions ({}) in \"{}\"", trouble, said);
	if (endsAtBlocker)
		return IsNextToBlocker(*end) ? "Home: ok, up to a door, monster or barrel" : fmt::format("Home: says something is in the way at ({},{}), nothing is", end->x, end->y);
	if (IsOneStepFrom(places, *end))
		return "Home: ok";
	return fmt::format("Home: directions end at ({},{}), out of reach", end->x, end->y);
}

/** Follows Shift+Home from `start` until it stops, clearing what it says is in the way. */
std::string CheckShiftHome(const TrackerTargetForTesting &target, const std::vector<Point> &places, Point start)
{
	PlacePlayer(start);
	for (int tick = 0; tick < 300; tick++) {
		std::string stopMessage;
		const Point position = MyPlayer->position.tile;
		const std::optional<Point> walkTo = PlanAutoWalkStepForTesting(target.category, target.id, /*starting=*/tick == 0, stopMessage);
		if (!walkTo) {
			if (stopMessage.find("in range") != std::string::npos)
				return IsOneStepFrom(places, position) ? "Shift+Home: ok" : fmt::format("Shift+Home: \"in range\" at ({},{}) but it is not", position.x, position.y);
			if (stopMessage.find("blocking the path") == std::string::npos)
				return "Shift+Home: " + stopMessage;
			if (!ClearBlockerNextTo(position, stopMessage))
				return "Shift+Home: says blocked, nothing there (" + stopMessage + ")";
			continue;
		}
		// The game walks there with MakePlrPath, which fails where this does.
		std::array<int8_t, MaxPathLengthPlayer> path;
		if (*walkTo == position)
			return fmt::format("Shift+Home: sent to where the player stands ({},{})", position.x, position.y);
		if (FindPath(CanStep, [](Point p) { return PosOkPlayer(*MyPlayer, p); }, position, *walkTo, path.data(), path.size()) == 0)
			return fmt::format("Shift+Home: the game cannot walk from ({},{}) to ({},{})", position.x, position.y, walkTo->x, walkTo->y);
		PlacePlayer(*walkTo);
	}
	return "Shift+Home: never arrives";
}

} // namespace

bool InitTrackerAudit(bool hellfire)
{
	HeadlessMode = true;
	gbMusicOn = false;
	gbSoundOn = false;
	LoadCoreArchives();
	LoadGameArchives();
	if (!HaveMainData())
		return false;
	gbIsSpawn = false;
	gbIsHellfire = false;
	UnloadModArchives();
	if (hellfire) {
		if (!HaveHellfire())
			return false;
		const std::string_view mods[] = { "Hellfire" };
		LoadModArchives(mods);
		LoadHellfireArchives();
		gbIsHellfire = true;
	}
	LoadTextData();
	LoadPlayerDataFiles();
	LoadSpellData();
	LoadMissileData();
	LoadMonsterData();
	LoadItemData();
	LoadObjectData();
	LoadQuestData();

	gbIsMultiplayer = false;
	sgGameInitInfo.fullQuests = 1;
	// Single player talks to itself through the loopback provider, as InitSingle sets it up.
	SNetInitializeProvider(SELCONN_LOOPBACK, &sgGameInitInfo);
	GameData gameInitInfo = sgGameInitInfo;
	gameInitInfo.swapLE();
	int playerId = 0;
	SNetCreateGame("local", "local", reinterpret_cast<char *>(&gameInitInfo), sizeof(gameInitInfo), &playerId);
	// A second, inactive player lets the audit open doors without sending network messages.
	Players.resize(2);
	MyPlayerId = 0;
	MyPlayer = &Players[0];
	CreatePlayer(*MyPlayer, HeroClass::Warrior);
	MyPlayer->plractive = true;
	return true;
}

void StartTrackerAuditGame(uint32_t gameSeed)
{
	for (int i = 0; i < NUMLEVELS; i++) {
		DungeonSeeds[i] = gameSeed * 2654435761U + static_cast<uint32_t>(i) * 40503U;
		LevelSeeds[i] = std::nullopt;
	}
	setlevel = false;
	InitQuests();
	for (bool &visited : MyPlayer->_pLvlVisited)
		visited = false;
}

bool EnterTrackerAuditLevel(int level)
{
	currlevel = static_cast<uint8_t>(level);
	leveltype = GetLevelType(level);
	setlevel = false;
	MyPlayer->setLevel(static_cast<uint8_t>(level));
	MyPlayer->_pLvlChanging = false;
	MyPlayer->_pmode = PM_STAND;
	// The Hive and the Crypt are entered from town, through their warps.
	const lvl_entry entry = level == 17 || level == 21 ? ENTRY_TWARPDN : ENTRY_MAIN;
	const tl::expected<void, std::string> result = LoadGameLevel(/*firstflag=*/false, entry);
	return result.has_value();
}

void AuditTrackerRoutesOnCurrentLevel(TrackerAuditReport &report)
{
	const Point start = MyPlayer->position.tile;
	const std::vector<bool> reachable = MapReachableTiles(start);
	LevelSnapshot snapshot;
	snapshot.Take();

	for (const TrackerTargetForTesting &target : ListTrackerTargetsForTesting()) {
		if (target.category == TrackerTargetCategory::Monsters || target.category == TrackerTargetCategory::Players)
			continue;
		report.targets++;
		const std::string label = DescribeTarget(target.category, target.id, target.name);
		const std::string category(CategoryName(target.category));
		const std::vector<Point> places = PlacesToUseTarget(target.category, target.id);
		const bool canReach = std::any_of(places.begin(), places.end(), [&](Point p) { return reachable[IndexOf(p)]; });
		const std::optional<Point> targetTile = TargetTile(target.category, target.id);
		PlacePlayer(start);
		const std::string map = targetTile ? DrawSurroundings(*targetTile, canReach ? 3 : 12, &reachable) : std::string {};

		if (places.empty()) {
			report.counts[category + ": listed, but nothing lets a player use it"]++;
			report.problems.push_back(fmt::format("Listed, but no tile lets a player use it: {}\n{}", label, map));
			continue;
		}

		const std::string homeOutcome = CheckHome(target, places, start);
		snapshot.Restore();
		const std::string walkOutcome = CheckShiftHome(target, places, start);
		snapshot.Restore();

		if (!canReach) {
			// Walled off until a quest opens it: both keys should say there is no way.
			report.counts[category + ": walled off"]++;
			if (homeOutcome != "Home: can't find a path" || walkOutcome != "Shift+Home: Can't find a path to the target.")
				report.problems.push_back(fmt::format("Walled off, yet {} / {}: {}\n{}", homeOutcome, walkOutcome, label, map));
			continue;
		}

		report.counts[category + ": " + homeOutcome]++;
		report.counts[category + ": " + walkOutcome]++;
		if (homeOutcome.rfind("Home: ok", 0) != 0 && homeOutcome != "Home: blocked next to the player")
			report.problems.push_back(fmt::format("{}: {}\n{}", homeOutcome, label, map));
		if (walkOutcome != "Shift+Home: ok")
			report.problems.push_back(fmt::format("{}: {} (from ({},{}))\n{}", walkOutcome, label, start.x, start.y, map));
	}
	PlacePlayer(start);
}

} // namespace devilution

#endif
