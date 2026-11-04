---
name: release-notes
description: Draft Xelee's release notes and hand the user a phone-friendly editor for them on the tailnet, then turn the edited draft into .github/QUEST_RELEASE_NOTES.md for the release. Use when the user asks to draft, write or edit release notes or a changelog, or to prepare, cut, tag or publish a release.
---

# Release notes

The user reviews release notes on their phone. Write a draft, serve it in a
small editor on the tailnet, let them edit and save, then build the notes
file from what they saved. A `v*` tag publishes to every player through the
updater, so **never tag before the user approves the notes**.

## 1. Gather what shipped

- The previous release and its notes:
  - `gh release list -R rhythmerc/xelee -L 3`
  - `gh release view <tag> -R rhythmerc/xelee --json body -q .body`
- **The published body is the source of truth.** The user edits notes on GitHub after publishing (they simplified the install steps there), so it can differ from the repo's `.github/QUEST_RELEASE_NOTES.md`. Match its tone and structure.
- What's new: `git log --no-merges --format='%h %s' <prev-tag>..<branch>` for every branch the release will ship. Check `git log fork/main ^<feature-branch>` too, in case main has commits of its own.
- Read commit bodies when a subject doesn't say what a player sees.

## 2. Write the draft

Use a folder that outlives the session: `~/.local/share/xelee-release-notes/<version>/`.

- **`notes.md`** holds the `## Changelog` section only.
  - `###` headings per area (a new mode, XeleeNet, Netplay, Headset, Platforms...).
  - Short bullets, written for players.
  - Leave out internal work: test tools, debug switches, CI, log-only stats, service flags.
- **`claude-notes.md`** is shown above the editor. Put there:
  - the suggested version and why;
  - which branches must merge first;
  - anything untested on real hardware (headset, phones);
  - what was left out as internal;
  - any question the user should answer.

## 3. Serve the editor

```sh
D=~/.local/share/xelee-release-notes/<version>
systemd-run --user --unit xelee-notes --collect \
  python3 -I <this skill>/scripts/server.py --dir "$D" --port 8793
```

**The server:**
- binds the Tailscale IPv4, so the address is `http://bazzite.tail557f1a.ts.net:8793/`;
- keeps every earlier text in `$D/history/`;
- if the unit already exists, `systemctl --user stop xelee-notes` first.

**Put a link on the share for the phone:** write a small HTML page whose `<meta http-equiv="refresh">` and link point at the editor. Then share it:

`python3 -I ~/.claude/skills/tailnet-share/scripts/share.py add PAGE --as release-notes-editor.html`

**Tell the user:**
- the link;
- what the draft covers;
- the questions from `claude-notes.md`.

On a phone, Edit and Preview are tabs. Save writes `notes.md`. Unsaved typing is kept in the browser.

## 4. Build the notes file

When the user says they're done, **read `$D/notes.md` back**; it is now theirs. Then assemble `.github/QUEST_RELEASE_NOTES.md` from:
1. The intro line, which names every platform the release ships.
2. `notes.md` as saved.
3. `## Installation` and `## Updating`, copied from the previous **published** body.
   - Add a section only for a new platform or a changed install step, in the same short style.
   - Drop the `**Full Changelog**` line: the workflow's `--generate-notes` adds its own.

## 5. Release

- **Approval:** show the final notes, or a diff against what they saved, and get the user's OK.
- **Merge:** merge into `main`. Never merge upstream master.
- **Push:** `git push fork main`.
- **Wait** until the Build workflow on main passes every job: Quest, Android, Windows, Linux, and both Macs.
  - Pushing main again cancels the run in progress (concurrency), so batch the commits.
  - A Mac job cancelled with "not acquired… capacity" is a GitHub runner shortage, not a build failure: re-run it.
- **Tag:** `git tag <version> && git push fork <version>`.
- **Watch the tag's run.** Its Publish job creates the release.
- **Check** the release has all its assets (Quest, Android, Windows, AppImage, tarball, macOS arm64, macOS x86_64).
- **Lobby service:** if the netplay protocol or the XeleeNet service changed, xn.xelee.net needs `tools/xeleenet/deploy/provision.sh root@178.128.154.183 xn.xelee.net` before or with the release. Ask the user first, unless they've already authorized it.
- **Afterwards:** `systemctl --user stop xelee-notes`.
