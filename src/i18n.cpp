#include "i18n.h"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace zz::i18n {

namespace {

const LangInfo kLanguages[kLangCount] = {
    {"en", "English"},
    {"de", "Deutsch"},
    {"fr", "Français"},
    {"es", "Español"},
    {"pt", "Português (Brasil)"},
    {"ru", "Русский"},
    {"ja", "日本語"},
    {"ko", "한국어"},
    {"zh", "简体中文"},
    {"tr", "Türkçe"},
};

std::mutex g_mutex;
int g_lang = kEnglish;
std::unordered_map<std::string_view, const char*> g_by_text;

void Rebuild() {
    g_by_text.clear();
    if (g_lang == kEnglish) return;
    for (int r = 0; r < kUiTextCount; ++r)
        if (const char* t = kUiText[r][g_lang]) g_by_text[kUiText[r][0]] = t;
}

}  // namespace

const LangInfo& Info(int lang) { return kLanguages[(lang >= 0 && lang < kLangCount) ? lang : 0]; }

int Language() { return g_lang; }

void SetLanguage(int lang) {
    std::lock_guard lock(g_mutex);
    g_lang = (lang >= 0 && lang < kLangCount) ? lang : kEnglish;
    Rebuild();
}

int FromCode(const char* code) {
    for (int i = 0; i < kLangCount; ++i)
        if (_stricmp(kLanguages[i].code, code) == 0) return i;
    return -1;
}

int SystemLanguage() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_GERMAN: return kGerman;
        case LANG_FRENCH: return kFrench;
        case LANG_SPANISH: return kSpanish;
        case LANG_PORTUGUESE: return kPortuguese;
        case LANG_RUSSIAN: return kRussian;
        case LANG_JAPANESE: return kJapanese;
        case LANG_KOREAN: return kKorean;
        case LANG_CHINESE: return kChinese;
        case LANG_TURKISH: return kTurkish;
        default: return kEnglish;
    }
}

const char* T(const char* english) {
    if (!english || g_lang == kEnglish) return english;
    std::lock_guard lock(g_mutex);
    auto it = g_by_text.find(english);
    return it != g_by_text.end() ? it->second : english;
}

const char* BuffName(int index, const char* english) {
    if (g_lang == kEnglish || index < 0 || index >= kBuffTextCount) return english;
    const char* t = kBuffText[index][g_lang];
    return t ? t : english;
}

}  // namespace zz::i18n
