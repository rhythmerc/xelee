// One lobby. It knows who is in it and how to reach them, and it is the only
// place a join is decided. Only the host polls it (the heartbeat); everyone
// else hears about joins and leaves from the host over the players' own
// links (docs/xeleenet-plan.md §3.1).
//
// The record is written to storage when it changes, never on a heartbeat. The
// heartbeat time and pending "Ask before joining" requests live in memory: a
// lobby whose host keeps polling stays loaded.

import { DurableObject } from "cloudflare:workers";
import { refuse, sameCompat, secs } from "./common.js";

const ok = (body = { ok: true }) => ({ status: 200, body });
// How long a decided "Ask before joining" request keeps its answer, for a
// joiner whose wait timed out just before the host answered.
const DECIDED_KEEP_MS = 60_000;

export class Lobby extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.s = null;
    this.beat = Date.now();
    this.pending = new Map(); // joiner key -> { joiner, since, decided, wake[] }
    ctx.blockConcurrencyWhile(async () => {
      this.s = (await ctx.storage.get("lobby")) || null;
    });
  }

  // --- host -----------------------------------------------------------------

  async create(init) {
    if (this.s) return refuse(409, "exists", "Lobby already exists");
    this.s = { ...init, members: [], banned: [], state: "open", score: null };
    this.beat = Date.now();
    await this.save();
    await this.ctx.storage.setAlarm(Date.now() + this.expireMs());
    return ok({ id: this.s.id, code: this.s.code });
  }

  // The host is alive. `roster` lists the guests its game is linked to;
  // anyone else who joined a while ago has gone. Answers with every member's
  // addresses (a new one means "start reaching them") and the pending joins.
  async heartbeat(key, { state, roster, score, xr }) {
    const host = this.hostOnly(key);
    if (host) return host;
    this.beat = Date.now();
    let changed = false;
    if (state !== this.s.state) {
      this.s.state = state;
      changed = true;
    }
    if (xr !== this.s.host.xr) {
      this.s.host.xr = xr;
      changed = true;
    }
    if (score !== undefined && JSON.stringify(score) !== JSON.stringify(this.s.score)) {
      this.s.score = score;
      changed = true;
    }
    const grace = Date.now() - secs(this.env, "ROSTER_GRACE_SECS", 20) * 1000;
    const kept = this.s.members.filter((m) => roster.includes(m.key) || m.joined > grace);
    if (kept.length !== this.s.members.length) {
      this.s.members = kept;
      changed = true;
    }
    if (changed) await this.save();
    return ok({
      state: this.s.state,
      members: this.s.members.map(publicMember),
      pending: [...this.pending.values()].filter((p) => !p.decided).map((p) => ({
        key: p.joiner.key, name: p.joiner.name, tag: p.joiner.tag, xr: p.joiner.xr,
        country: p.joiner.geo.country, since: p.since,
      })),
    });
  }

  async settings(key, { name, visibility, ask, settings }) {
    const host = this.hostOnly(key);
    if (host) return host;
    if (settings.seats < this.taken()) {
      return refuse(409, "seats", `${this.taken()} players are in the lobby; remove someone first`);
    }
    Object.assign(this.s, { name, public: visibility === "public", ask, settings });
    await this.save();
    return ok();
  }

  async admit(key, target, accept) {
    const host = this.hostOnly(key);
    if (host) return host;
    const p = this.pending.get(target);
    if (!p || p.decided) return refuse(404, "not_pending", "Nobody is waiting to join with that key");
    this.decide(p, accept ? await this.seat(p.joiner) : refuse(403, "declined", "The host declined"));
    return ok({ result: p.decided.body.status || p.decided.body.reason });
  }

  // Removes a player and keeps them out for the rest of the lobby's life.
  async kick(key, target) {
    const host = this.hostOnly(key);
    if (host) return host;
    if (target === this.s.host.key) return refuse(400, "bad_request", "The host can't remove themselves");
    if (!this.s.banned.includes(target)) this.s.banned.push(target);
    this.s.members = this.s.members.filter((m) => m.key !== target);
    const p = this.pending.get(target);
    if (p && !p.decided) this.decide(p, refuse(403, "banned", "The host removed you from this lobby"));
    await this.save();
    return ok();
  }

  // --- joiners --------------------------------------------------------------

  async join(joiner) {
    if (!this.s) return refuse(404, "not_found", "No lobby with that code");
    if (!sameCompat(this.s.compat, joiner.compat)) {
      return refuse(409, "version", "This lobby is on a different Xelee version; both need the same one");
    }
    if (this.s.banned.includes(joiner.key)) return refuse(403, "banned", "The host removed you from this lobby");
    if (joiner.key === this.s.host.key) return refuse(400, "own_lobby", "That's your own lobby");
    const member = this.s.members.find((m) => m.key === joiner.key);
    if (member) {
      // Joining again (a retry, a lost answer): same seat, fresh addresses.
      member.candidates = joiner.candidates;
      await this.save();
      return ok(this.joined(member));
    }
    if (this.taken() >= this.s.settings.seats) return refuse(409, "full", "The lobby is full");
    if (!this.s.ask) return this.seat(joiner);
    let p = this.pending.get(joiner.key);
    if (!p || p.decided) {
      p = { joiner, since: Date.now(), decided: null, wake: [] };
      this.pending.set(joiner.key, p);
    } else {
      p.joiner = joiner;
    }
    return this.wait(p);
  }

  // A joiner on "Ask before joining" asking again after a "pending".
  async joinStatus(key) {
    if (!this.s) return refuse(404, "closed", "The lobby closed");
    const member = this.s.members.find((m) => m.key === key);
    if (member) return ok(this.joined(member));
    const p = this.pending.get(key);
    if (!p) return refuse(404, "not_pending", "Not waiting to join this lobby");
    return this.wait(p);
  }

  async leave(key) {
    if (!this.s) return refuse(404, "closed", "The lobby closed");
    if (key === this.s.host.key) return this.close("host_left");
    const before = this.s.members.length;
    this.s.members = this.s.members.filter((m) => m.key !== key);
    const p = this.pending.get(key);
    if (p && !p.decided) this.pending.delete(key);
    if (this.s.members.length !== before) await this.save();
    return ok();
  }

  // --- closing --------------------------------------------------------------

  async close(reason, key = null) {
    if (!this.s) return ok();
    if (key !== null && key !== this.s.host.key) return refuse(403, "not_host", "Only the host can close the lobby");
    const id = this.s.id;
    for (const p of this.pending.values()) {
      if (!p.decided) this.decide(p, refuse(404, "closed", "The lobby closed"));
    }
    this.s = null;
    await this.ctx.storage.deleteAll();
    await this.directory().remove(id);
    console.log(`lobby ${id} closed: ${reason}`);
    return ok();
  }

  async alarm() {
    if (!this.s) return;
    const quiet = Date.now() - this.beat;
    if (quiet >= this.expireMs()) {
      await this.close("expired");
    } else {
      await this.ctx.storage.setAlarm(this.beat + this.expireMs() + 1000);
    }
  }

  // --- inside ---------------------------------------------------------------

  hostOnly(key) {
    if (!this.s) return refuse(404, "closed", "The lobby closed");
    if (key !== this.s.host.key) return refuse(403, "not_host", "Only the host can do that");
    return null;
  }

  taken() {
    return 1 + this.s.members.length;
  }

  // Gives a joiner the lowest free port (the host is port 0) and answers
  // with everyone they need to reach.
  async seat(joiner) {
    if (!this.s) return refuse(404, "closed", "The lobby closed");
    if (this.taken() >= this.s.settings.seats) return refuse(409, "full", "The lobby is full");
    const used = new Set(this.s.members.map((m) => m.port));
    let port = 1;
    while (used.has(port)) port++;
    const member = {
      key: joiner.key, name: joiner.name, tag: joiner.tag, xr: joiner.xr,
      candidates: joiner.candidates, country: joiner.geo.country, port, joined: Date.now(),
    };
    this.s.members.push(member);
    this.s.members.sort((a, b) => a.port - b.port);
    await this.save();
    return ok(this.joined(member));
  }

  joined(member) {
    const others = [{ ...publicMember(this.s.host), port: 0 }]
      .concat(this.s.members.filter((m) => m.key !== member.key).map(publicMember));
    return {
      status: "joined", id: this.s.id, code: this.s.code, name: this.s.name, port: member.port,
      state: this.s.state, members: others,
    };
  }

  decide(p, result) {
    p.decided = result;
    for (const wake of p.wake.splice(0)) wake();
    setTimeout(() => {
      if (this.pending.get(p.joiner.key) === p) this.pending.delete(p.joiner.key);
    }, DECIDED_KEEP_MS);
  }

  // Waits for the host's answer, up to JOIN_WAIT_SECS; then "pending", and
  // the joiner asks again with joinStatus.
  async wait(p) {
    if (!p.decided) {
      await new Promise((resolve) => {
        const timer = setTimeout(resolve, secs(this.env, "JOIN_WAIT_SECS", 20) * 1000);
        p.wake.push(() => {
          clearTimeout(timer);
          resolve();
        });
      });
    }
    return p.decided || ok({ status: "pending", id: this.s ? this.s.id : null });
  }

  expireMs() {
    return secs(this.env, "EXPIRE_SECS", 30) * 1000;
  }

  directory() {
    return this.env.DIRECTORY.get(this.env.DIRECTORY.idFromName("directory"));
  }

  // Stores the record and refreshes the browser's row.
  async save() {
    await this.ctx.storage.put("lobby", this.s);
    const s = this.s;
    await this.directory().upsert({
      id: s.id, name: s.name, public: s.public, compat: s.compat,
      host: { name: s.host.name, tag: s.host.tag }, geo: s.host.geo,
      taken: this.taken(), settings: s.settings, state: s.state,
      xr: s.host.xr || s.members.some((m) => m.xr), score: s.score,
    });
  }
}

function publicMember(m) {
  return { key: m.key, name: m.name, tag: m.tag, xr: m.xr, port: m.port, candidates: m.candidates };
}
