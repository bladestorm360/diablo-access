/**
 * @file controls/escape_to_stairs.hpp
 *
 * Ctrl+R: drop everything and walk to the nearest way up, keeping away from monsters.
 */
#pragma once

namespace devilution {

// Stops what the player is doing and walks to the nearest way up, steering around monsters.
// Pressing it again cancels.
void EscapeToStairsKeyPressed();

// Runs once per game tick: steers the escape one step at a time.
void UpdateEscapeToStairs();

// Ends the escape without a word, as any other walk or action does.
void StopEscapeToStairs();

} // namespace devilution
