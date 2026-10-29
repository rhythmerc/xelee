/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The game's text by key (lang.h), read from resources/lang/<language>.yaml
 * with rapidyaml (extern/rapidyaml). Used from the main thread only. */
#include "lang.h"

#include "launcher.h"
#include "pc.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#define RYML_SINGLE_HDR_DEFINE_NOW
#include <ryml_all.hpp>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// A key's text: a string, or a plural's forms by category.
struct Entry {
    std::string text;                         // a plural's "other"
    std::map<std::string, std::string> forms; // a plural's: "one", "other"
};
using Table = std::map<std::string, Entry>;

struct Texts {
    std::string code; // MELEE_LANG
    Table en, lang;   // lang empty for English
};

// The text now is the last; the ones before it stay, as pc_tr's pointers
// may point into them.
std::vector<std::unique_ptr<Texts>> s_texts;
std::set<std::string> s_missing, s_fallen; // keys logged once

struct YamlError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// rapidyaml's errors must not return: they leave the parse.
[[noreturn]] void error_basic(ryml::csubstr msg, const ryml::ErrorDataBasic&, void*) {
    throw YamlError(std::string(msg.str, msg.len));
}

[[noreturn]] void error_parse(ryml::csubstr msg, const ryml::ErrorDataParse& at, void*) {
    throw YamlError("line " + std::to_string(at.ymlloc.line) + ": " + std::string(msg.str, msg.len));
}

[[noreturn]] void error_visit(ryml::csubstr msg, const ryml::ErrorDataVisit&, void*) {
    throw YamlError(std::string(msg.str, msg.len));
}

std::string str(ryml::csubstr s) { return s.str != nullptr ? std::string(s.str, s.len) : std::string(); }

bool is_category(const std::string& k) {
    return k == "zero" || k == "one" || k == "two" || k == "few" || k == "many" || k == "other";
}

// A map of plural categories with "other" among them.
bool is_plural(ryml::ConstNodeRef n) {
    bool other = false;
    for (ryml::ConstNodeRef ch : n.children()) {
        const std::string k = str(ch.key());
        if (!is_category(k) || ch.is_map())
            return false;
        other |= k == "other";
    }
    return other;
}

// The tree's strings by their dotted paths.
void flatten(ryml::ConstNodeRef n, const std::string& prefix, Table& out) {
    for (ryml::ConstNodeRef ch : n.children()) {
        const std::string key = prefix.empty() ? str(ch.key()) : prefix + "." + str(ch.key());
        if (ch.is_map() && is_plural(ch)) {
            Entry& e = out[key];
            for (ryml::ConstNodeRef form : ch.children())
                e.forms[str(form.key())] = str(form.val());
            e.text = e.forms["other"];
        } else if (ch.is_map()) {
            flatten(ch, key, out);
        } else if (ch.has_val()) {
            out[key].text = str(ch.val());
        } else {
            pc_log_line("lang: %s is neither text nor a map; skipped", key.c_str());
        }
    }
}

// One language's file; false when it can't be read or isn't YAML.
bool load(const std::string& code, Table& out) {
    static bool callbacks;
    if (!callbacks) {
        callbacks = true;
        ryml::set_callbacks(
            ryml::Callbacks().set_error_basic(error_basic).set_error_parse(error_parse).set_error_visit(error_visit));
    }
    const std::string path = std::string(pc_resources_dir()) + "lang/" + code + ".yaml";
    size_t size = 0;
    char* text = static_cast<char*>(SDL_LoadFile(path.c_str(), &size));
    if (text == nullptr) {
        pc_log_line("lang: no %s", path.c_str());
        return false;
    }
    bool ok = true;
    try {
        const ryml::Tree tree = ryml::parse_in_arena(ryml::to_csubstr(path), ryml::csubstr(text, size));
        if (tree.rootref().is_map())
            flatten(tree.rootref(), "", out);
    } catch (const YamlError& e) {
        pc_log_line("lang: %s: %s", path.c_str(), e.what());
        out.clear();
        ok = false;
    }
    SDL_free(text);
    return ok;
}

// MELEE_LANG, as a file name: letters, digits, '-' and '_'.
std::string language() {
    const char* env = std::getenv("MELEE_LANG");
    std::string code;
    for (const char* p = env != nullptr ? env : ""; *p != '\0'; p++)
        if (std::isalnum(static_cast<unsigned char>(*p)) || *p == '-' || *p == '_')
            code += *p;
    return code.empty() ? std::string("en") : code;
}

