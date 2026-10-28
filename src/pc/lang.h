/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The game's own text, by key, from resources/lang/<language>.yaml: nested
 * maps whose dotted path is the key ("xeleenet.host.title"). The language is
 * MELEE_LANG ("en" by default); a key it lacks comes from en.yaml, and a key
 * en.yaml lacks is logged once and shown as the key itself.
 *
 * Text may hold named placeholders, "{name}", filled by the caller. A plural
 * is a map of CLDR categories (one:, other:) instead of a string, picked by
 * the count passed with it (English: one for 1, other otherwise).
 *
 * Read on first use; pc_lang_reload() reads the files again (F5, with the
 * stylesheets). Only XeleeNet's text lives there so far. */
#ifndef PC_LANG_H
#define PC_LANG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The text at `key`; a plural's "other". It stays valid until the process
 * ends (a reload keeps the old text). */
const char* pc_tr(const char* key);
/* The text at `key` with its placeholders filled into `out`, which it
 * returns: the arguments after `key` are name, value pairs of strings, ending
 * with NULL. */
const char* pc_trf(char* out, size_t size, const char* key, ...);
void pc_lang_reload(void);

#ifdef __cplusplus
}

#include <initializer_list>
#include <string>
#include <type_traits>

/* A placeholder's value; a number also counts for a plural. */
struct TrArg {
    const char* name;
    std::string value;
    bool counted = false;
    long long count = 0;

    TrArg(const char* n, const std::string& v) : name(n), value(v) {}
    TrArg(const char* n, const char* v) : name(n), value(v) {}
    template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    TrArg(const char* n, T v) : name(n), value(std::to_string(v)), counted(true), count(static_cast<long long>(v)) {}
};

/* The text at `key`, filled; a plural is picked by the first number given. */
std::string tr(const char* key, std::initializer_list<TrArg> args = {});
#endif

#endif
