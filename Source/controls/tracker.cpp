/**
 * @file controls/tracker.cpp
 *
 * Tracker system for accessibility: target cycling, pathfinding, and auto-walk.
 */
#include "controls/tracker.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#ifdef USE_SDL3
#include <SDL3/SDL_keycode.h>
#else
#include <SDL.h>
#endif

#include "appfat.h"
#include "automap.h"
#include "controls/accessibility_keys.hpp"
#include "controls/plrctrls.h"
#include "diablo.h"
#include "engine/path.h"
#include "gamemenu.h"
#include "help.h"
#include "items.h"
#include "levels/gendung.h"
#include "levels/setmaps.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "missiles.h"
#include "monster.h"
#include "multi.h"
#include "objects.h"
#include "player.h"
#include "portal.h"
#include "qol/chatlog.h"
#include "quests.h"
#include "stores.h"
#include "tables/textdat.h"
#include "towners.h"
#include "utils/accessibility_announcements.hpp"
#include "utils/is_of.hpp"
#include "utils/language.h"
#include "utils/navigation_speech.hpp"
#include "utils/quest_spots.hpp"
#include "utils/screen_reader.hpp"
#include "utils/sdl_compat.h"
#include "utils/str_cat.hpp"
#include "utils/string_or_view.hpp"
#include "utils/walk_path_speech.hpp"

namespace devilution {

namespace {

TrackerTargetCategory SelectedTrackerTargetCategory = TrackerTargetCategory::Items;

TrackerTargetCategory AutoWalkTrackerTargetCategory = TrackerTargetCategory::Items; ///< Category of the active auto-walk target.
int AutoWalkTrackerTargetId = -1;                                                   ///< ID of the target being auto-walked to, or -1 if inactive.
bool AutoWalkTrackerStarting = false;                                               ///< Set until the auto-walk plans its first step.

/// Maximum Chebyshev distance (in tiles) at which the player is considered
/// close enough to interact with a tracker target.
constexpr int TrackerInteractDistanceTiles = 1;
// Selection list range for PageUp/PageDown. Use a value larger than the maximum
// possible distance across the 112x112 dungeon grid so the list includes all
// eligible targets on the current level.
constexpr int TrackerCycleDistanceTiles = MAXDUNX + MAXDUNY;

int LockedTrackerItemId = -1;
int LockedTrackerChestId = -1;
int LockedTrackerDoorId = -1;
int LockedTrackerShrineId = -1;
int LockedTrackerObjectId = -1;
int LockedTrackerBreakableId = -1;
int LockedTrackerMonsterId = -1;
int LockedTrackerDeadBodyId = -1;
int LockedTrackerNpcId = -1;
int LockedTrackerPlayerId = -1;
int LockedTrackerDungeonEntranceId = -1;
int LockedTrackerStairsId = -1;
int LockedTrackerQuestLocationId = -1;
int LockedTrackerPortalId = -1;

struct TrackerLevelKey {
	dungeon_type levelType;
	int currLevel;
	bool isSetLevel;
	int setLevelNum;

	friend bool operator==(const TrackerLevelKey &lhs, const TrackerLevelKey &rhs)
	{
		return lhs.levelType == rhs.levelType && lhs.currLevel == rhs.currLevel
		    && lhs.isSetLevel == rhs.isSetLevel && lhs.setLevelNum == rhs.setLevelNum;
	}
	friend bool operator!=(const TrackerLevelKey &lhs, const TrackerLevelKey &rhs)
	{
		return !(lhs == rhs);
	}
};

std::optional<TrackerLevelKey> LockedTrackerLevelKey;

void ClearTrackerLocks()
{
	LockedTrackerItemId = -1;
	LockedTrackerChestId = -1;
	LockedTrackerDoorId = -1;
	LockedTrackerShrineId = -1;
	LockedTrackerObjectId = -1;
	LockedTrackerBreakableId = -1;
	LockedTrackerMonsterId = -1;
	LockedTrackerDeadBodyId = -1;
	LockedTrackerNpcId = -1;
	LockedTrackerPlayerId = -1;
	LockedTrackerDungeonEntranceId = -1;
	LockedTrackerStairsId = -1;
	LockedTrackerQuestLocationId = -1;
	LockedTrackerPortalId = -1;
}

void EnsureTrackerLocksMatchCurrentLevel()
{
	const TrackerLevelKey current {
		.levelType = leveltype,
		.currLevel = currlevel,
		.isSetLevel = setlevel,
		.setLevelNum = setlvlnum,
	};

	if (!LockedTrackerLevelKey || *LockedTrackerLevelKey != current) {
		ClearTrackerLocks();
		LockedTrackerLevelKey = current;
	}
}

int &LockedTrackerTargetId(TrackerTargetCategory category)
{
	switch (category) {
	case TrackerTargetCategory::Items:
		return LockedTrackerItemId;
	case TrackerTargetCategory::Chests:
		return LockedTrackerChestId;
	case TrackerTargetCategory::Doors:
		return LockedTrackerDoorId;
	case TrackerTargetCategory::Shrines:
		return LockedTrackerShrineId;
	case TrackerTargetCategory::Objects:
		return LockedTrackerObjectId;
	case TrackerTargetCategory::Breakables:
		return LockedTrackerBreakableId;
	case TrackerTargetCategory::Monsters:
		return LockedTrackerMonsterId;
	case TrackerTargetCategory::DeadBodies:
		return LockedTrackerDeadBodyId;
	case TrackerTargetCategory::Npcs:
		return LockedTrackerNpcId;
	case TrackerTargetCategory::Players:
		return LockedTrackerPlayerId;
	case TrackerTargetCategory::DungeonEntrances:
		return LockedTrackerDungeonEntranceId;
	case TrackerTargetCategory::Stairs:
		return LockedTrackerStairsId;
	case TrackerTargetCategory::QuestLocations:
		return LockedTrackerQuestLocationId;
	case TrackerTargetCategory::Portals:
		return LockedTrackerPortalId;
	}
	app_fatal("Invalid TrackerTargetCategory");
}

std::string_view TrackerTargetCategoryLabel(TrackerTargetCategory category)
{
	switch (category) {
	case TrackerTargetCategory::Items:
		return _("items");
	case TrackerTargetCategory::Chests:
		return _("chests");
	case TrackerTargetCategory::Doors:
		return _("doors");
	case TrackerTargetCategory::Shrines:
		return _("shrines");
	case TrackerTargetCategory::Objects:
		return _("objects");
	case TrackerTargetCategory::Breakables:
		return _("breakables");
	case TrackerTargetCategory::Monsters:
		return _("monsters");
	case TrackerTargetCategory::DeadBodies:
		return _("dead bodies");
	case TrackerTargetCategory::Npcs:
		return _("NPCs");
	case TrackerTargetCategory::Players:
		return _("players");
	case TrackerTargetCategory::DungeonEntrances:
		if (leveltype != DTYPE_TOWN)
			return _("exits");
		return _("dungeon entrances");
	case TrackerTargetCategory::Stairs:
		return _("stairs");
	case TrackerTargetCategory::QuestLocations:
		return _("quest locations");
	case TrackerTargetCategory::Portals:
		return _("portals");
	}
	app_fatal("Invalid TrackerTargetCategory");
}

void SpeakTrackerTargetCategory()
{
	SpeakText(TrackerTargetCategoryLabel(SelectedTrackerTargetCategory), true);
}

/** Whether a player could stand on the tile once doors are opened and barrels broken. */
[[nodiscard]] bool CouldStandOn(Point tile)
{
	return InDungeonBounds(tile) && IsTileWalkableForTrackerPath(tile, /*ignoreDoors=*/true, /*ignoreBreakables=*/true);
}

/** Whether a player could stand on or next to the tile, as picking up an item needs. */
[[nodiscard]] bool HasRoomAround(Point position)
{
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			if (CouldStandOn(position + Displacement { dx, dy }))
				return true;
		}
	}
	return false;
}

std::optional<int> FindNearestGroundItemId(Point playerPosition)
{
	std::optional<int> bestId;
	int bestDistance = 0;

	for (int y = 0; y < MAXDUNY; ++y) {
		for (int x = 0; x < MAXDUNX; ++x) {
			const int itemId = std::abs(dItem[x][y]) - 1;
			if (itemId < 0 || itemId > MAXITEMS)
				continue;

			const Item &item = Items[itemId];
			if (item.isEmpty() || item._iClass == ICLASS_NONE)
				continue;
			if (!HasRoomAround(item.position))
				continue;

			const int distance = playerPosition.WalkingDistance(Point { x, y });
			if (!bestId || distance < bestDistance) {
				bestId = itemId;
				bestDistance = distance;
			}
		}
	}

	return bestId;
}

[[nodiscard]] constexpr int CorpseTrackerIdForPosition(Point position)
{
	return position.x + position.y * MAXDUNX;
}

[[nodiscard]] constexpr Point CorpsePositionForTrackerId(int corpseId)
{
	return { corpseId % MAXDUNX, corpseId / MAXDUNX };
}

std::optional<int> FindNearestCorpseId(Point playerPosition)
{
	std::optional<int> bestId;
	int bestDistance = 0;

	for (int y = 0; y < MAXDUNY; ++y) {
		for (int x = 0; x < MAXDUNX; ++x) {
			if (dCorpse[x][y] == 0 || !HasRoomAround({ x, y }))
				continue;

			const Point position { x, y };
			const int distance = playerPosition.WalkingDistance(position);
			if (!bestId || distance < bestDistance) {
				bestId = CorpseTrackerIdForPosition(position);
				bestDistance = distance;
			}
		}
	}

	return bestId;
}

struct TrackerCandidate {
	int id;
	int distance;
	StringOrView name;
};

[[nodiscard]] bool IsBetterTrackerCandidate(const TrackerCandidate &a, const TrackerCandidate &b)
{
	if (a.distance != b.distance)
		return a.distance < b.distance;
	return a.id < b.id;
}

[[nodiscard]] constexpr int RedPortalTrackerIdForPosition(Point position)
{
	// Encode tile position into a stable negative id.
	// MAXDUNX/MAXDUNY are 112, so this easily fits in int.
	return -((position.y * MAXDUNX) + position.x + 1);
}

[[nodiscard]] constexpr bool IsRedPortalTrackerId(int id)
{
	return id < 0;
}

[[nodiscard]] constexpr Point RedPortalPositionForTrackerId(int id)
{
	const int encoded = -id - 1;
	return { encoded % MAXDUNX, encoded / MAXDUNX };
}

// Quest spot ids share the QuestLocations id space with quest indices, trigger indices and
// (negative) red portal ids, so they start far above any of those.
constexpr int QuestSpotTrackerIdBase = 10000;

[[nodiscard]] constexpr int QuestSpotTrackerId(QuestSpotId id)
{
	return QuestSpotTrackerIdBase + static_cast<int>(id);
}

[[nodiscard]] constexpr bool IsQuestSpotTrackerId(int id)
{
	return id >= QuestSpotTrackerIdBase && id < QuestSpotTrackerIdBase + 256;
}

[[nodiscard]] constexpr QuestSpotId QuestSpotIdForTrackerId(int id)
{
	return static_cast<QuestSpotId>(id - QuestSpotTrackerIdBase);
}

[[nodiscard]] StringOrView ItemLabelForSpeech(const Item &item)
{
	const StringOrView name = item.getName();
	if (name.empty())
		return name;

	switch (item._iMagical) {
	case ITEM_QUALITY_MAGIC:
		return StrCat(name, ", ", _("magic item"));
	case ITEM_QUALITY_UNIQUE:
		return StrCat(name, ", ", _("unique item"));
	default:
		return name;
	}
}

[[nodiscard]] std::vector<TrackerCandidate> CollectNearbyItemTrackerCandidates(Point playerPosition, int maxDistance)
{
	std::vector<TrackerCandidate> result;
	result.reserve(ActiveItemCount);

	const int minX = std::max(0, playerPosition.x - maxDistance);
	const int minY = std::max(0, playerPosition.y - maxDistance);
	const int maxX = std::min(MAXDUNX - 1, playerPosition.x + maxDistance);
	const int maxY = std::min(MAXDUNY - 1, playerPosition.y + maxDistance);

	std::array<bool, MAXITEMS + 1> seen {};

	for (int y = minY; y <= maxY; ++y) {
		for (int x = minX; x <= maxX; ++x) {
			const int itemId = std::abs(dItem[x][y]) - 1;
			if (itemId < 0 || itemId > MAXITEMS)
				continue;
			if (seen[itemId])
				continue;
			seen[itemId] = true;

			const Item &item = Items[itemId];
			if (item.isEmpty() || item._iClass == ICLASS_NONE)
				continue;
			if (!HasRoomAround(item.position))
				continue;

			const int distance = playerPosition.WalkingDistance(Point { x, y });
			if (distance > maxDistance)
				continue;

			result.push_back(TrackerCandidate {
			    .id = itemId,
			    .distance = distance,
			    .name = ItemLabelForSpeech(item),
			});
		}
	}

	std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
	return result;
}

