#pragma once
// Controller actions implementation

#include <cstddef>
#include <cstdint>

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#else
#include <SDL.h>
#endif

#include "controls/axis_direction.h"
#include "controls/controller.h"
#include "controls/game_controls.h"
#include "player.h"

namespace devilution {

enum class BeltItemType : uint8_t {
	Healing,
	Mana,
};

extern GameActionType ControllerActionHeld;
extern bool StandToggle;
extern bool StandGroundHeld;

// Runs every frame.
// Handles menu movement.
void plrctrls_every_frame();

// Run after every game logic iteration.
// Handles player movement.
void plrctrls_after_game_logic();

// Runs at the end of CheckCursMove()
// Handles item, object, and monster auto-aim.
void plrctrls_after_check_curs_move();

// Moves the map if active, the cursor otherwise.
void HandleRightStickMotion();

// Whether we're in a dialog menu that the game handles natively with keyboard controls.
bool InGameMenu();

void SetPointAndClick(bool value);

bool IsPointAndClick();
bool IsMovementHandlerActive();

void DetectInputMethod(const SDL_Event &event, const ControllerButtonEvent &gamepadEvent);
void ProcessGameAction(const GameAction &action);

void UseBeltItem(BeltItemType type);

// Talk to towners, click on inv items, attack, etc.
void PerformPrimaryAction();

// Open chests, doors, pickup items.
void PerformSecondaryAction();

// Like PerformPrimaryAction but auto-selects a nearby target for keyboard-only play.
// Auto-repeats of the key are ignored while it is held: UpdateActionKeyHolds takes over.
void PerformPrimaryActionAutoTarget();

// Like PerformSecondaryAction but auto-selects a nearby target for keyboard-only play.
// Acts once per press: auto-repeats of the key are ignored.
void PerformSecondaryActionAutoTarget();

// Like PerformSpellAction but auto-selects a nearby target for keyboard-only play.
// Auto-repeats of the key are ignored while it is held: UpdateActionKeyHolds takes over.
void PerformSpellActionAutoTarget();

// The key bound to the matching auto-target action was released.
void ReleasePrimaryActionAutoTarget();
void ReleaseSecondaryActionAutoTarget();
void ReleaseSpellActionAutoTarget();

// Runs once per game tick. While the primary action or spell key is held in a dungeon, keeps
// attacking or casting at the monster the press chose, following it when attacking, and moves on
// to the next nearest, naming it, once that monster dies, leaves sight or cannot be reached. A melee
// attack turns first to any monster already in reach.
void UpdateActionKeyHolds();

// Stops held primary action and spell keys from fighting until they are pressed again.
void StopActionKeyHolds();

// Whether a held primary action or spell key is locked on this monster.
bool IsActionKeyHoldTarget(int monsterId);

void UpdateSpellTarget(SpellID spell);
bool TryDropItem();
void InvalidateInventorySlot();
void FocusOnInventory();
void ToggleStashFocus();
void InventoryMoveFromKeyboard(AxisDirection dir);
void HotSpellMove(AxisDirection dir);
void PerformSpellAction();
void QuickCast(size_t slot);

extern int speedspellcount;

} // namespace devilution