void read() {
    auto t = std::make_unique<Texts>();
    t->code = language();
    bool ok = load("en", t->en);
    if (t->code != "en")
        ok = load(t->code, t->lang) && ok;
    if (!ok && !s_texts.empty()) {
        pc_log_line("lang: kept the text as it was");
        return; // a typo while editing doesn't blank the screen
    }
    pc_log_line("lang: %s, %zu keys (en %zu)", t->code.c_str(), t->code == "en" ? t->en.size() : t->lang.size(),
                t->en.size());
    s_texts.push_back(std::move(t));
}

const Texts& texts() {
    if (s_texts.empty())
        read();
    return *s_texts.back();
}

// pc_lang_overlay's: keys under `from` are looked up under `to` first.
std::string s_overlay_from, s_overlay_to;

const Entry* find_exact(const std::string& key) {
    const Texts& t = texts();
    if (auto it = t.lang.find(key); it != t.lang.end())
        return &it->second;
    auto it = t.en.find(key);
    if (it == t.en.end())
        return nullptr;
    if (t.code != "en" && s_fallen.insert(key).second)
        pc_log_line("lang: %s has no %s; English used", t.code.c_str(), key.c_str());
    return &it->second;
}

const Entry* find(const char* key) {
    if (!s_overlay_from.empty() && std::strncmp(key, s_overlay_from.c_str(), s_overlay_from.size()) == 0)
        if (const Entry* e = find_exact(s_overlay_to + (key + s_overlay_from.size())))
            return e;
    return find_exact(key);
}

// A key with no text shows as itself.
const char* missing(const char* key) {
    auto [it, added] = s_missing.insert(key);
    if (added)
        pc_log_line("lang: no text for %s", key);
    return it->c_str();
}

// The plural form for `count`: English's rule, with "other" when a language
// has no such form.
const std::string& form(const Entry& e, long long count) {
    auto it = e.forms.find(count == 1 ? "one" : "other");
    return it != e.forms.end() ? it->second : e.text;
}

std::string fill(const std::string& text, const TrArg* args, size_t n) {
    std::string r;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t open = text.find('{', pos), close = open == std::string::npos ? open : text.find('}', open);
        if (close == std::string::npos) {
            r += text.substr(pos);
            break;
        }
        r += text.substr(pos, open - pos);
        const std::string name = text.substr(open + 1, close - open - 1);
        const TrArg* a = nullptr;
        for (size_t i = 0; i < n && a == nullptr; i++)
            if (name == args[i].name)
                a = &args[i];
        r += a != nullptr ? a->value : text.substr(open, close - open + 1); // unknown: left as written
        pos = close + 1;
    }
    return r;
}

std::string tr_args(const char* key, const TrArg* args, size_t n) {
    const Entry* e = find(key);
    if (e == nullptr)
        return missing(key);
    const std::string* text = &e->text;
    for (size_t i = 0; i < n && !e->forms.empty(); i++)
        if (args[i].counted) {
            text = &form(*e, args[i].count);
            break;
        }
    return fill(*text, args, n);
}

} // namespace

std::string tr(const char* key, std::initializer_list<TrArg> args) { return tr_args(key, args.begin(), args.size()); }

extern "C" const char* pc_tr(const char* key) {
    const Entry* e = find(key);
    return e != nullptr ? e->text.c_str() : missing(key);
}

extern "C" const char* pc_trf(char* out, size_t size, const char* key, ...) {
    std::vector<TrArg> args;
    va_list ap;
    va_start(ap, key);
    while (const char* name = va_arg(ap, const char*)) {
        const char* value = va_arg(ap, const char*);
        args.emplace_back(name, value != nullptr ? value : "");
    }
    va_end(ap);
    std::snprintf(out, size, "%s", tr_args(key, args.data(), args.size()).c_str());
    return out;
}

extern "C" void pc_lang_overlay(const char* from, const char* to) {
    s_overlay_from = from != nullptr && to != nullptr ? from : "";
    s_overlay_to = from != nullptr && to != nullptr ? to : "";
}

extern "C" void pc_lang_reload(void) {
    if (!s_texts.empty())
        read();
}