[[nodiscard]] std::vector<TrackerCandidate> CollectNearbyCorpseTrackerCandidates(Point playerPosition, int maxDistance)
{
	std::vector<TrackerCandidate> result;

	const int minX = std::max(0, playerPosition.x - maxDistance);
	const int minY = std::max(0, playerPosition.y - maxDistance);
	const int maxX = std::min(MAXDUNX - 1, playerPosition.x + maxDistance);
	const int maxY = std::min(MAXDUNY - 1, playerPosition.y + maxDistance);

	for (int y = minY; y <= maxY; ++y) {
		for (int x = minX; x <= maxX; ++x) {
			if (dCorpse[x][y] == 0 || !HasRoomAround({ x, y }))
				continue;

			const Point position { x, y };
			const int distance = playerPosition.WalkingDistance(position);
			if (distance > maxDistance)
				continue;

			result.push_back(TrackerCandidate {
			    .id = CorpseTrackerIdForPosition(position),
			    .distance = distance,
			    .name = _("Dead body"),
			});
		}
	}

	std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
	return result;
}

[[nodiscard]] constexpr bool IsTrackedChestObject(const Object &object)
{
	return object.canInteractWith() && (object.IsChest() || object._otype == _object_id::OBJ_SIGNCHEST);
}

[[nodiscard]] constexpr bool IsTrackedDoorObject(const Object &object)
{
	return object.isDoor() && object.canInteractWith();
}

[[nodiscard]] constexpr bool IsShrineLikeObject(const Object &object)
{
	return object.canInteractWith()
	    && (object.IsShrine()
	        || IsAnyOf(object._otype, _object_id::OBJ_BLOODFTN, _object_id::OBJ_PURIFYINGFTN, _object_id::OBJ_GOATSHRINE, _object_id::OBJ_CAULDRON,
	            _object_id::OBJ_MURKYFTN, _object_id::OBJ_TEARFTN));
}

[[nodiscard]] constexpr bool IsTrackedBreakableObject(const Object &object)
{
	return object.IsBreakable();
}

[[nodiscard]] bool IsLazarusMagicCircleObject(const Object &object)
{
	return setlevel && setlvlnum == SL_VILEBETRAYER && IsAnyOf(object._otype, _object_id::OBJ_MCIRCLE1, _object_id::OBJ_MCIRCLE2);
}

/**
 * Whether a player on `tile` can use `object`, by the game's rule (IsPlayerAdjacentToObject in
 * player.cpp): a tile around it, or beyond the far end of an object two tiles long. Lazarus' magic
 * circles work only when stood on.
 */
[[nodiscard]] bool CanUseObjectFrom(const Object &object, Point tile)
{
	if (IsLazarusMagicCircleObject(object))
		return tile == object.position;
	const int dx = std::abs(tile.x - object.position.x);
	int dy = std::abs(tile.y - object.position.y);
	if (dy > 1 && object.position.y >= 1 && FindObjectAtPosition(object.position + Direction::NorthEast) == &object)
		dy = std::abs(tile.y - object.position.y + 1);
	return dx <= 1 && dy <= 1;
}

/**
 * Whether any tile lets a player use the object. The level generator can leave an object inside the
 * rock at the map's corner: a theme room's shrine or library whose place other objects took goes to
 * tile (0,0) (Theme_Shrine ignores TFit_Shrine failing), and its candles land off the map, where they
 * read as dead bodies at the far corners.
 */
[[nodiscard]] bool HasRoomToUseObject(const Object &object)
{
	for (int dy = -2; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			const Point tile = object.position + Displacement { dx, dy };
			if (CanUseObjectFrom(object, tile) && CouldStandOn(tile))
				return true;
		}
	}
	return false;
}

[[nodiscard]] StringOrView TrackerObjectLabelForSpeech(const Object &object)
{
	if (IsLazarusMagicCircleObject(object)) {
		if (object._otype == _object_id::OBJ_MCIRCLE1)
			return _("Central magic circle");
		return _("Magic circle");
	}

	if (leveltype == DTYPE_CRYPT && currlevel == 24) {
		if (object._otype == OBJ_L5LEVER)
			return _("Na-Krul's lever");
		// The three books by Na-Krul's door share one title; their words tell them apart, and the
		// tracker's distance-based numbering changes as the player moves.
		if (object._otype == OBJ_L5BOOKS && object._oVar8 >= 6 && object._oVar8 <= 8 && static_cast<size_t>(object._oVar2) < Speeches.size())
			return StrCat(object.name(), ": ", _(Speeches[object._oVar2].txtstr));
	}

	return object.name();
}

[[nodiscard]] bool IsTrackedMiscInteractableObject(const Object &object)
{
	if (IsLazarusMagicCircleObject(object))
		return true;
	if (!object.canInteractWith())
		return false;
	if (object.IsChest() || object._otype == _object_id::OBJ_SIGNCHEST)
		return false;
	if (object.isDoor())
		return false;
	if (IsShrineLikeObject(object))
		return false;
	if (object.IsBreakable())
		return false;
	return true;
}

[[nodiscard]] bool IsTrackedMonster(const Monster &monster)
{
	return !monster.isInvalid
	    && (monster.flags & MFLAG_HIDDEN) == 0
	    && monster.hitPoints > 0
	    && !(monster.type().type == MT_GOLEM && monster.position.tile == GolemHoldingCell);
}

template <typename Predicate>
[[nodiscard]] std::vector<TrackerCandidate> CollectNearbyObjectTrackerCandidates(Point playerPosition, int maxDistance, Predicate predicate)
{
	std::vector<TrackerCandidate> result;
	result.reserve(ActiveObjectCount);

	const int minX = std::max(0, playerPosition.x - maxDistance);
	const int minY = std::max(0, playerPosition.y - maxDistance);
	const int maxX = std::min(MAXDUNX - 1, playerPosition.x + maxDistance);
	const int maxY = std::min(MAXDUNY - 1, playerPosition.y + maxDistance);

	std::array<int, MAXOBJECTS> bestDistanceById {};
	bestDistanceById.fill(std::numeric_limits<int>::max());

	for (int y = minY; y <= maxY; ++y) {
		for (int x = minX; x <= maxX; ++x) {
			const int objectId = std::abs(dObject[x][y]) - 1;
			if (objectId < 0 || objectId >= MAXOBJECTS)
				continue;

			const Object &object = Objects[objectId];
			if (object._otype == OBJ_NULL)
				continue;
			if (!predicate(object) || !HasRoomToUseObject(object))
				continue;

			const int distance = playerPosition.WalkingDistance(Point { x, y });
			if (distance > maxDistance)
				continue;

			int &bestDistance = bestDistanceById[objectId];
			if (distance < bestDistance)
				bestDistance = distance;
		}
	}

	for (int objectId = 0; objectId < MAXOBJECTS; ++objectId) {
		const int distance = bestDistanceById[objectId];
		if (distance == std::numeric_limits<int>::max())
			continue;

		const Object &object = Objects[objectId];
		result.push_back(TrackerCandidate {
		    .id = objectId,
		    .distance = distance,
		    .name = TrackerObjectLabelForSpeech(object),
		});
	}

	std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
	return result;
}

template <typename Predicate>
[[nodiscard]] std::optional<int> FindNearestObjectId(Point playerPosition, Predicate predicate)
{
	std::array<int, MAXOBJECTS> bestDistanceById {};
	bestDistanceById.fill(std::numeric_limits<int>::max());

	for (int y = 0; y < MAXDUNY; ++y) {
		for (int x = 0; x < MAXDUNX; ++x) {
			const int objectId = std::abs(dObject[x][y]) - 1;
			if (objectId < 0 || objectId >= MAXOBJECTS)
				continue;

			const Object &object = Objects[objectId];
			if (object._otype == OBJ_NULL)
				continue;
			if (!predicate(object) || !HasRoomToUseObject(object))
				continue;

			const int distance = playerPosition.WalkingDistance(Point { x, y });
			int &bestDistance = bestDistanceById[objectId];
			if (distance < bestDistance)
				bestDistance = distance;
		}
	}

	std::optional<int> bestId;
	int bestDistance = 0;
	for (int objectId = 0; objectId < MAXOBJECTS; ++objectId) {
		const int distance = bestDistanceById[objectId];
		if (distance == std::numeric_limits<int>::max())
			continue;

		if (!bestId || distance < bestDistance) {
			bestId = objectId;
			bestDistance = distance;
		}
	}

	return bestId;
}

[[nodiscard]] std::vector<TrackerCandidate> CollectNearbyChestTrackerCandidates(Point playerPosition, int maxDistance)
{
	return CollectNearbyObjectTrackerCandidates(playerPosition, maxDistance, IsTrackedChestObject);
}

[[nodiscard]] std::vector<TrackerCandidate> CollectNearbyDoorTrackerCandidates(Point playerPosition, int maxDistance)
{
	return CollectNearbyObjectTrackerCandidates(playerPosition, maxDistance, IsTrackedDoorObject);
}

[[nodiscard]] std::vector<TrackerCandidate> CollectNearbyShrineTrackerCandidates(Point playerPosition, int maxDistance)
{
	return CollectNearbyObjectTrackerCandidates(playerPosition, maxDistance, IsShrineLikeObject);
}

[[nodiscard]] std::vector<TrackerCandidate> CollectNearbyBreakableTrackerCandidates(Point playerPosition, int maxDistance)
{
	return CollectNearbyObjectTrackerCandidates(playerPosition, maxDistance, IsTrackedBreakableObject);
}

[[nodiscard]] std::vector<TrackerCandidate> CollectNearbyObjectInteractableTrackerCandidates(Point playerPosition, int maxDistance)
{
	return CollectNearbyObjectTrackerCandidates(playerPosition, maxDistance, IsTrackedMiscInteractableObject);
}

[[nodiscard]] std::vector<TrackerCandidate> CollectNearbyMonsterTrackerCandidates(Point playerPosition, int maxDistance)
{
	std::vector<TrackerCandidate> result;
	result.reserve(ActiveMonsterCount);

	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const int monsterId = static_cast<int>(ActiveMonsters[i]);
		const Monster &monster = Monsters[monsterId];
		if (!IsTrackedMonster(monster))
			continue;

		const int distance = playerPosition.ApproxDistance(monster.position.future);
		if (distance > maxDistance)
			continue;

		result.push_back(TrackerCandidate {
		    .id = monsterId,
		    .distance = distance,
		    .name = MonsterLabelForSpeech(monster),
		});
	}

	std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
	return result;
}

[[nodiscard]] std::vector<TrackerCandidate> CollectNpcTrackerCandidates(Point playerPosition)
{
	std::vector<TrackerCandidate> result;
	if (leveltype != DTYPE_TOWN)
		return result;

	result.reserve(GetNumTowners());
	for (size_t i = 0; i < GetNumTowners(); ++i) {
		const Towner &towner = Towners[i];
		if (!IsTownerPresent(towner._ttype))
			continue;

		const int distance = playerPosition.WalkingDistance(towner.position);
		result.push_back(TrackerCandidate {
		    .id = static_cast<int>(i),
		    .distance = distance,
		    .name = towner.name,
		});
	}

	std::sort(result.begin(), result.end(), [](const TrackerCandidate &a, const TrackerCandidate &b) {
		if (a.distance != b.distance)
			return a.distance < b.distance;
		return a.name.str() < b.name.str();
	});
	return result;
}

[[nodiscard]] std::vector<TrackerCandidate> CollectPlayerTrackerCandidates(Point playerPosition)
{
	std::vector<TrackerCandidate> result;
	if (!gbIsMultiplayer || MyPlayer == nullptr)
		return result;

	result.reserve(MAX_PLRS);

	const uint8_t currentLevel = MyPlayer->plrlevel;
	const bool currentIsSetLevel = setlevel;

	for (int i = 0; i < MAX_PLRS; ++i) {
		if (i == MyPlayerId)
			continue;
		const Player &player = Players[i];
		if (!player.plractive)
			continue;
		if (player._pLvlChanging)
			continue;
		if (player.plrlevel != currentLevel)
			continue;
		if (player.plrIsOnSetLevel != currentIsSetLevel)
			continue;

		const Point otherPosition = player.position.future;
		if (!InDungeonBounds(otherPosition))
			continue;

		const int distance = playerPosition.WalkingDistance(otherPosition);
		result.push_back(TrackerCandidate {
		    .id = i,
		    .distance = distance,
		    .name = player.name(),
		});
	}

	std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
	return result;
}

