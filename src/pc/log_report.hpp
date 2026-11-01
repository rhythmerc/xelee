/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <string>

/* The launcher's Send logs row: the last sessions' logs as one text bundle,
 * uploaded to the Xelee log drop (tools/xeleenet/logs.go), which answers with a short
 * code the player posts in Discord or on a GitHub issue. The bundle format is
 * read back by tools/fetch_logs.py. */
namespace pc::log_report {

struct Bundle {
    std::string text;
    int files = 0;
};

/* Gathers the newest logs (and on Android the app's logcat). Reads files and,
 * on Android, runs logcat, so it takes a moment; not for the frame loop. */
Bundle build();

struct Result {
    std::string code;   // set when the upload worked
    std::string saved;  // where the bundle was written when it did not
    std::string error;  // why the upload did not happen
};

/* Uploads the bundle; when that fails, or no log drop is configured, writes
 * it somewhere the player can reach instead. Blocking: run it off the UI
 * thread. */
Result send(const Bundle& bundle);

/* True when a log drop URL is built in or set by MELEE_LOGS_URL. */
bool can_upload();

}  // namespace pc::log_report
