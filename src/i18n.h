#pragma once

// Menu languages. UI text comes from lang/<code>.tsv (tools/gen_lang.py -> generated/ui_text.cpp);
// buff names come from the game's own item / spell names in that language
// (tools/gen_buff_text.py -> generated/buff_text.cpp).
namespace zz::i18n {

enum Lang {
    kEnglish, kGerman, kFrench, kSpanish, kPortuguese, kRussian,
    kJapanese, kKorean, kChinese, kTurkish, kLangCount
};

struct LangInfo {
    const char* code;  // ini value and lang/<code>.tsv
    const char* name;  // shown in the language list, in that language
};

const LangInfo& Info(int lang);
int Language();
void SetLanguage(int lang);
int FromCode(const char* code);  // -1 if unknown
// The Windows display language if it is one of ours, else English.
int SystemLanguage();

// English text -> current language (the English text itself if there is no translation).
// Format strings keep their %d / %s in the same order. Works for any string, not only literals.
const char* T(const char* english);

// Buff name in the current language (`english` if the game has none).
const char* BuffName(int index, const char* english);

// Generated tables -------------------------------------------------------------------------

extern const char* const kUiText[][kLangCount];  // [row][lang], [row][0] = English
extern const int kUiTextCount;
// [buff index in kBuffNames][lang], nullptr = use the English name.
extern const char* const kBuffText[][kLangCount];
extern const int kBuffTextCount;

}  // namespace zz::i18n