[[nodiscard]] std::vector<TrackerCandidate> CollectDungeonEntranceTrackerCandidates(Point playerPosition)
{
	std::vector<TrackerCandidate> result;
	if (MyPlayer == nullptr)
		return result;

	if (leveltype == DTYPE_TOWN) {
		const std::vector<int> candidates = CollectTownDungeonTriggerIndices();
		result.reserve(candidates.size());

		for (const int triggerIndex : candidates) {
			if (triggerIndex < 0 || triggerIndex >= numtrigs)
				continue;
			const TriggerStruct &trigger = trigs[triggerIndex];
			const Point triggerPosition { trigger.position.x, trigger.position.y };
			const int distance = playerPosition.WalkingDistance(triggerPosition);
			result.push_back(TrackerCandidate {
			    .id = triggerIndex,
			    .distance = distance,
			    .name = TriggerLabelForSpeech(trigger),
			});
		}

		std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
		return result;
	}

	for (int i = 0; i < numtrigs; ++i) {
		const TriggerStruct &trigger = trigs[i];
		if (setlevel) {
			if (trigger._tmsg != WM_DIABRTNLVL)
				continue;
		} else {
			if (!IsAnyOf(trigger._tmsg, WM_DIABPREVLVL, WM_DIABTWARPUP))
				continue;
		}

		const Point triggerPosition { trigger.position.x, trigger.position.y };
		const int distance = playerPosition.WalkingDistance(triggerPosition);
		result.push_back(TrackerCandidate {
		    .id = i,
		    .distance = distance,
			    .name = TriggerLabelForSpeech(trigger),
			});
	}

	// Lazarus' set level (SL_VILEBETRAYER) uses a RedPortal missile instead of a return trigger.
	// Include it so the player can navigate out like other quest levels.
	if (setlevel) {
		for (const Missile &missile : Missiles) {
			if (missile._mitype != MissileID::RedPortal)
				continue;
			const Point portalPosition = missile.position.tile;
			if (!InDungeonBounds(portalPosition))
				continue;
			const int distance = playerPosition.WalkingDistance(portalPosition);
			result.push_back(TrackerCandidate {
			    .id = RedPortalTrackerIdForPosition(portalPosition),
			    .distance = distance,
			    .name = _("Red portal"),
			});
		}
	}

	std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
	return result;
}

[[nodiscard]] std::optional<Point> FindTownPortalPositionInTownByPortalIndex(int portalIndex)
{
	if (portalIndex < 0 || portalIndex >= MAXPORTAL)
		return std::nullopt;

	for (const Missile &missile : Missiles) {
		if (missile._mitype != MissileID::TownPortal)
			continue;
		if (missile._misource != portalIndex)
			continue;
		return missile.position.tile;
	}

	return std::nullopt;
}

[[nodiscard]] bool IsTownPortalOpenOnCurrentLevel(int portalIndex)
{
	if (portalIndex < 0 || portalIndex >= MAXPORTAL)
		return false;
	const Portal &portal = Portals[portalIndex];
	if (!portal.open)
		return false;
	if (portal.setlvl != setlevel)
		return false;
	if (portal.level != currlevel)
		return false;
	if (portal.ltype != leveltype)
		return false;
	return InDungeonBounds(portal.position);
}

[[nodiscard]] std::vector<TrackerCandidate> CollectPortalTrackerCandidates(Point playerPosition)
{
	std::vector<TrackerCandidate> result;
	if (MyPlayer == nullptr)
		return result;

	if (leveltype == DTYPE_TOWN) {
		std::array<bool, MAXPORTAL> seen {};
		for (const Missile &missile : Missiles) {
			if (missile._mitype != MissileID::TownPortal)
				continue;
			const int portalIndex = missile._misource;
			if (portalIndex < 0 || portalIndex >= MAXPORTAL)
				continue;
			if (seen[portalIndex])
				continue;
			seen[portalIndex] = true;

			const Point portalPosition = missile.position.tile;
			const int distance = playerPosition.WalkingDistance(portalPosition);
			result.push_back(TrackerCandidate {
			    .id = portalIndex,
			    .distance = distance,
			    .name = TownPortalLabelForSpeech(Portals[portalIndex]),
			});
		}
		std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
		return result;
	}

	for (int i = 0; i < MAXPORTAL; ++i) {
		if (!IsTownPortalOpenOnCurrentLevel(i))
			continue;
		const Portal &portal = Portals[i];
		const int distance = playerPosition.WalkingDistance(portal.position);
		result.push_back(TrackerCandidate {
		    .id = i,
		    .distance = distance,
		    .name = TownPortalLabelForSpeech(portal),
		});
	}
	std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
	return result;
}

[[nodiscard]] std::vector<TrackerCandidate> CollectStairsTrackerCandidates(Point playerPosition)
{
	std::vector<TrackerCandidate> result;
	if (MyPlayer == nullptr || leveltype == DTYPE_TOWN)
		return result;

	for (int i = 0; i < numtrigs; ++i) {
		const TriggerStruct &trigger = trigs[i];
		if (!IsAnyOf(trigger._tmsg, WM_DIABNEXTLVL, WM_DIABPREVLVL, WM_DIABTWARPUP))
			continue;

		const Point triggerPosition { trigger.position.x, trigger.position.y };
		const int distance = playerPosition.WalkingDistance(triggerPosition);
		result.push_back(TrackerCandidate {
		    .id = i,
		    .distance = distance,
		    .name = TriggerLabelForSpeech(trigger),
		});
	}

	std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
	return result;
}

[[nodiscard]] std::vector<TrackerCandidate> CollectQuestLocationTrackerCandidates(Point playerPosition)
{
	std::vector<TrackerCandidate> result;
	if (MyPlayer == nullptr)
		return result;

	for (const QuestSpot &spot : CollectQuestSpots()) {
		result.push_back(TrackerCandidate {
		    .id = QuestSpotTrackerId(spot.id),
		    .distance = playerPosition.WalkingDistance(spot.position),
		    .name = QuestSpotLabel(spot.id),
		});
	}

	if (leveltype == DTYPE_TOWN) {
		std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
		return result;
	}

	if (setlevel) {
		for (int i = 0; i < numtrigs; ++i) {
			const TriggerStruct &trigger = trigs[i];
			if (trigger._tmsg != WM_DIABRTNLVL)
				continue;

			const Point triggerPosition { trigger.position.x, trigger.position.y };
			const int distance = playerPosition.WalkingDistance(triggerPosition);
			result.push_back(TrackerCandidate {
			    .id = i,
			    .distance = distance,
			    .name = TriggerLabelForSpeech(trigger),
			});
		}

		// Lazarus' set level (SL_VILEBETRAYER) uses a RedPortal missile instead of a return trigger.
		for (const Missile &missile : Missiles) {
			if (missile._mitype != MissileID::RedPortal)
				continue;
			const Point portalPosition = missile.position.tile;
			if (!InDungeonBounds(portalPosition))
				continue;
			const int distance = playerPosition.WalkingDistance(portalPosition);
			result.push_back(TrackerCandidate {
			    .id = RedPortalTrackerIdForPosition(portalPosition),
			    .distance = distance,
			    .name = _("Red portal"),
			});
		}

		std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
		return result;
	}

	// Mirrors CheckQuests(): set-level entrances do nothing with multiplayer quests, and Lazarus' only
	// once his red portal has opened.
	constexpr size_t NumQuests = sizeof(Quests) / sizeof(Quests[0]);
	result.reserve(NumQuests);
	for (size_t questIndex = 0; questIndex < NumQuests && !UseMultiplayerQuests(); ++questIndex) {
		const Quest &quest = Quests[questIndex];
		if (quest._qslvl == SL_NONE)
			continue;
		if (quest._qactive == QUEST_NOTAVAIL)
			continue;
		if (quest._qidx == Q_BETRAYER && quest._qvar1 < 3)
			continue;
		if (quest._qlevel != currlevel)
			continue;
		if (!InDungeonBounds(quest.position))
			continue;

		const char *questLevelName = QuestLevelNames[quest._qslvl];
		if (questLevelName == nullptr || questLevelName[0] == '\0')
			questLevelName = N_("Set level");

		const int distance = playerPosition.WalkingDistance(quest.position);
		result.push_back(TrackerCandidate {
		    .id = static_cast<int>(questIndex),
		    .distance = distance,
		    .name = _(questLevelName),
		});
	}

	std::sort(result.begin(), result.end(), IsBetterTrackerCandidate);
	return result;
}

[[nodiscard]] std::optional<int> FindNextTrackerCandidateId(const std::vector<TrackerCandidate> &candidates, int currentId)
{
	if (candidates.empty())
		return std::nullopt;
	if (currentId < 0)
		return candidates.front().id;

	const auto it = std::find_if(candidates.begin(), candidates.end(), [currentId](const TrackerCandidate &c) { return c.id == currentId; });
	if (it == candidates.end())
		return candidates.front().id;

	if (candidates.size() <= 1)
		return std::nullopt;

	const size_t idx = static_cast<size_t>(it - candidates.begin());
	const size_t nextIdx = (idx + 1) % candidates.size();
	return candidates[nextIdx].id;
}

[[nodiscard]] std::optional<int> FindPreviousTrackerCandidateId(const std::vector<TrackerCandidate> &candidates, int currentId)
{
	if (candidates.empty())
		return std::nullopt;
	if (currentId < 0)
		return candidates.back().id;

	const auto it = std::find_if(candidates.begin(), candidates.end(), [currentId](const TrackerCandidate &c) { return c.id == currentId; });
	if (it == candidates.end())
		return candidates.back().id;

	if (candidates.size() <= 1)
		return std::nullopt;

	const size_t idx = static_cast<size_t>(it - candidates.begin());
	const size_t prevIdx = (idx + candidates.size() - 1) % candidates.size();
	return candidates[prevIdx].id;
}

void DecorateTrackerTargetNameWithOrdinalIfNeeded(int targetId, StringOrView &targetName, const std::vector<TrackerCandidate> &candidates)
{
	if (targetName.empty())
		return;

	const std::string_view baseName = targetName.str();
	int total = 0;
	for (const TrackerCandidate &c : candidates) {
		if (c.name.str() == baseName)
			++total;
	}
	if (total <= 1)
		return;

	int ordinal = 0;
	int seen = 0;
	for (const TrackerCandidate &c : candidates) {
		if (c.name.str() != baseName)
			continue;
		++seen;
		if (c.id == targetId) {
			ordinal = seen;
			break;
		}
	}
	if (ordinal <= 0)
		return;

	std::string decorated;
	StrAppend(decorated, baseName, " ", ordinal);
	targetName = std::move(decorated);
}

[[nodiscard]] bool IsGroundItemPresent(int itemId)
{
	if (itemId < 0 || itemId > MAXITEMS)
		return false;

	for (uint8_t i = 0; i < ActiveItemCount; ++i) {
		if (ActiveItems[i] == itemId)
			return true;
	}

	return false;
}

[[nodiscard]] bool IsCorpsePresent(int corpseId)
{
	if (corpseId < 0 || corpseId >= MAXDUNX * MAXDUNY)
		return false;

	const Point position = CorpsePositionForTrackerId(corpseId);
	return InDungeonBounds(position) && dCorpse[position.x][position.y] != 0;
}

std::optional<int> FindNearestUnopenedChestObjectId(Point playerPosition)
{
	return FindNearestObjectId(playerPosition, IsTrackedChestObject);
}

std::optional<int> FindNearestDoorObjectId(Point playerPosition)
{
	return FindNearestObjectId(playerPosition, IsTrackedDoorObject);
}

std::optional<int> FindNearestShrineObjectId(Point playerPosition)
{
	return FindNearestObjectId(playerPosition, IsShrineLikeObject);
}

std::optional<int> FindNearestBreakableObjectId(Point playerPosition)
{
	return FindNearestObjectId(playerPosition, IsTrackedBreakableObject);
}

std::optional<int> FindNearestMiscInteractableObjectId(Point playerPosition)
{
	return FindNearestObjectId(playerPosition, IsTrackedMiscInteractableObject);
}

std::optional<int> FindNearestMonsterId(Point playerPosition)
{
	std::optional<int> bestId;
	int bestDistance = 0;

	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const int monsterId = static_cast<int>(ActiveMonsters[i]);
		const Monster &monster = Monsters[monsterId];
		if (!IsTrackedMonster(monster))
			continue;

		const int distance = playerPosition.ApproxDistance(monster.position.future);
		if (!bestId || distance < bestDistance) {
			bestId = monsterId;
			bestDistance = distance;
		}
	}

	return bestId;
}

/** A place the tracker leads to, and the tiles that count as having reached it. */
struct TrackerGoal {
	/** The target's own tile. Routes head for it. */
	Point position;
	/** An object is reached from wherever the game lets the player use it. */
	const Object *object = nullptr;
	/** A quest spot has a rule of its own. */
	std::optional<QuestSpot> spot;
	/** Someone or something solid stands on its tile, so it is reached from the tiles around it. */
	bool occupied = false;

