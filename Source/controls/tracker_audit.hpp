/**
 * @file controls/tracker_audit.hpp
 *
 * Offline check of the tracker's routes (test builds only): builds dungeon levels from seeds and
 * tries to reach every target the tracker lists, with Home's directions and with Shift+Home's walk.
 */
#pragma once

#ifdef BUILD_TESTING

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace devilution {

/** Loads the game data. False when DIABDAT.MPQ, or hellfire.mpq if asked for, is missing. */
bool InitTrackerAudit(bool hellfire);

/** Starts a new game: every level's seed comes from `gameSeed`, and the quests are set up afresh. */
void StartTrackerAuditGame(uint32_t gameSeed);

/** Builds a level the way entering it for the first time does, the player at the stairs in. */
bool EnterTrackerAuditLevel(int level);

struct TrackerAuditReport {
	int targets = 0;
	/** Targets per outcome, such as "shrines: Home unreachable". */
	std::map<std::string, int> counts;
	/** One line per problem, with enough detail to find it again. */
	std::vector<std::string> problems;
};

/** Tries every target the tracker lists on the current level. Monsters and players are skipped: they move. */
void AuditTrackerRoutesOnCurrentLevel(TrackerAuditReport &report);

} // namespace devilution

#endif
