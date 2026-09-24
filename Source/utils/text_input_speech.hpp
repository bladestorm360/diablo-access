/**
 * @file utils/text_input_speech.hpp
 *
 * Speech for editing a text field (chat, Lua console): what the caret moved onto and what was deleted.
 */
#pragma once

#include <string_view>

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#else
#include <SDL.h>
#endif

#include "DiabloUI/text_input.hpp"

namespace devilution {

/**
 * @brief Same as HandleTextInputEvent, then speaks the effect of an editing key.
 *
 * Left/Right speak the character (Ctrl: the word) the caret lands on, Home/End speak the first
 * character or "blank", Backspace/Delete speak what they removed, and Ctrl+V speaks the field.
 * Typed characters are left to the screen reader's own typing echo.
 */
bool HandleTextInputEventWithSpeech(const SDL_Event &event, TextInputState &state);

/** @brief Speaks the whole field, or "blank" when it is empty. */
void SpeakTextInputValue(std::string_view value);

} // namespace devilution