	/**
	 * Whether a player on `tile` has reached the target. Anything that is not an object, quest spot or
	 * someone standing there is walked onto: stairs, exits, portals, items and dead bodies.
	 */
	[[nodiscard]] bool IsReachedFrom(Point tile) const
	{
		if (spot)
			return IsQuestSpotInRange(*spot, tile);
		if (object != nullptr)
			return CanUseObjectFrom(*object, tile);
		if (occupied)
			return tile != position && position.WalkingDistance(tile) <= TrackerInteractDistanceTiles;
		return tile == position;
	}

	/** Whether a target that is walked onto is one step from `tile`. */
	[[nodiscard]] bool IsOneStepFrom(Point tile) const
	{
		return object == nullptr && !spot && !occupied && position.WalkingDistance(tile) == 1 && CanStep(tile, position);
	}
};

/** A target walked onto, unless something solid stands there, such as the pedestal under the Magic Rock. */
[[nodiscard]] TrackerGoal TileGoal(Point position)
{
	return { .position = position, .occupied = !IsTileWalkable(position) };
}

[[nodiscard]] TrackerGoal OccupiedTileGoal(Point position)
{
	return { .position = position, .occupied = true };
}

[[nodiscard]] TrackerGoal ObjectGoal(const Object &object)
{
	return { .position = object.position, .object = &object };
}

[[nodiscard]] TrackerGoal QuestSpotGoal(const QuestSpot &spot)
{
	return { .position = spot.position, .spot = spot };
}

enum class TrackerPathBlockType : uint8_t {
	Door,
	Monster,
	Breakable,
};

struct TrackerPathBlockInfo {
	TrackerPathBlockType type;
	size_t stepIndex;
	Point beforeBlock;
	Point blockPosition;
};

[[nodiscard]] std::optional<TrackerPathBlockInfo> FindFirstTrackerPathBlock(Point startPosition, const int8_t *path, size_t steps, bool considerDoors, bool considerMonsters, bool considerBreakables)
{
	Point position = startPosition;
	for (size_t i = 0; i < steps; ++i) {
		const Point next = NextPositionForWalkDirection(position, path[i]);
		Object *object = FindObjectAtPosition(next);
		if (considerDoors && object != nullptr && object->isDoor() && object->_oVar4 == DOOR_CLOSED) {
			return TrackerPathBlockInfo {
				.type = TrackerPathBlockType::Door,
				.stepIndex = i,
				.beforeBlock = position,
				.blockPosition = object->position,
			};
		}
		if (considerBreakables && object != nullptr && object->_oSolidFlag && object->IsBreakable()) {
			return TrackerPathBlockInfo {
				.type = TrackerPathBlockType::Breakable,
				.stepIndex = i,
				.beforeBlock = position,
				.blockPosition = next,
			};
		}

		if (considerMonsters && leveltype != DTYPE_TOWN && dMonster[next.x][next.y] != 0) {
			const int monsterRef = dMonster[next.x][next.y];
			const int monsterId = std::abs(monsterRef) - 1;
			const bool blocks = monsterRef <= 0 || (monsterId >= 0 && monsterId < static_cast<int>(MaxMonsters) && !Monsters[monsterId].hasNoLife());
			if (blocks) {
				return TrackerPathBlockInfo {
					.type = TrackerPathBlockType::Monster,
					.stepIndex = i,
					.beforeBlock = position,
					.blockPosition = next,
				};
			}
		}

		position = next;
	}

	return std::nullopt;
}

/** A route to a goal, and what it passes through that the player has to clear first. */
struct TrackerRoute {
	std::vector<int8_t> path;
	bool passesDoors = false;
	bool passesMonsters = false;
	bool passesBreakables = false;
};

/**
 * The route to the nearest tile where `isGoal` holds, searched over the whole level, by the strictest
 * rule that finds one: around closed doors, monsters and barrels first, then through doors, then past
 * monsters, then both, then through barrels as well. Spoken routes prefer steps along the axes.
 */
[[nodiscard]] std::optional<TrackerRoute> FindTrackerRoute(const Player &player, Point start, Point heading, tl::function_ref<bool(Point)> isGoal, bool preferAxisSteps)
{
	struct Rule {
		bool (*posOk)(const Player &, Point);
		bool doors;
		bool monsters;
		bool breakables;
	};
	static constexpr std::array<Rule, 5> Rules { {
		{ PosOkPlayer, false, false, false },
		{ PosOkPlayerIgnoreDoors, true, false, false },
		{ PosOkPlayerIgnoreMonsters, false, true, false },
		{ PosOkPlayerIgnoreDoorsAndMonsters, true, true, false },
		{ PosOkPlayerIgnoreDoorsMonstersAndBreakables, true, true, true },
	} };
	for (const Rule &rule : Rules) {
		std::optional<std::vector<int8_t>> path = FindWalkPathToGoal(player, start, heading, isGoal, rule.posOk, preferAxisSteps);
		if (path)
			return TrackerRoute { .path = std::move(*path), .passesDoors = rule.doors, .passesMonsters = rule.monsters, .passesBreakables = rule.breakables };
	}
	return std::nullopt;
}

/** Cuts a route short before the first closed door, monster or barrel on it, and returns that. */
std::optional<TrackerPathBlockInfo> StopRouteAtFirstBlock(TrackerRoute &route, Point start)
{
	if (!route.passesDoors && !route.passesMonsters && !route.passesBreakables)
		return std::nullopt;
	const std::optional<TrackerPathBlockInfo> block = FindFirstTrackerPathBlock(start, route.path.data(), route.path.size(), route.passesDoors, route.passesMonsters, route.passesBreakables);
	if (block)
		route.path.resize(block->stepIndex);
	return block;
}

[[nodiscard]] std::string_view TrackerPathBlockMessage(TrackerPathBlockType type)
{
	switch (type) {
	case TrackerPathBlockType::Door:
		return _("A door is blocking the path. Open it and try again.");
	case TrackerPathBlockType::Monster:
		return _("A monster is blocking the path. Clear it and try again.");
	case TrackerPathBlockType::Breakable:
		return _("A breakable object is blocking the path. Destroy it and try again.");
	}
	return {};
}

/** Said after directions that end at something in the way. */
[[nodiscard]] std::string_view TrackerPathBlockAheadLabel(TrackerPathBlockType type)
{
	switch (type) {
	case TrackerPathBlockType::Door:
		return _("then a closed door");
	case TrackerPathBlockType::Monster:
		return _("then a monster");
	case TrackerPathBlockType::Breakable:
		return _("then a breakable object");
	}
	return {};
}

/**
 * Where along `path` the game is told to walk. The game's own path search looks at a limited number
 * of tiles (engine/path.cpp) and gives up on long or winding routes, so it gets the farthest point on
 * this route that it finds its way to. Empty if not even the first step works.
 */
[[nodiscard]] std::optional<Point> ChooseAutoWalkWaypoint(const Player &player, Point start, const std::vector<int8_t> &path)
{
	std::array<int8_t, MaxPathLengthPlayer> enginePath;
	// One order walks at most MaxPathLengthPlayer - 1 steps.
	for (size_t steps = std::min(path.size(), MaxPathLengthPlayer - 1); steps > 0; steps /= 2) {
		const Point waypoint = PositionAfterWalkPathSteps(start, path.data(), static_cast<int>(steps));
		if (FindPath(CanStep, [&player](Point position) { return PosOkPlayer(player, position); }, start, waypoint, enginePath.data(), enginePath.size()) != 0)
			return waypoint;
	}
	return std::nullopt;
}

/** Whether a red portal (the way out of Lazarus' lair) stands on the tile. */
[[nodiscard]] bool IsRedPortalAt(Point position)
{
	return std::any_of(Missiles.begin(), Missiles.end(), [position](const Missile &missile) {
		return missile._mitype == MissileID::RedPortal && missile.position.tile == position;
	});
}

/** One step of Shift+Home's walk. */
struct AutoWalkStep {
	/** Where to send the player next. Empty when the walk ends. */
	std::optional<Point> walkTo;
	/** What to say when the walk ends. */
	std::string stopMessage;
};

[[nodiscard]] AutoWalkStep StopAutoWalk(std::string_view message)
{
	return { .walkTo = std::nullopt, .stopMessage = std::string(message) };
}

[[nodiscard]] AutoWalkStep WalkTo(Point position)
{
	return { .walkTo = position, .stopMessage = {} };
}

/** Whether an object tracker target still exists and still belongs to its category. */
template <typename Predicate>
[[nodiscard]] bool IsTrackedObjectPresent(int objectId, Predicate isValid)
{
	return objectId >= 0 && objectId < MAXOBJECTS && isValid(Objects[objectId]);
}

/**
 * Resolves which object to walk toward for the given tracker category.
 */
template <typename Predicate, typename FindNearest, typename GetName>
std::optional<int> ResolveObjectTrackerTarget(
    int &lockedTargetId, Point playerPosition,
    Predicate isValid, FindNearest findNearest, GetName getName,
    const char *notFoundMessage, StringOrView &targetName)
{
	std::optional<int> targetId;
	if (lockedTargetId >= 0 && lockedTargetId < MAXOBJECTS) {
		targetId = lockedTargetId;
	} else {
		targetId = findNearest(playerPosition);
	}
	if (!targetId) {
		SpeakText(_(notFoundMessage), true);
		return std::nullopt;
	}
	if (!isValid(Objects[*targetId])) {
		lockedTargetId = -1;
		targetId = findNearest(playerPosition);
		if (!targetId) {
			SpeakText(_(notFoundMessage), true);
			return std::nullopt;
		}
		if (!isValid(Objects[*targetId])) {
			SpeakText(_(notFoundMessage), true);
			return std::nullopt;
		}
	}
	lockedTargetId = *targetId;
	targetName = getName(*targetId);
	return targetId;
}

[[nodiscard]] std::vector<TrackerTargetCategory> TrackerTargetCategoriesForCurrentLevel()
{
	if (leveltype == DTYPE_TOWN) {
		return {
			TrackerTargetCategory::Items,
			TrackerTargetCategory::DeadBodies,
			TrackerTargetCategory::Npcs,
			TrackerTargetCategory::Players,
			TrackerTargetCategory::DungeonEntrances,
			TrackerTargetCategory::QuestLocations,
			TrackerTargetCategory::Portals,
		};
	}

	return {
		TrackerTargetCategory::Items,
		TrackerTargetCategory::Chests,
		TrackerTargetCategory::Doors,
		TrackerTargetCategory::Shrines,
		TrackerTargetCategory::Objects,
		TrackerTargetCategory::Breakables,
		TrackerTargetCategory::Monsters,
		TrackerTargetCategory::DeadBodies,
		TrackerTargetCategory::DungeonEntrances,
		TrackerTargetCategory::Stairs,
		TrackerTargetCategory::QuestLocations,
		TrackerTargetCategory::Players,
		TrackerTargetCategory::Portals,
	};
}

void SelectTrackerTargetCategoryRelative(int delta)
{
	if (!CanPlayerTakeAction() || InGameMenu())
		return;

	AutoWalkTrackerTargetId = -1;

	const std::vector<TrackerTargetCategory> categories = TrackerTargetCategoriesForCurrentLevel();
	if (categories.empty())
		return;

	auto it = std::find(categories.begin(), categories.end(), SelectedTrackerTargetCategory);
	int currentIndex = 0;
	if (it == categories.end()) {
		currentIndex = delta > 0 ? -1 : 0;
	} else {
		currentIndex = static_cast<int>(it - categories.begin());
	}

	const int count = static_cast<int>(categories.size());
	int newIndex = (currentIndex + delta) % count;
	if (newIndex < 0)
		newIndex += count;

	SelectedTrackerTargetCategory = categories[static_cast<size_t>(newIndex)];
	SpeakTrackerTargetCategory();
}

[[nodiscard]] std::vector<TrackerCandidate> CollectTrackerCandidatesForSelection(TrackerTargetCategory category, Point playerPosition)
{
	switch (category) {
	case TrackerTargetCategory::Items:
		return CollectNearbyItemTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
	case TrackerTargetCategory::Chests:
		return CollectNearbyChestTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
	case TrackerTargetCategory::Doors: {
		std::vector<TrackerCandidate> candidates = CollectNearbyDoorTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
		for (TrackerCandidate &c : candidates) {
			if (c.id < 0 || c.id >= MAXOBJECTS)
				continue;
			c.name = DoorLabelForSpeech(Objects[c.id]);
		}
		return candidates;
	}
	case TrackerTargetCategory::Shrines:
		return CollectNearbyShrineTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
	case TrackerTargetCategory::Objects:
		return CollectNearbyObjectInteractableTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
	case TrackerTargetCategory::Breakables:
		return CollectNearbyBreakableTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
	case TrackerTargetCategory::Monsters:
		return CollectNearbyMonsterTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
	case TrackerTargetCategory::DeadBodies:
		return CollectNearbyCorpseTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
	case TrackerTargetCategory::Npcs:
		return CollectNpcTrackerCandidates(playerPosition);
	case TrackerTargetCategory::Players:
		return CollectPlayerTrackerCandidates(playerPosition);
	case TrackerTargetCategory::DungeonEntrances:
		return CollectDungeonEntranceTrackerCandidates(playerPosition);
	case TrackerTargetCategory::Stairs:
		return CollectStairsTrackerCandidates(playerPosition);
	case TrackerTargetCategory::QuestLocations:
		return CollectQuestLocationTrackerCandidates(playerPosition);
	case TrackerTargetCategory::Portals:
		return CollectPortalTrackerCandidates(playerPosition);
	}
	app_fatal("Invalid TrackerTargetCategory");
}

