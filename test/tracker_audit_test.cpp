/**
 * Builds dungeon levels from seeds and checks every target the tracker lists: Home's directions and
 * Shift+Home's walk must reach each one the game lets a player reach, and must say there is no path
 * to one a quest still walls off.
 *
 * TRACKER_AUDIT_GAMES sets how many games (seeds) to build, 3 by default. TRACKER_AUDIT_SHOW sets
 * how many problems to print, 40 by default.
 */
#include <algorithm>
#include <cstdlib>
#include <iostream>

#include <gtest/gtest.h>

#include "controls/tracker_audit.hpp"

using namespace devilution;

namespace {

int EnvInt(const char *name, int fallback)
{
	const char *value = std::getenv(name);
	return value != nullptr ? std::max(0, std::atoi(value)) : fallback;
}

void RunAudit(bool hellfire, int lastLevel)
{
	if (!InitTrackerAudit(hellfire))
		GTEST_SKIP() << "MPQ assets (spawn.mpq or DIABDAT.MPQ) not found - skipping test";

	TrackerAuditReport report;
	const int games = std::max(1, EnvInt("TRACKER_AUDIT_GAMES", 3));
	for (int game = 1; game <= games; game++) {
		StartTrackerAuditGame(static_cast<uint32_t>(game));
		for (int level = 1; level <= lastLevel; level++) {
			ASSERT_TRUE(EnterTrackerAuditLevel(level)) << "game " << game << " level " << level;
			AuditTrackerRoutesOnCurrentLevel(report);
		}
	}

	std::cout << report.targets << " targets on " << games * lastLevel << " levels\n";
	for (const auto &[outcome, count] : report.counts)
		std::cout << "  " << count << "  " << outcome << "\n";
	const int show = EnvInt("TRACKER_AUDIT_SHOW", 40);
	std::cout << report.problems.size() << " problems\n";
	for (size_t i = 0; i < report.problems.size() && static_cast<int>(i) < show; i++)
		std::cout << report.problems[i] << "\n";
	EXPECT_TRUE(report.problems.empty());
}

TEST(TrackerAudit, Diablo)
{
	RunAudit(/*hellfire=*/false, 16);
}

TEST(TrackerAudit, Hellfire)
{
	RunAudit(/*hellfire=*/true, 24);
}

} // namespace
