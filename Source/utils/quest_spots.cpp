/**
 * @file utils/quest_spots.cpp
 *
 * Quest spots: places a quest needs the player to reach that are only map tiles (not objects,
 * items, monsters or triggers), so no other tracker category can see them.
 */
#include "utils/quest_spots.hpp"

#include <algorithm>
#include <cstddef>

#include "engine/path.h"
#include "game_mode.hpp"
#include "inv.h"
#include "items.h"
#include "levels/crypt.h"
#include "levels/gendung.h"
#include "levels/town.h"
#include "levels/trigs.h"
#include "player.h"
#include "quests.h"
#include "utils/language.h"
#include "utils/str_cat.hpp"
#include "utils/walk_path_speech.hpp"

namespace devilution {

namespace {

// These two areas mirror OpensHive() and OpensGrave() in levels/town.cpp. Range checks go through
// those functions, so a change there cannot desync what the tracker calls "in range".
constexpr Rectangle HiveGrowthArea { { 79, 61 }, { 4, 4 } };
constexpr Rectangle CryptGraveArea { { 35, 20 }, { 4, 5 } };

/// Where the town warps appear once these spots are opened (InitTownTriggers).
constexpr Point HiveEntrance { 80, 62 };
constexpr Point CryptEntrance { 36, 24 };

[[nodiscard]] int DistanceToArea(Point tile, const Rectangle &area)
{
	const int dx = std::max({ area.position.x - tile.x, 0, tile.x - (area.position.x + area.size.width - 1) });
	const int dy = std::max({ area.position.y - tile.y, 0, tile.y - (area.position.y + area.size.height - 1) });
	return std::max(dx, dy);
}

/// Matches UseItemOpensHive()/UseItemOpensGrave(): one of the 8 neighbouring tiles must be on the spot.
template <typename Predicate>
[[nodiscard]] bool AnyNeighbourMatches(Point tile, Predicate predicate)
{
	for (const Displacement &dir : PathDirs) {
		if (predicate(tile + dir))
			return true;
	}
	return false;
}

[[nodiscard]] bool IsCarrying(const Player &player, _item_indexes id)
{
	return HasInventoryOrBeltItemWithId(player, id) || (!player.HoldItem.isEmpty() && player.HoldItem.IDidx == id);
}

} // namespace

std::vector<QuestSpot> CollectQuestSpots()
{
	std::vector<QuestSpot> spots;
	if (MyPlayer == nullptr || !gbIsHellfire || setlevel)
		return spots;

	// Each spot is listed only while the player is on its quest, not merely while it exists.
	if (leveltype == DTYPE_TOWN) {
		const Player &player = *MyPlayer;
		// The farmer (or Complete Nut, in the cow quest) sets his quest active when he hands over the bomb.
		if (!IsWarpOpen(DTYPE_NEST)
		    && (IsCarrying(player, IDI_RUNEBOMB) || Quests[Q_FARMER]._qactive == QUEST_ACTIVE || Quests[Q_JERSEY]._qactive == QUEST_ACTIVE))
			spots.push_back({ QuestSpotId::HiveGrowth, HiveGrowthArea, HiveEntrance });
		// Grave Matters turns active by itself on Hell 4, long before the map drops from the Defiler, so
		// it counts from picking the map up (_qvar1) or Gillian's hint about it (_qlog).
		if (!IsWarpOpen(DTYPE_CRYPT)
		    && (IsCarrying(player, IDI_MAPOFDOOM) || Quests[Q_GRAVE]._qvar1 == 1 || Quests[Q_GRAVE]._qlog))
			spots.push_back({ QuestSpotId::CryptGrave, CryptGraveArea, CryptEntrance });
		return spots;
	}

	if (leveltype != DTYPE_CRYPT)
		return spots;

	// SyncNakrulRoom() replaces these four tiles when the chamber opens.
	// The game starts the Na-Krul quest on the way down (Hive 4), so this shows whenever he is still sealed in.
	if (currlevel == 24 && UberRow > 0 && UberCol > 2 && !IsUberRoomOpened && !IsUberLeverActivated
	    && Quests[Q_NAKRUL]._qactive == QUEST_ACTIVE) {
		spots.push_back({ QuestSpotId::NaKrulDoor, Rectangle { { UberRow, UberCol - 2 }, { 1, 4 } }, Point { UberRow, UberCol } });
	}

	// The Cornerstone quest has no start; it counts once the player has seen the stone.
	if (CornerStone.isAvailable() && InDungeonBounds(CornerStone.position) && CornerStone.position != Point { 0, 0 }
	    && HasAnyOf(dFlags[CornerStone.position.x][CornerStone.position.y], DungeonFlag::Explored))
		spots.push_back({ QuestSpotId::Cornerstone, Rectangle { CornerStone.position, { 1, 1 } }, CornerStone.position });

	return spots;
}

std::optional<QuestSpot> FindQuestSpot(QuestSpotId id)
{
	for (const QuestSpot &spot : CollectQuestSpots()) {
		if (spot.id == id)
			return spot;
	}
	return std::nullopt;
}

std::string_view QuestSpotLabel(QuestSpotId id)
{
	switch (id) {
	case QuestSpotId::HiveGrowth:
		return _("Orchard growth");
	case QuestSpotId::CryptGrave:
		return _("Marked grave");
	case QuestSpotId::NaKrulDoor:
		return _("Na-Krul's sealed door");
	case QuestSpotId::Cornerstone:
		return _("Cornerstone of the World");
	}
	return {};
}

bool IsQuestSpotInRange(const QuestSpot &spot, Point tile)
{
	switch (spot.id) {
	case QuestSpotId::HiveGrowth:
		return AnyNeighbourMatches(tile, OpensHive);
	case QuestSpotId::CryptGrave:
		return AnyNeighbourMatches(tile, OpensGrave);
	case QuestSpotId::NaKrulDoor:
	case QuestSpotId::Cornerstone:
		return DistanceToArea(tile, spot.area) <= 1;
	}
	return false;
}

std::string QuestSpotInRangeHint(const QuestSpot &spot, const Player &player)
{
	switch (spot.id) {
	case QuestSpotId::HiveGrowth:
		if (IsCarrying(player, IDI_RUNEBOMB))
			return std::string(_("Use the rune bomb from your inventory here."));
		return std::string(_("It takes the rune bomb from the farmer to destroy it."));
	case QuestSpotId::CryptGrave:
		if (IsCarrying(player, IDI_MAPOFDOOM))
			return std::string(_("Use the Cathedral Map from your inventory here."));
		return std::string(_("It takes the Cathedral Map to open it."));
	case QuestSpotId::NaKrulDoor:
		return std::string(_("Reading the three spellbooks nearby in the right order opens it and weakens Na-Krul. The lever beside it opens it with Na-Krul at full strength."));
	case QuestSpotId::Cornerstone:
		if (!CornerStone.item.isEmpty())
			return std::string(_("An item rests on the stone."));
		return std::string(_("Hold an item and press the secondary action to place it on the stone."));
	}
	return {};
}

std::string QuestSpotInRangeMessage(const QuestSpot &spot, const Player &player)
{
	return StrCat(QuestSpotLabel(spot.id), ", ", _("in range."), " ", QuestSpotInRangeHint(spot, player));
}

std::optional<Point> FindQuestSpotApproachTile(const Player &player, Point from, const QuestSpot &spot)
{
	if (IsQuestSpotInRange(spot, from))
		return from;

	std::optional<Point> best;
	size_t bestPathLength = 0;
	std::optional<Point> fallback;
	int fallbackDistance = 0;

	const Rectangle &area = spot.area;
	for (int y = area.position.y - 1; y <= area.position.y + area.size.height; ++y) {
		for (int x = area.position.x - 1; x <= area.position.x + area.size.width; ++x) {
			const Point tile { x, y };
			if (!InDungeonBounds(tile) || !IsQuestSpotInRange(spot, tile) || !PosOkPlayer(player, tile))
				continue;

			const int distance = from.WalkingDistance(tile);
			if (!fallback || distance < fallbackDistance) {
				fallback = tile;
				fallbackDistance = distance;
			}

			const std::optional<std::vector<int8_t>> path = FindKeyboardWalkPathForSpeech(player, from, tile);
			if (path && (!best || path->size() < bestPathLength)) {
				best = tile;
				bestPathLength = path->size();
			}
		}
	}

	return best ? best : fallback;
}

} // namespace devilution