[[nodiscard]] std::string_view TrackerCategoryNoCandidatesFoundMessage(TrackerTargetCategory category)
{
	switch (category) {
	case TrackerTargetCategory::Items:
		return _("No items found.");
	case TrackerTargetCategory::Chests:
		return _("No chests found.");
	case TrackerTargetCategory::Doors:
		return _("No doors found.");
	case TrackerTargetCategory::Shrines:
		return _("No shrines found.");
	case TrackerTargetCategory::Objects:
		return _("No objects found.");
	case TrackerTargetCategory::Breakables:
		return _("No breakables found.");
	case TrackerTargetCategory::Monsters:
		return _("No monsters found.");
	case TrackerTargetCategory::DeadBodies:
		return _("No dead bodies found.");
	case TrackerTargetCategory::Npcs:
		return _("No NPCs found.");
	case TrackerTargetCategory::Players:
		return _("No players found.");
	case TrackerTargetCategory::DungeonEntrances:
		if (leveltype != DTYPE_TOWN)
			return _("No exits found.");
		return _("No dungeon entrances found.");
	case TrackerTargetCategory::Stairs:
		return _("No stairs found.");
	case TrackerTargetCategory::QuestLocations:
		return _("No quest locations found.");
	case TrackerTargetCategory::Portals:
		return _("No portals found.");
	}
	app_fatal("Invalid TrackerTargetCategory");
}

[[nodiscard]] constexpr bool TrackerCategorySelectionIsProximityLimited(TrackerTargetCategory category)
{
	return IsAnyOf(category, TrackerTargetCategory::Items, TrackerTargetCategory::Chests, TrackerTargetCategory::Doors, TrackerTargetCategory::Shrines, TrackerTargetCategory::Objects,
	    TrackerTargetCategory::Breakables, TrackerTargetCategory::Monsters, TrackerTargetCategory::DeadBodies);
}

[[nodiscard]] bool TrackerCategoryHasAnyTargets(TrackerTargetCategory category, Point playerPosition)
{
	switch (category) {
	case TrackerTargetCategory::Items:
		return FindNearestGroundItemId(playerPosition).has_value();
	case TrackerTargetCategory::Chests:
		return FindNearestUnopenedChestObjectId(playerPosition).has_value();
	case TrackerTargetCategory::Doors:
		return FindNearestDoorObjectId(playerPosition).has_value();
	case TrackerTargetCategory::Shrines:
		return FindNearestShrineObjectId(playerPosition).has_value();
	case TrackerTargetCategory::Objects:
		return FindNearestMiscInteractableObjectId(playerPosition).has_value();
	case TrackerTargetCategory::Breakables:
		return FindNearestBreakableObjectId(playerPosition).has_value();
	case TrackerTargetCategory::Monsters:
		return FindNearestMonsterId(playerPosition).has_value();
	case TrackerTargetCategory::DeadBodies:
		return FindNearestCorpseId(playerPosition).has_value();
	default:
		return false;
	}
}

[[nodiscard]] std::string_view TrackerCategoryNoNearbyCandidatesFoundMessage(TrackerTargetCategory category)
{
	switch (category) {
	case TrackerTargetCategory::Items:
		return _("No nearby items found.");
	case TrackerTargetCategory::Chests:
		return _("No nearby chests found.");
	case TrackerTargetCategory::Doors:
		return _("No nearby doors found.");
	case TrackerTargetCategory::Shrines:
		return _("No nearby shrines found.");
	case TrackerTargetCategory::Objects:
		return _("No nearby objects found.");
	case TrackerTargetCategory::Breakables:
		return _("No nearby breakables found.");
	case TrackerTargetCategory::Monsters:
		return _("No nearby monsters found.");
	case TrackerTargetCategory::DeadBodies:
		return _("No nearby dead bodies found.");
	default:
		return TrackerCategoryNoCandidatesFoundMessage(category);
	}
}

[[nodiscard]] std::string_view TrackerCategoryNoNextMessage(TrackerTargetCategory category)
{
	switch (category) {
	case TrackerTargetCategory::Items:
		return _("No next item.");
	case TrackerTargetCategory::Chests:
		return _("No next chest.");
	case TrackerTargetCategory::Doors:
		return _("No next door.");
	case TrackerTargetCategory::Shrines:
		return _("No next shrine.");
	case TrackerTargetCategory::Objects:
		return _("No next object.");
	case TrackerTargetCategory::Breakables:
		return _("No next breakable.");
	case TrackerTargetCategory::Monsters:
		return _("No next monster.");
	case TrackerTargetCategory::DeadBodies:
		return _("No next dead body.");
	case TrackerTargetCategory::Npcs:
		return _("No next NPC.");
	case TrackerTargetCategory::Players:
		return _("No next player.");
	case TrackerTargetCategory::DungeonEntrances:
		return _("No next dungeon entrance.");
	case TrackerTargetCategory::Stairs:
		return _("No next stairs.");
	case TrackerTargetCategory::QuestLocations:
		return _("No next quest location.");
	case TrackerTargetCategory::Portals:
		return _("No next portal.");
	}
	app_fatal("Invalid TrackerTargetCategory");
}

[[nodiscard]] std::string_view TrackerCategoryNoPreviousMessage(TrackerTargetCategory category)
{
	switch (category) {
	case TrackerTargetCategory::Items:
		return _("No previous item.");
	case TrackerTargetCategory::Chests:
		return _("No previous chest.");
	case TrackerTargetCategory::Doors:
		return _("No previous door.");
	case TrackerTargetCategory::Shrines:
		return _("No previous shrine.");
	case TrackerTargetCategory::Objects:
		return _("No previous object.");
	case TrackerTargetCategory::Breakables:
		return _("No previous breakable.");
	case TrackerTargetCategory::Monsters:
		return _("No previous monster.");
	case TrackerTargetCategory::DeadBodies:
		return _("No previous dead body.");
	case TrackerTargetCategory::Npcs:
		return _("No previous NPC.");
	case TrackerTargetCategory::Players:
		return _("No previous player.");
	case TrackerTargetCategory::DungeonEntrances:
		return _("No previous dungeon entrance.");
	case TrackerTargetCategory::Stairs:
		return _("No previous stairs.");
	case TrackerTargetCategory::QuestLocations:
		return _("No previous quest location.");
	case TrackerTargetCategory::Portals:
		return _("No previous portal.");
	}
	app_fatal("Invalid TrackerTargetCategory");
}

/**
 * Returns true if the given tracker category requires a dungeon (i.e. is not
 * available in town).
 */
[[nodiscard]] bool IsDungeonOnlyTrackerCategory(TrackerTargetCategory category)
{
	return IsNoneOf(category, TrackerTargetCategory::Items, TrackerTargetCategory::DeadBodies,
	    TrackerTargetCategory::Npcs, TrackerTargetCategory::Players,
	    TrackerTargetCategory::DungeonEntrances, TrackerTargetCategory::QuestLocations, TrackerTargetCategory::Portals);
}

void SelectTrackerTargetRelative(int delta)
{
	if (!CanPlayerTakeAction() || InGameMenu())
		return;
	if (MyPlayer == nullptr)
		return;

	if (leveltype == DTYPE_TOWN && IsDungeonOnlyTrackerCategory(SelectedTrackerTargetCategory)) {
		SpeakText(_("Not in a dungeon."), true);
		return;
	}
	if (AutomapActive) {
		SpeakText(_("Close the map first."), true);
		return;
	}

	EnsureTrackerLocksMatchCurrentLevel();

	const Point playerPosition = MyPlayer->position.future;
	AutoWalkTrackerTargetId = -1;

	const std::vector<TrackerCandidate> candidates = CollectTrackerCandidatesForSelection(SelectedTrackerTargetCategory, playerPosition);
	if (candidates.empty()) {
		LockedTrackerTargetId(SelectedTrackerTargetCategory) = -1;
		if (TrackerCategorySelectionIsProximityLimited(SelectedTrackerTargetCategory) && TrackerCategoryHasAnyTargets(SelectedTrackerTargetCategory, playerPosition))
			SpeakText(TrackerCategoryNoNearbyCandidatesFoundMessage(SelectedTrackerTargetCategory), true);
		else
			SpeakText(TrackerCategoryNoCandidatesFoundMessage(SelectedTrackerTargetCategory), true);
		return;
	}

	int &lockedTargetId = LockedTrackerTargetId(SelectedTrackerTargetCategory);
	if (candidates.size() == 1) {
		lockedTargetId = candidates.front().id;
		SpeakText(candidates.front().name.str(), /*force=*/true);
		return;
	}
	const std::optional<int> targetId = delta > 0 ? FindNextTrackerCandidateId(candidates, lockedTargetId) : FindPreviousTrackerCandidateId(candidates, lockedTargetId);
	if (!targetId) {
		SpeakText(delta > 0 ? TrackerCategoryNoNextMessage(SelectedTrackerTargetCategory) : TrackerCategoryNoPreviousMessage(SelectedTrackerTargetCategory), true);
		return;
	}

	const auto it = std::find_if(candidates.begin(), candidates.end(), [id = *targetId](const TrackerCandidate &c) { return c.id == id; });
	if (it == candidates.end()) {
		lockedTargetId = -1;
		SpeakText(TrackerCategoryNoCandidatesFoundMessage(SelectedTrackerTargetCategory), true);
		return;
	}

	lockedTargetId = *targetId;
	StringOrView targetName = it->name.str();
	DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, candidates);
	SpeakText(targetName.str(), /*force=*/true);
}

} // namespace

