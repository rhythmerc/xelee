// The directory: one instance for the whole service. It hands out lobby codes,
// resolves a code to its lobby, remembers each host's open lobby, and keeps a
// summary row per lobby for the browser. Each Lobby pushes its row here when
// something the browser shows changes, and removes it when it closes.
//
// Rows live in memory and are written through to storage, so a list never
// reads storage.

import { DurableObject } from "cloudflare:workers";
import { newCode, km, sameCompat } from "./common.js";

// A code reserved for a lobby that never finished opening is dropped after this.
const STUB_SECS = 60;
const LIST_MAX = 50;
// Lobbies this close together sort as equally near; within a band, newest first.
const BAND_KM = 1000;

export class Directory extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.rows = new Map(); // lobby id -> row
    this.codes = new Map(); // code -> lobby id
    ctx.blockConcurrencyWhile(async () => {
      for (const row of (await ctx.storage.list({ prefix: "row:" })).values()) {
        this.rows.set(row.id, row);
        this.codes.set(row.code, row.id);
      }
    });
  }

  // A code for a new lobby, and the host's previous lobby if one is still
  // open: a host has one lobby at a time, and the caller closes the old one
  // (a game that crashed and hosts again leaves no ghost behind).
  async reserve(id, hostKey) {
    let replaced = null;
    for (const row of this.rows.values()) {
      if (row.host_key === hostKey) replaced = row.id;
    }
    let code;
    do code = newCode(); while (this.codes.has(code));
    const row = { id, code, host_key: hostKey, stub: true, created: Date.now() };
    await this.put(row);
    return { code, replaced };
  }

  async resolve(code) {
    const id = this.codes.get(code);
    const row = id ? this.rows.get(id) : null;
    return row && !this.stale(row) ? id : null;
  }

  async upsert(row) {
    const old = this.rows.get(row.id);
    if (!old) return; // removed meanwhile: the lobby is closing
    await this.put({ ...row, code: old.code, host_key: old.host_key, created: old.created, stub: false });
  }

  async remove(id) {
    const row = this.rows.get(id);
    if (!row) return;
    this.rows.delete(id);
    if (this.codes.get(row.code) === id) this.codes.delete(row.code);
    await this.ctx.storage.delete(`row:${id}`);
  }

  // The public lobbies a player can join: the same build, not full unless
  // asked for, nearest first. Lobbies on other builds are only counted.
  async list({ compat, full, from }) {
    const out = [];
    let otherVersions = 0;
    for (const row of this.rows.values()) {
      if (row.stub || !row.public) continue;
      if (!sameCompat(row.compat, compat)) {
        otherVersions++;
        continue;
      }
      if (!full && row.taken >= row.settings.seats) continue;
      const d = km(from, row.geo);
      out.push({ row, band: d === null ? 0 : Math.floor(d / BAND_KM) });
    }
    out.sort((a, b) => a.band - b.band || b.row.created - a.row.created);
    return {
      lobbies: out.slice(0, LIST_MAX).map(({ row }) => ({
        code: row.code,
        name: row.name,
        host: { name: row.host.name, tag: row.host.tag },
        country: row.geo.country,
        taken: row.taken,
        settings: row.settings,
        state: row.state,
        xr: row.xr,
        created: row.created,
      })),
      other_versions: otherVersions,
    };
  }

  stale(row) {
    return row.stub && Date.now() - row.created > STUB_SECS * 1000;
  }

  async put(row) {
    for (const old of this.rows.values()) {
      if (this.stale(old)) await this.remove(old.id);
    }
    this.rows.set(row.id, row);
    this.codes.set(row.code, row.id);
    await this.ctx.storage.put(`row:${row.id}`, row);
  }
}
