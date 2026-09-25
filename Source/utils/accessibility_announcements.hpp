/**
 * @file utils/accessibility_announcements.hpp
 *
 * Periodic accessibility announcements (low HP warning, durability, boss health,
 * attackable monsters, interactable doors).
 */
#pragma once

#include "utils/string_or_view.hpp"

namespace devilution {

struct Monster;
struct Object;

void UpdatePlayerLowHpWarningSound();
void UpdateLowDurabilityWarnings();
void UpdateBossHealthAnnouncements();
void UpdateAttackableMonsterAnnouncements();
// Names a monster the attack or spell key has just chosen, by a press or while held, unless it was
// already named as the monster next to the player.
void SpeakTargetMonster(int monsterId);
StringOrView MonsterLabelForSpeech(const Monster &monster);
StringOrView DoorLabelForSpeech(const Object &door);
void UpdateInteractableDoorAnnouncements();

} // namespace devilution