namespace {

/** Spoken directions from the player to a target, or why there are none. */
std::string DescribeRouteToTrackerTarget(const Player &player, Point playerPosition, const TrackerGoal &goal, std::string_view targetName)
{
	std::optional<TrackerRoute> route = FindTrackerRoute(player, playerPosition, goal.position, [&goal](Point tile) { return goal.IsReachedFrom(tile); }, /*preferAxisSteps=*/true);

	std::string message;
	if (!targetName.empty())
		StrAppend(message, targetName, "\n");

	if (!route) {
		message.append(_("Can't find a path to the target."));
		Point closestPosition;
		const std::optional<std::vector<int8_t>> partialPath = FindKeyboardWalkPathToClosestReachableForSpeech(player, playerPosition, goal.position, closestPosition);
		if (partialPath && !partialPath->empty()) {
			message.append("\n");
			AppendKeyboardWalkPathForSpeech(message, *partialPath);
		}
		return message;
	}

	const std::optional<TrackerPathBlockInfo> block = StopRouteAtFirstBlock(*route, playerPosition);
	if (block && playerPosition.WalkingDistance(block->blockPosition) <= TrackerInteractDistanceTiles)
		return std::string(TrackerPathBlockMessage(block->type));

	AppendKeyboardWalkPathForSpeech(message, route->path);
	if (block)
		StrAppend(message, ", ", TrackerPathBlockAheadLabel(block->type));
	return message;
}

/**
 * What Home says about the selected target: the route to it, or why there is none. With
 * `cycleTarget`, only its name.
 */
std::string DescribeSelectedTrackerTarget(bool cycleTarget)
{
	const Point playerPosition = MyPlayer->position.future;
	int &lockedTargetId = LockedTrackerTargetId(SelectedTrackerTargetCategory);

	std::optional<int> targetId;
	std::optional<TrackerGoal> goal;
	StringOrView targetName;

	switch (SelectedTrackerTargetCategory) {
	case TrackerTargetCategory::Items: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectNearbyItemTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No items found.") : _("No next item."));
		} else if (IsGroundItemPresent(lockedTargetId)) {
			targetId = lockedTargetId;
		} else {
			targetId = FindNearestGroundItemId(playerPosition);
		}
		if (!targetId)
			return std::string(_("No items found."));

		if (!IsGroundItemPresent(*targetId)) {
			lockedTargetId = -1;
			return std::string(_("No items found."));
		}

		lockedTargetId = *targetId;
		const Item &tracked = Items[*targetId];

		targetName = tracked.getName();
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		goal = TileGoal(tracked.position);
		break;
	}
	case TrackerTargetCategory::Chests: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectNearbyChestTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No chests found.") : _("No next chest."));
		} else if (lockedTargetId >= 0 && lockedTargetId < MAXOBJECTS) {
			targetId = lockedTargetId;
		} else {
			targetId = FindNearestUnopenedChestObjectId(playerPosition);
		}
		if (!targetId)
			return std::string(_("No chests found."));

		const Object &object = Objects[*targetId];
		if (!IsTrackedChestObject(object)) {
			lockedTargetId = -1;
			targetId = FindNearestUnopenedChestObjectId(playerPosition);
			if (!targetId)
				return std::string(_("No chests found."));
		}

		lockedTargetId = *targetId;
		const Object &tracked = Objects[*targetId];

		targetName = tracked.name();
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			goal = ObjectGoal(tracked);
		}
		break;
	}
	case TrackerTargetCategory::Doors: {
		std::vector<TrackerCandidate> nearbyCandidates = CollectNearbyDoorTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
		for (TrackerCandidate &c : nearbyCandidates) {
			if (c.id < 0 || c.id >= MAXOBJECTS)
				continue;
			c.name = DoorLabelForSpeech(Objects[c.id]);
		}
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No doors found.") : _("No next door."));
		} else if (lockedTargetId >= 0 && lockedTargetId < MAXOBJECTS) {
			targetId = lockedTargetId;
		} else {
			targetId = FindNearestDoorObjectId(playerPosition);
		}
		if (!targetId)
			return std::string(_("No doors found."));

		const Object &object = Objects[*targetId];
		if (!IsTrackedDoorObject(object)) {
			lockedTargetId = -1;
			targetId = FindNearestDoorObjectId(playerPosition);
			if (!targetId)
				return std::string(_("No doors found."));
		}

		lockedTargetId = *targetId;
		const Object &tracked = Objects[*targetId];

		targetName = DoorLabelForSpeech(tracked);
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			goal = ObjectGoal(tracked);
		}
		break;
	}
	case TrackerTargetCategory::Shrines: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectNearbyShrineTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No shrines found.") : _("No next shrine."));
		} else if (lockedTargetId >= 0 && lockedTargetId < MAXOBJECTS) {
			targetId = lockedTargetId;
		} else {
			targetId = FindNearestShrineObjectId(playerPosition);
		}
		if (!targetId)
			return std::string(_("No shrines found."));

		const Object &object = Objects[*targetId];
		if (!IsShrineLikeObject(object)) {
			lockedTargetId = -1;
			targetId = FindNearestShrineObjectId(playerPosition);
			if (!targetId)
				return std::string(_("No shrines found."));
		}

		lockedTargetId = *targetId;
		const Object &tracked = Objects[*targetId];

		targetName = tracked.name();
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			goal = ObjectGoal(tracked);
		}
		break;
	}
	case TrackerTargetCategory::Objects: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectNearbyObjectInteractableTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No objects found.") : _("No next object."));
		} else if (lockedTargetId >= 0 && lockedTargetId < MAXOBJECTS) {
			targetId = lockedTargetId;
		} else {
			targetId = FindNearestMiscInteractableObjectId(playerPosition);
		}
		if (!targetId)
			return std::string(_("No objects found."));

		const Object &object = Objects[*targetId];
		if (!IsTrackedMiscInteractableObject(object)) {
			lockedTargetId = -1;
			targetId = FindNearestMiscInteractableObjectId(playerPosition);
			if (!targetId)
				return std::string(_("No objects found."));
		}

		lockedTargetId = *targetId;
		const Object &tracked = Objects[*targetId];

		targetName = TrackerObjectLabelForSpeech(tracked);
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			goal = ObjectGoal(tracked);
		}
		break;
	}
	case TrackerTargetCategory::Breakables: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectNearbyBreakableTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No breakables found.") : _("No next breakable."));
		} else if (lockedTargetId >= 0 && lockedTargetId < MAXOBJECTS) {
			targetId = lockedTargetId;
		} else {
			targetId = FindNearestBreakableObjectId(playerPosition);
		}
		if (!targetId)
			return std::string(_("No breakables found."));

		const Object &object = Objects[*targetId];
		if (!IsTrackedBreakableObject(object)) {
			lockedTargetId = -1;
			targetId = FindNearestBreakableObjectId(playerPosition);
			if (!targetId)
				return std::string(_("No breakables found."));
		}

		lockedTargetId = *targetId;
		const Object &tracked = Objects[*targetId];

		targetName = tracked.name();
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			goal = ObjectGoal(tracked);
		}
		break;
	}
	case TrackerTargetCategory::Monsters: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectNearbyMonsterTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No monsters found.") : _("No next monster."));
		} else if (lockedTargetId >= 0 && lockedTargetId < static_cast<int>(MaxMonsters)) {
			targetId = lockedTargetId;
		} else {
			targetId = FindNearestMonsterId(playerPosition);
		}
		if (!targetId)
			return std::string(_("No monsters found."));

		const Monster &monster = Monsters[*targetId];
		if (!IsTrackedMonster(monster)) {
			lockedTargetId = -1;
			targetId = FindNearestMonsterId(playerPosition);
			if (!targetId)
				return std::string(_("No monsters found."));
		}

		lockedTargetId = *targetId;
		const Monster &tracked = Monsters[*targetId];

		targetName = MonsterLabelForSpeech(tracked);
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			goal = OccupiedTileGoal(tracked.position.tile);
		}
		break;
	}
	case TrackerTargetCategory::DeadBodies: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectNearbyCorpseTrackerCandidates(playerPosition, TrackerCycleDistanceTiles);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No dead bodies found.") : _("No next dead body."));
		} else if (IsCorpsePresent(lockedTargetId)) {
			targetId = lockedTargetId;
		} else {
			targetId = FindNearestCorpseId(playerPosition);
		}
		if (!targetId)
			return std::string(_("No dead bodies found."));

		if (!IsCorpsePresent(*targetId)) {
			lockedTargetId = -1;
			return std::string(_("No dead bodies found."));
		}

		lockedTargetId = *targetId;
		targetName = _("Dead body");
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			goal = TileGoal(CorpsePositionForTrackerId(*targetId));
		}
		break;
	}
	case TrackerTargetCategory::Npcs: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectNpcTrackerCandidates(playerPosition);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No NPCs found.") : _("No next NPC."));
		} else if (lockedTargetId >= 0 && lockedTargetId < static_cast<int>(GetNumTowners())) {
			targetId = lockedTargetId;
		} else if (!nearbyCandidates.empty()) {
			targetId = nearbyCandidates.front().id;
		}
		if (!targetId)
			return std::string(_("No NPCs found."));

		const auto it = std::find_if(nearbyCandidates.begin(), nearbyCandidates.end(), [id = *targetId](const TrackerCandidate &c) { return c.id == id; });
		if (it == nearbyCandidates.end()) {
			lockedTargetId = -1;
			return std::string(_("No NPCs found."));
		}

		lockedTargetId = *targetId;
		targetName = Towners[*targetId].name;
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			goal = OccupiedTileGoal(Towners[*targetId].position);
		}
		break;
	}
	case TrackerTargetCategory::Players: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectPlayerTrackerCandidates(playerPosition);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No players found.") : _("No next player."));
		} else if (lockedTargetId >= 0 && lockedTargetId < MAX_PLRS) {
			targetId = lockedTargetId;
		} else if (!nearbyCandidates.empty()) {
			targetId = nearbyCandidates.front().id;
		}
		if (!targetId)
			return std::string(_("No players found."));

		const auto it = std::find_if(nearbyCandidates.begin(), nearbyCandidates.end(), [id = *targetId](const TrackerCandidate &c) { return c.id == id; });
		if (it == nearbyCandidates.end()) {
			lockedTargetId = -1;
			return std::string(_("No players found."));
		}

		lockedTargetId = *targetId;
		targetName = Players[*targetId].name();
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			goal = OccupiedTileGoal(Players[*targetId].position.future);
		}
		break;
	}
	case TrackerTargetCategory::DungeonEntrances: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectDungeonEntranceTrackerCandidates(playerPosition);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No dungeon entrances found.") : _("No next dungeon entrance."));
		} else if (!nearbyCandidates.empty()) {
			const auto lockedIt = std::find_if(nearbyCandidates.begin(), nearbyCandidates.end(), [id = lockedTargetId](const TrackerCandidate &c) { return c.id == id; });
			targetId = lockedIt != nearbyCandidates.end() ? lockedTargetId : nearbyCandidates.front().id;
		}
		if (!targetId)
			return std::string(_("No dungeon entrances found."));

		const auto it = std::find_if(nearbyCandidates.begin(), nearbyCandidates.end(), [id = *targetId](const TrackerCandidate &c) { return c.id == id; });
		if (it == nearbyCandidates.end()) {
			lockedTargetId = -1;
			return std::string(_("No dungeon entrances found."));
		}

		lockedTargetId = *targetId;
		targetName = std::string(it->name.str());
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			if (IsRedPortalTrackerId(*targetId)) {
				goal = TileGoal(RedPortalPositionForTrackerId(*targetId));
			} else {
				const TriggerStruct &trigger = trigs[*targetId];
				goal = TileGoal(trigger.position);
			}
		}
		break;
	}
	case TrackerTargetCategory::Stairs: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectStairsTrackerCandidates(playerPosition);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No stairs found.") : _("No next stairs."));
		} else if (lockedTargetId >= 0 && lockedTargetId < numtrigs) {
			targetId = lockedTargetId;
		} else if (!nearbyCandidates.empty()) {
			targetId = nearbyCandidates.front().id;
		}
		if (!targetId)
			return std::string(_("No stairs found."));

		const auto it = std::find_if(nearbyCandidates.begin(), nearbyCandidates.end(), [id = *targetId](const TrackerCandidate &c) { return c.id == id; });
		if (it == nearbyCandidates.end()) {
			lockedTargetId = -1;
			return std::string(_("No stairs found."));
		}

		lockedTargetId = *targetId;
		targetName = std::string(it->name.str());
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			const TriggerStruct &trigger = trigs[*targetId];
			goal = TileGoal(trigger.position);
		}
		break;
	}
	case TrackerTargetCategory::QuestLocations: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectQuestLocationTrackerCandidates(playerPosition);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No quest locations found.") : _("No next quest location."));
		} else if (!nearbyCandidates.empty()) {
			const auto lockedIt = std::find_if(nearbyCandidates.begin(), nearbyCandidates.end(), [id = lockedTargetId](const TrackerCandidate &c) { return c.id == id; });
			targetId = lockedIt != nearbyCandidates.end() ? lockedTargetId : nearbyCandidates.front().id;
		}
		if (!targetId)
			return std::string(_("No quest locations found."));

		const auto it = std::find_if(nearbyCandidates.begin(), nearbyCandidates.end(), [id = *targetId](const TrackerCandidate &c) { return c.id == id; });
		if (it == nearbyCandidates.end()) {
			lockedTargetId = -1;
			return std::string(_("No quest locations found."));
		}

		lockedTargetId = *targetId;
		targetName = std::string(it->name.str());
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			if (IsQuestSpotTrackerId(*targetId)) {
				const std::optional<QuestSpot> spot = FindQuestSpot(QuestSpotIdForTrackerId(*targetId));
				if (!spot) {
					lockedTargetId = -1;
					return std::string(_("No quest locations found."));
				}
				if (IsQuestSpotInRange(*spot, playerPosition))
					return std::string(QuestSpotInRangeMessage(*spot, *MyPlayer));
				goal = QuestSpotGoal(*spot);
			} else if (setlevel) {
				if (IsRedPortalTrackerId(*targetId)) {
					goal = TileGoal(RedPortalPositionForTrackerId(*targetId));
				} else {
					const TriggerStruct &trigger = trigs[*targetId];
					goal = TileGoal(trigger.position);
				}
			} else {
				const Quest &quest = Quests[static_cast<size_t>(*targetId)];
				goal = TileGoal(quest.position);
			}
		}
		break;
	}
	case TrackerTargetCategory::Portals: {
		const std::vector<TrackerCandidate> nearbyCandidates = CollectPortalTrackerCandidates(playerPosition);
		if (cycleTarget) {
			targetId = FindNextTrackerCandidateId(nearbyCandidates, lockedTargetId);
			if (!targetId)
				return std::string(nearbyCandidates.empty() ? _("No portals found.") : _("No next portal."));
		} else if (lockedTargetId >= 0 && lockedTargetId < MAXPORTAL) {
			targetId = lockedTargetId;
		} else if (!nearbyCandidates.empty()) {
			targetId = nearbyCandidates.front().id;
		}
		if (!targetId)
			return std::string(_("No portals found."));

		const auto it = std::find_if(nearbyCandidates.begin(), nearbyCandidates.end(), [id = *targetId](const TrackerCandidate &c) { return c.id == id; });
		if (it == nearbyCandidates.end()) {
			lockedTargetId = -1;
			return std::string(_("No portals found."));
		}

		Point portalPosition;
		if (leveltype == DTYPE_TOWN) {
			const std::optional<Point> townPos = FindTownPortalPositionInTownByPortalIndex(*targetId);
			if (!townPos) {
				lockedTargetId = -1;
				return std::string(_("No portals found."));
			}
			portalPosition = *townPos;
		} else {
			if (!IsTownPortalOpenOnCurrentLevel(*targetId)) {
				lockedTargetId = -1;
				return std::string(_("No portals found."));
			}
			portalPosition = Portals[*targetId].position;
		}

		lockedTargetId = *targetId;
		targetName = TownPortalLabelForSpeech(Portals[*targetId]);
		DecorateTrackerTargetNameWithOrdinalIfNeeded(*targetId, targetName, nearbyCandidates);
		if (!cycleTarget) {
			goal = TileGoal(portalPosition);
		}
		break;
	}
	}

	if (cycleTarget)
		return std::string(targetName.str());

	if (!goal)
		return std::string(_("Can't find a path to the target."));

	return DescribeRouteToTrackerTarget(*MyPlayer, playerPosition, *goal, targetName.str());
}

