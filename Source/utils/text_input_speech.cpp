/**
 * @file utils/text_input_speech.cpp
 *
 * Speech for editing a text field (chat, Lua console, gold amounts): what the caret moved onto and
 * what was deleted.
 */
#include "utils/text_input_speech.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "utils/format_int.hpp"
#include "utils/language.h"
#include "utils/screen_reader.hpp"
#include "utils/sdl_compat.h"
#include "utils/utf8.hpp"

namespace devilution {

namespace {

[[nodiscard]] std::string_view CharacterAt(std::string_view text, size_t position)
{
	if (position >= text.size())
		return {};
	return text.substr(position, Utf8CodePointLen(text.data() + position));
}

[[nodiscard]] bool IsWordCharacter(unsigned char c)
{
	return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

[[nodiscard]] std::string_view WordAt(std::string_view text, size_t position)
{
	size_t end = position;
	while (end < text.size() && IsWordCharacter(static_cast<unsigned char>(text[end])))
		++end;
	if (end == position)
		return CharacterAt(text, position);
	return text.substr(position, end - position);
}

/// Screen readers often stay silent on a lone punctuation mark, which matters when typing Lua.
[[nodiscard]] std::string_view SymbolName(char c)
{
	switch (c) {
	case ' ':
		return _("space");
	case '\n':
		return _("new line");
	case '.':
		return _("dot");
	case ',':
		return _("comma");
	case ':':
		return _("colon");
	case ';':
		return _("semicolon");
	case '(':
		return _("left paren");
	case ')':
		return _("right paren");
	case '[':
		return _("left bracket");
	case ']':
		return _("right bracket");
	case '{':
		return _("left brace");
	case '}':
		return _("right brace");
	case '"':
		return _("quote");
	case '\'':
		return _("apostrophe");
	case '_':
		return _("underscore");
	case '-':
		return _("dash");
	case '=':
		return _("equals");
	case '/':
		return _("slash");
	case '\\':
		return _("backslash");
	case '+':
		return _("plus");
	case '*':
		return _("star");
	case '<':
		return _("less than");
	case '>':
		return _("greater than");
	case '#':
		return _("number sign");
	case '~':
		return _("tilde");
	default:
		return {};
	}
}

void SpeakFragment(std::string_view fragment)
{
	if (fragment.empty()) {
		SpeakText(_("blank"), /*force=*/true);
		return;
	}
	if (fragment.size() == 1) {
		const std::string_view name = SymbolName(fragment[0]);
		if (!name.empty()) {
			SpeakText(name, /*force=*/true);
			return;
		}
	}
	SpeakText(fragment, /*force=*/true);
}

/// Speaks what an editing key did, given the field's text from before the key was handled.
void SpeakEditingKey(const SDL_Event &event, std::string_view before, TextInputState &state)
{
	const auto modState = SDL_GetModState();
	const bool isShift = (modState & SDL_KMOD_SHIFT) != 0;
	const bool isWordJump = (modState & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) != 0;
	const std::string_view after = state.value();
	const size_t cursor = state.cursorPosition();

	switch (SDLC_EventKey(event)) {
	case SDLK_LEFT:
	case SDLK_RIGHT:
		if (isShift)
			SpeakFragment(state.selectedText());
		else
			SpeakFragment(isWordJump ? WordAt(after, cursor) : CharacterAt(after, cursor));
		break;
	case SDLK_HOME:
	case SDLK_END:
		if (isShift)
			SpeakFragment(state.selectedText());
		else
			SpeakFragment(CharacterAt(after, cursor));
		break;
	case SDLK_BACKSPACE:
	case SDLK_DELETE:
		// Both keys leave the caret where the removed text began.
		if (before.size() > after.size())
			SpeakFragment(std::string_view(before).substr(cursor, before.size() - after.size()));
		else
			SpeakFragment({});
		break;
	case SDLK_V:
		if ((modState & SDL_KMOD_CTRL) != 0)
			SpeakTextInputValue(after);
		break;
	default:
		break;
	}
}

} // namespace

bool HandleTextInputEventWithSpeech(const SDL_Event &event, TextInputState &state)
{
	if (event.type != SDL_EVENT_KEY_DOWN)
		return HandleTextInputEvent(event, state);

	const std::string before { state.value() };
	if (!HandleTextInputEvent(event, state))
		return false;

	SpeakEditingKey(event, before, state);
	return true;
}

bool HandleNumberInputEventWithSpeech(const SDL_Event &event, NumberInputState &state)
{
	const std::string before { state.textInput().value() };
	if (!HandleNumberInputEvent(event, state))
		return false;

	if (event.type == SDL_EVENT_KEY_DOWN) {
		SpeakEditingKey(event, before, state.textInput());
	} else if (event.type == SDL_EVENT_TEXT_INPUT && !state.empty() && state.value() == state.max()) {
		SpeakText(fmt::format(fmt::runtime(_("{:s}, the most you can enter")), FormatInteger(state.max())), /*force=*/true);
	}
	return true;
}

void SpeakTextInputValue(std::string_view value)
{
	if (value.empty())
		SpeakText(_("blank"), /*force=*/true);
	else
		SpeakText(value, /*force=*/true);
}

} // namespace devilution
