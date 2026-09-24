/**
 * @file utils/quest_spots.hpp
 *
 * Quest spots: places a quest needs the player to reach that are only map tiles (not objects,
 * items, monsters or triggers), so no other tracker category can see them.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/point.hpp"
#include "engine/rectangle.hpp"

namespace devilution {

struct Player;

enum class QuestSpotId : uint8_t {
	/** Farmer's Orchard: the growth over the Hive entrance in town, destroyed with the rune bomb. */
	HiveGrowth,
	/** Grave Matters: the grave over the Crypt entrance in town, opened with the Cathedral Map. */
	CryptGrave,
	/** Na-Krul: the sealed door of his chamber on Crypt level 4. */
	NaKrulDoor,
	/** Cornerstone of the World: the stone on Crypt level 1 that keeps an item placed on it. */
	Cornerstone,
};

struct QuestSpot {
	QuestSpotId id;
	/** Tiles the spot covers. The player acts on it from any tile in or next to this area. */
	Rectangle area;
	/** Tile used for distances and as the fallback walking target. */
	Point position;
};

/** @brief The spots on the current level whose quest step is still open. */
[[nodiscard]] std::vector<QuestSpot> CollectQuestSpots();

[[nodiscard]] std::optional<QuestSpot> FindQuestSpot(QuestSpotId id);

[[nodiscard]] std::string_view QuestSpotLabel(QuestSpotId id);

/** @brief True when a player standing on @p tile can act on the spot (use the item, drop on it). */
[[nodiscard]] bool IsQuestSpotInRange(const QuestSpot &spot, Point tile);

/** @brief What to do once in range, phrased for the player's current inventory. */
[[nodiscard]] std::string QuestSpotInRangeHint(const QuestSpot &spot, const Player &player);

/** @brief "<label>, in range. <hint>" */
[[nodiscard]] std::string QuestSpotInRangeMessage(const QuestSpot &spot, const Player &player);

/** @brief The walkable in-range tile with the shortest walk from @p from. */
[[nodiscard]] std::optional<Point> FindQuestSpotApproachTile(const Player &player, Point from, const QuestSpot &spot);

} // namespace devilution