void NavigateToTrackerTargetKeyPressed()
{
	if (!CanPlayerTakeAction() || InGameMenu())
		return;
	if (leveltype == DTYPE_TOWN && IsDungeonOnlyTrackerCategory(SelectedTrackerTargetCategory)) {
		SpeakText(_("Not in a dungeon."), true);
		return;
	}
	if (AutomapActive) {
		SpeakText(_("Close the map first."), true);
		return;
	}
	if (MyPlayer == nullptr)
		return;

	EnsureTrackerLocksMatchCurrentLevel();

	const SDL_Keymod modState = SDL_GetModState();
	const bool cycleTarget = (modState & SDL_KMOD_SHIFT) != 0;
	const bool clearTarget = (modState & SDL_KMOD_CTRL) != 0;

	AutoWalkTrackerTargetId = -1;

	if (clearTarget) {
		LockedTrackerTargetId(SelectedTrackerTargetCategory) = -1;
		SpeakText(_("Tracker target cleared."), true);
		return;
	}

	SpeakText(DescribeSelectedTrackerTarget(cycleTarget), true);
}

void AutoWalkToTrackerTargetKeyPressed()
{
	if (AutoWalkTrackerTargetId >= 0) {
		CancelAutoWalk();
		SpeakText(_("Walk cancelled."), true);
		return;
	}

	if (!CanPlayerTakeAction() || InGameMenu())
		return;

	if (leveltype == DTYPE_TOWN && IsDungeonOnlyTrackerCategory(SelectedTrackerTargetCategory)) {
		SpeakText(_("Not in a dungeon."), true);
		return;
	}
	if (AutomapActive) {
		SpeakText(_("Close the map first."), true);
		return;
	}
	if (MyPlayer == nullptr) {
		AutoWalkTrackerTargetId = -1;
		SpeakText(_("Cannot walk right now."), true);
		return;
	}

	EnsureTrackerLocksMatchCurrentLevel();

	const Point playerPosition = MyPlayer->position.future;
	int &lockedTargetId = LockedTrackerTargetId(SelectedTrackerTargetCategory);

	std::optional<int> targetId;
	StringOrView targetName;

	switch (SelectedTrackerTargetCategory) {
	case TrackerTargetCategory::Items: {
		if (IsGroundItemPresent(lockedTargetId)) {
			targetId = lockedTargetId;
		} else {
			targetId = FindNearestGroundItemId(playerPosition);
		}
		if (!targetId) {
			SpeakText(_("No items found."), true);
			return;
		}
		if (!IsGroundItemPresent(*targetId)) {
			lockedTargetId = -1;
			SpeakText(_("No items found."), true);
			return;
		}
		lockedTargetId = *targetId;
		targetName = Items[*targetId].getName();
		break;
	}
	case TrackerTargetCategory::Chests:
		targetId = ResolveObjectTrackerTarget(lockedTargetId, playerPosition, IsTrackedChestObject, FindNearestUnopenedChestObjectId, [](int id) -> StringOrView { return Objects[id].name(); }, N_("No chests found."), targetName);
		if (!targetId)
			return;
		break;
	case TrackerTargetCategory::Doors:
		targetId = ResolveObjectTrackerTarget(lockedTargetId, playerPosition, IsTrackedDoorObject, FindNearestDoorObjectId, [](int id) -> StringOrView { return DoorLabelForSpeech(Objects[id]); }, N_("No doors found."), targetName);
		if (!targetId)
			return;
		break;
	case TrackerTargetCategory::Shrines:
		targetId = ResolveObjectTrackerTarget(lockedTargetId, playerPosition, IsShrineLikeObject, FindNearestShrineObjectId, [](int id) -> StringOrView { return Objects[id].name(); }, N_("No shrines found."), targetName);
		if (!targetId)
			return;
		break;
	case TrackerTargetCategory::Objects:
		targetId = ResolveObjectTrackerTarget(lockedTargetId, playerPosition, IsTrackedMiscInteractableObject, FindNearestMiscInteractableObjectId, [](int id) -> StringOrView { return TrackerObjectLabelForSpeech(Objects[id]); }, N_("No objects found."), targetName);
		if (!targetId)
			return;
		break;
	case TrackerTargetCategory::Breakables:
		targetId = ResolveObjectTrackerTarget(lockedTargetId, playerPosition, IsTrackedBreakableObject, FindNearestBreakableObjectId, [](int id) -> StringOrView { return Objects[id].name(); }, N_("No breakables found."), targetName);
		if (!targetId)
			return;
		break;
	case TrackerTargetCategory::Monsters: {
		if (lockedTargetId >= 0 && lockedTargetId < static_cast<int>(MaxMonsters)) {
			targetId = lockedTargetId;
		} else {
			targetId = FindNearestMonsterId(playerPosition);
		}
		if (!targetId) {
			SpeakText(_("No monsters found."), true);
			return;
		}
		const Monster &monster = Monsters[*targetId];
		if (!IsTrackedMonster(monster)) {
			lockedTargetId = -1;
			targetId = FindNearestMonsterId(playerPosition);
			if (!targetId) {
				SpeakText(_("No monsters found."), true);
				return;
			}
		}
		lockedTargetId = *targetId;
		targetName = Monsters[*targetId].name();
		break;
	}
	case TrackerTargetCategory::DeadBodies: {
		if (IsCorpsePresent(lockedTargetId)) {
			targetId = lockedTargetId;
		} else {
			targetId = FindNearestCorpseId(playerPosition);
		}
		if (!targetId) {
			SpeakText(_("No dead bodies found."), true);
			return;
		}
		if (!IsCorpsePresent(*targetId)) {
			lockedTargetId = -1;
			SpeakText(_("No dead bodies found."), true);
			return;
		}
		lockedTargetId = *targetId;
		targetName = _("Dead body");
		break;
	}
	case TrackerTargetCategory::Npcs: {
		const std::vector<TrackerCandidate> candidates = CollectNpcTrackerCandidates(playerPosition);
		if (candidates.empty()) {
			SpeakText(_("No NPCs found."), true);
			return;
		}

		if (lockedTargetId >= 0 && lockedTargetId < static_cast<int>(GetNumTowners())) {
			const auto it = std::find_if(candidates.begin(), candidates.end(), [id = lockedTargetId](const TrackerCandidate &c) { return c.id == id; });
			if (it != candidates.end())
				targetId = lockedTargetId;
		}
		if (!targetId)
			targetId = candidates.front().id;

		lockedTargetId = *targetId;
		targetName = Towners[*targetId].name;
		break;
	}
	case TrackerTargetCategory::Players: {
		const std::vector<TrackerCandidate> candidates = CollectPlayerTrackerCandidates(playerPosition);
		if (candidates.empty()) {
			SpeakText(_("No players found."), true);
			return;
		}

		if (lockedTargetId >= 0 && lockedTargetId < MAX_PLRS) {
			const auto it = std::find_if(candidates.begin(), candidates.end(), [id = lockedTargetId](const TrackerCandidate &c) { return c.id == id; });
			if (it != candidates.end())
				targetId = lockedTargetId;
		}
		if (!targetId)
			targetId = candidates.front().id;

		lockedTargetId = *targetId;
		targetName = Players[*targetId].name();
		break;
	}
	case TrackerTargetCategory::DungeonEntrances: {
		const std::vector<TrackerCandidate> candidates = CollectDungeonEntranceTrackerCandidates(playerPosition);
		if (candidates.empty()) {
			SpeakText(_("No dungeon entrances found."), true);
			return;
		}

		// Red portals have negative ids and no trigger, so the name comes from the list.
		const auto locked = std::find_if(candidates.begin(), candidates.end(), [id = lockedTargetId](const TrackerCandidate &c) { return c.id == id; });
		const TrackerCandidate &chosen = locked != candidates.end() ? *locked : candidates.front();
		targetId = chosen.id;
		lockedTargetId = chosen.id;
		targetName = std::string(chosen.name.str());
		break;
	}
	case TrackerTargetCategory::Stairs: {
		const std::vector<TrackerCandidate> candidates = CollectStairsTrackerCandidates(playerPosition);
		if (candidates.empty()) {
			SpeakText(_("No stairs found."), true);
			return;
		}

		if (lockedTargetId >= 0 && lockedTargetId < numtrigs) {
			const auto it = std::find_if(candidates.begin(), candidates.end(), [id = lockedTargetId](const TrackerCandidate &c) { return c.id == id; });
			if (it != candidates.end())
				targetId = lockedTargetId;
		}
		if (!targetId)
			targetId = candidates.front().id;

		lockedTargetId = *targetId;
		targetName = TriggerLabelForSpeech(trigs[*targetId]);
		break;
	}
	case TrackerTargetCategory::QuestLocations: {
		const std::vector<TrackerCandidate> candidates = CollectQuestLocationTrackerCandidates(playerPosition);
		if (candidates.empty()) {
			SpeakText(_("No quest locations found."), true);
			return;
		}

		if (lockedTargetId != -1) {
			const auto it = std::find_if(candidates.begin(), candidates.end(), [id = lockedTargetId](const TrackerCandidate &c) { return c.id == id; });
			if (it != candidates.end())
				targetId = lockedTargetId;
		}
		if (!targetId)
			targetId = candidates.front().id;

		lockedTargetId = *targetId;
		targetName = std::string(candidates.front().name.str());
		if (const auto it = std::find_if(candidates.begin(), candidates.end(), [id = *targetId](const TrackerCandidate &c) { return c.id == id; }); it != candidates.end())
			targetName = std::string(it->name.str());
		break;
	}
	case TrackerTargetCategory::Portals: {
		const std::vector<TrackerCandidate> candidates = CollectPortalTrackerCandidates(playerPosition);
		if (candidates.empty()) {
			SpeakText(_("No portals found."), true);
			return;
		}

		if (lockedTargetId >= 0 && lockedTargetId < MAXPORTAL) {
			const auto it = std::find_if(candidates.begin(), candidates.end(), [id = lockedTargetId](const TrackerCandidate &c) { return c.id == id; });
			if (it != candidates.end())
				targetId = lockedTargetId;
		}
		if (!targetId)
			targetId = candidates.front().id;

		lockedTargetId = *targetId;
		targetName = TownPortalLabelForSpeech(Portals[*targetId]);
		break;
	}
	}

	if (!targetId)
		return;

	std::string msg;
	StrAppend(msg, _("Going to: "), targetName);
	SpeakText(msg, true);

	AutoWalkTrackerTargetId = *targetId;
	AutoWalkTrackerTargetCategory = SelectedTrackerTargetCategory;
	AutoWalkTrackerStarting = true;
	UpdateAutoWalkTracker();
}

/**
 * One step of Shift+Home's walk to a target: where to send the player next, or why the walk ends.
 * `starting` is set for the first step, where a target walked onto that is a step away is in range.
 */
AutoWalkStep PlanAutoWalkStep(const Player &myPlayer, TrackerTargetCategory category, int targetId, bool starting)
{
	const Point playerPosition = myPlayer.position.future;

	std::optional<TrackerGoal> goal;
	std::string inRangeMessage;

	switch (category) {
	case TrackerTargetCategory::Items:
		if (!IsGroundItemPresent(targetId))
			return StopAutoWalk(_("Target item is gone."));
		goal = TileGoal(Items[targetId].position);
		inRangeMessage = _("Item in range.");
		break;
	case TrackerTargetCategory::Chests:
		if (!IsTrackedObjectPresent(targetId, IsTrackedChestObject))
			return StopAutoWalk(_("Target chest is gone."));
		goal = ObjectGoal(Objects[targetId]);
		inRangeMessage = _("Chest in range.");
		break;
	case TrackerTargetCategory::Doors:
		if (!IsTrackedObjectPresent(targetId, IsTrackedDoorObject))
			return StopAutoWalk(_("Target door is gone."));
		goal = ObjectGoal(Objects[targetId]);
		inRangeMessage = _("Door in range.");
		break;
	case TrackerTargetCategory::Shrines:
		if (!IsTrackedObjectPresent(targetId, IsShrineLikeObject))
			return StopAutoWalk(_("Target shrine is gone."));
		goal = ObjectGoal(Objects[targetId]);
		inRangeMessage = _("Shrine in range.");
		break;
	case TrackerTargetCategory::Objects:
		if (!IsTrackedObjectPresent(targetId, IsTrackedMiscInteractableObject))
			return StopAutoWalk(_("Target object is gone."));
		goal = ObjectGoal(Objects[targetId]);
		inRangeMessage = _("Object in range.");
		break;
	case TrackerTargetCategory::Breakables:
		if (!IsTrackedObjectPresent(targetId, IsTrackedBreakableObject))
			return StopAutoWalk(_("Target breakable is gone."));
		goal = ObjectGoal(Objects[targetId]);
		inRangeMessage = _("Breakable in range.");
		break;
	case TrackerTargetCategory::Monsters:
		if (targetId < 0 || targetId >= static_cast<int>(MaxMonsters) || !IsTrackedMonster(Monsters[targetId]))
			return StopAutoWalk(_("Target monster is gone."));
		goal = OccupiedTileGoal(Monsters[targetId].position.tile);
		inRangeMessage = _("Monster in range.");
		break;
	case TrackerTargetCategory::DeadBodies:
		if (!IsCorpsePresent(targetId))
			return StopAutoWalk(_("Target dead body is gone."));
		goal = TileGoal(CorpsePositionForTrackerId(targetId));
		inRangeMessage = _("Dead body in range.");
		break;
	case TrackerTargetCategory::Npcs:
		if (leveltype != DTYPE_TOWN || targetId < 0 || targetId >= static_cast<int>(GetNumTowners()) || !IsTownerPresent(Towners[targetId]._ttype))
			return StopAutoWalk(_("Target NPC is gone."));
		goal = OccupiedTileGoal(Towners[targetId].position);
		inRangeMessage = _("NPC in range.");
		break;
	case TrackerTargetCategory::Players: {
		if (targetId < 0 || targetId >= MAX_PLRS)
			return StopAutoWalk(_("Target player is gone."));
		const Player &player = Players[targetId];
		if (!player.plractive || player._pLvlChanging || player.plrIsOnSetLevel != setlevel || player.plrlevel != myPlayer.plrlevel || !InDungeonBounds(player.position.future))
			return StopAutoWalk(_("Target player is gone."));
		goal = OccupiedTileGoal(player.position.future);
		inRangeMessage = _("Player in range.");
		break;
	}
	case TrackerTargetCategory::DungeonEntrances: {
		if (IsRedPortalTrackerId(targetId)) {
			const Point portalPosition = RedPortalPositionForTrackerId(targetId);
			if (!setlevel || !IsRedPortalAt(portalPosition))
				return StopAutoWalk(_("Target entrance is gone."));
			goal = TileGoal(portalPosition);
		} else {
			if (targetId >= numtrigs)
				return StopAutoWalk(_("Target entrance is gone."));
			const TriggerStruct &trigger = trigs[targetId];
			const bool valid = leveltype == DTYPE_TOWN
			    ? IsAnyOf(trigger._tmsg, WM_DIABNEXTLVL, WM_DIABTOWNWARP)
			    : (setlevel ? trigger._tmsg == WM_DIABRTNLVL : IsAnyOf(trigger._tmsg, WM_DIABPREVLVL, WM_DIABTWARPUP));
			if (!valid)
				return StopAutoWalk(_("Target entrance is gone."));
			goal = TileGoal(trigger.position);
		}
		inRangeMessage = _("Entrance in range.");
		break;
	}
	case TrackerTargetCategory::Stairs: {
		if (leveltype == DTYPE_TOWN || targetId < 0 || targetId >= numtrigs || !IsAnyOf(trigs[targetId]._tmsg, WM_DIABNEXTLVL, WM_DIABPREVLVL, WM_DIABTWARPUP))
			return StopAutoWalk(_("Target stairs are gone."));
		goal = TileGoal(trigs[targetId].position);
		inRangeMessage = _("Stairs in range.");
		break;
	}
	case TrackerTargetCategory::QuestLocations: {
		if (IsQuestSpotTrackerId(targetId)) {
			const std::optional<QuestSpot> spot = FindQuestSpot(QuestSpotIdForTrackerId(targetId));
			if (!spot)
				return StopAutoWalk(_("Target quest location is gone."));
			goal = QuestSpotGoal(*spot);
			inRangeMessage = QuestSpotInRangeMessage(*spot, myPlayer);
			break;
		}
		if (setlevel) {
			if (IsRedPortalTrackerId(targetId)) {
				const Point portalPosition = RedPortalPositionForTrackerId(targetId);
				if (!IsRedPortalAt(portalPosition))
					return StopAutoWalk(_("Target quest location is gone."));
				goal = TileGoal(portalPosition);
			} else {
				if (leveltype == DTYPE_TOWN || targetId >= numtrigs || trigs[targetId]._tmsg != WM_DIABRTNLVL)
					return StopAutoWalk(_("Target quest location is gone."));
				goal = TileGoal(trigs[targetId].position);
			}
			inRangeMessage = _("Quest exit in range.");
			break;
		}

		if (targetId < 0 || targetId >= static_cast<int>(sizeof(Quests) / sizeof(Quests[0])))
			return StopAutoWalk(_("Target quest location is gone."));
		const Quest &quest = Quests[static_cast<size_t>(targetId)];
		if (quest._qslvl == SL_NONE || quest._qactive == QUEST_NOTAVAIL || quest._qlevel != currlevel || !InDungeonBounds(quest.position))
			return StopAutoWalk(_("Target quest location is gone."));
		goal = TileGoal(quest.position);
		inRangeMessage = _("Quest entrance in range.");
		break;
	}
	case TrackerTargetCategory::Portals: {
		std::optional<Point> portalPosition;
		if (leveltype == DTYPE_TOWN) {
			portalPosition = FindTownPortalPositionInTownByPortalIndex(targetId);
		} else if (IsTownPortalOpenOnCurrentLevel(targetId)) {
			portalPosition = Portals[targetId].position;
		}
		if (!portalPosition)
			return StopAutoWalk(_("Target portal is gone."));
		goal = TileGoal(*portalPosition);
		inRangeMessage = _("Portal in range.");
		break;
	}
	}

	if (!goal)
		return StopAutoWalk(_("Can't find a path to the target."));
	if (goal->IsReachedFrom(playerPosition) || (starting && goal->IsOneStepFrom(playerPosition)))
		return StopAutoWalk(inRangeMessage);

	std::optional<TrackerRoute> route = FindTrackerRoute(myPlayer, playerPosition, goal->position, [&goal](Point tile) { return goal->IsReachedFrom(tile); }, /*preferAxisSteps=*/false);
	if (!route)
		return StopAutoWalk(_("Can't find a path to the target."));

	const std::optional<TrackerPathBlockInfo> block = StopRouteAtFirstBlock(*route, playerPosition);
	if (block && (route->path.empty() || playerPosition.WalkingDistance(block->blockPosition) <= TrackerInteractDistanceTiles))
		return StopAutoWalk(TrackerPathBlockMessage(block->type));

	const std::optional<Point> waypoint = ChooseAutoWalkWaypoint(myPlayer, playerPosition, route->path);
	if (!waypoint)
		return StopAutoWalk(_("Can't find a path to the target."));
	return WalkTo(*waypoint);
}

} // namespace

void UpdateAutoWalkTracker()
{
	if (AutoWalkTrackerTargetId < 0)
		return;
	if (IsPlayerInStore() || ChatLogFlag || HelpFlag || InGameMenu()) {
		AutoWalkTrackerTargetId = -1;
		return;
	}
	if (leveltype == DTYPE_TOWN
	    && IsDungeonOnlyTrackerCategory(AutoWalkTrackerTargetCategory)) {
		AutoWalkTrackerTargetId = -1;
		return;
	}
	if (!CanPlayerTakeAction())
		return;

	if (MyPlayer == nullptr) {
		AutoWalkTrackerTargetId = -1;
		SpeakText(_("Cannot walk right now."), true);
		return;
	}
	if (MyPlayer->_pmode != PM_STAND)
		return;
	if (MyPlayer->walkpath[0] != WALK_NONE)
		return;
	if (MyPlayer->destAction != ACTION_NONE)
		return;

	const AutoWalkStep step = PlanAutoWalkStep(*MyPlayer, AutoWalkTrackerTargetCategory, AutoWalkTrackerTargetId, AutoWalkTrackerStarting);
	AutoWalkTrackerStarting = false;
	if (!step.walkTo) {
		AutoWalkTrackerTargetId = -1;
		SpeakText(step.stopMessage, true);
		return;
	}
	NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, *step.walkTo);
}

void TrackerPageUpKeyPressed()
{
	const SDL_Keymod modState = SDL_GetModState();
	const bool cycleCategory = (modState & SDL_KMOD_CTRL) != 0;

	if (cycleCategory) {
		SelectTrackerTargetCategoryRelative(-1);
		if (MyPlayer != nullptr) {
			const Point playerPosition = MyPlayer->position.future;
			if (CollectTrackerCandidatesForSelection(SelectedTrackerTargetCategory, playerPosition).empty()) {
				if (TrackerCategorySelectionIsProximityLimited(SelectedTrackerTargetCategory) && TrackerCategoryHasAnyTargets(SelectedTrackerTargetCategory, playerPosition))
					SpeakText(TrackerCategoryNoNearbyCandidatesFoundMessage(SelectedTrackerTargetCategory), true);
				else
					SpeakText(TrackerCategoryNoCandidatesFoundMessage(SelectedTrackerTargetCategory), true);
			}
		}
		return;
	}

	SelectTrackerTargetRelative(-1);
}

void TrackerPageDownKeyPressed()
{
	const SDL_Keymod modState = SDL_GetModState();
	const bool cycleCategory = (modState & SDL_KMOD_CTRL) != 0;

	if (cycleCategory) {
		SelectTrackerTargetCategoryRelative(+1);
		if (MyPlayer != nullptr) {
			const Point playerPosition = MyPlayer->position.future;
			if (CollectTrackerCandidatesForSelection(SelectedTrackerTargetCategory, playerPosition).empty()) {
				if (TrackerCategorySelectionIsProximityLimited(SelectedTrackerTargetCategory) && TrackerCategoryHasAnyTargets(SelectedTrackerTargetCategory, playerPosition))
					SpeakText(TrackerCategoryNoNearbyCandidatesFoundMessage(SelectedTrackerTargetCategory), true);
				else
					SpeakText(TrackerCategoryNoCandidatesFoundMessage(SelectedTrackerTargetCategory), true);
			}
		}
		return;
	}

	SelectTrackerTargetRelative(+1);
}

void TrackerHomeKeyPressed()
{
	const SDL_Keymod modState = SDL_GetModState();
	const bool autoWalk = (modState & SDL_KMOD_SHIFT) != 0;

	if (autoWalk)
		AutoWalkToTrackerTargetKeyPressed();
	else
		NavigateToTrackerTargetKeyPressed();
}

void ResetAutoWalkTracker()
{
	AutoWalkTrackerTargetId = -1;
}

#ifdef BUILD_TESTING
std::vector<TrackerTargetForTesting> ListTrackerTargetsForTesting()
{
	std::vector<TrackerTargetForTesting> targets;
	const Point playerPosition = MyPlayer->position.future;
	for (const TrackerTargetCategory category : TrackerTargetCategoriesForCurrentLevel()) {
		for (const TrackerCandidate &candidate : CollectTrackerCandidatesForSelection(category, playerPosition))
			targets.push_back({ category, candidate.id, std::string(candidate.name.str()) });
	}
	return targets;
}

std::string DescribeTrackerTargetForTesting(TrackerTargetCategory category, int id)
{
	EnsureTrackerLocksMatchCurrentLevel();
	SelectedTrackerTargetCategory = category;
	LockedTrackerTargetId(category) = id;
	return DescribeSelectedTrackerTarget(/*cycleTarget=*/false);
}

std::optional<Point> PlanAutoWalkStepForTesting(TrackerTargetCategory category, int id, bool starting, std::string &stopMessage)
{
	AutoWalkStep step = PlanAutoWalkStep(*MyPlayer, category, id, starting);
	stopMessage = std::move(step.stopMessage);
	return step.walkTo;
}
#endif

} // namespace devilution
