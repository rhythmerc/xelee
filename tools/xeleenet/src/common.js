// Shared by the router and the Durable Objects: replies, encodings, the
// session token, and the checks every field from a client goes through.

export function json(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" },
  });
}

// A refusal: `reason` is the stable word the game switches on, `error` the
// line it may show as is.
export function refuse(status, reason, error) {
  return { status, body: { error, reason } };
}

export class HttpError extends Error {
  constructor(status, reason, error) {
    super(error);
    this.status = status;
    this.body = { error, reason };
  }
}

export function b64url(bytes) {
  let s = "";
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export function unb64url(text) {
  if (typeof text !== "string" || !/^[A-Za-z0-9_-]*$/.test(text)) return null;
  const s = atob(text.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((text.length + 3) % 4));
  return Uint8Array.from(s, (c) => c.charCodeAt(0));
}

const enc = new TextEncoder();

export function randomBytes(n) {
  return crypto.getRandomValues(new Uint8Array(n));
}

// The tag after a player's name: the first 40 bits of SHA-1(public key) in
// RFC 4648 base32, exactly as net_identity.c makes a connect code's suffix.
// Names can repeat; the tag is what tells two players apart.
export async function tagOf(pubkey) {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-1", pubkey));
  const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  let bits = 0n;
  for (let i = 0; i < 5; i++) bits = (bits << 8n) | BigInt(digest[i]);
  let tag = "";
  for (let i = 0; i < 8; i++) tag += alphabet[Number((bits >> BigInt(35 - 5 * i)) & 31n)];
  return tag;
}

// Lobby codes: six characters with no 0/O or 1/I, so one read off a stream or
// a headset is typed back right. Shown as K7M-Q2D; accepted in any case, with
// or without the dash.
const CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

export function newCode() {
  let code = "";
  for (const b of randomBytes(6)) code += CODE_ALPHABET[b & 31];
  return code;
}

export function normalizeCode(text) {
  if (typeof text !== "string") return null;
  const code = text.toUpperCase().replace(/[\s-]/g, "");
  return /^[A-HJ-NP-Z2-9]{6}$/.test(code) ? code : null;
}

// --- session ---------------------------------------------------------------
//
// A client proves it holds its key once: POST /v1/session gives it a nonce,
// it signs "xeleenet-session-v1\n<nonce>\n<pubkey>", and /v1/session/verify
// answers with a token good for an hour. Neither the nonce nor the token is
// stored: both carry an HMAC under TOKEN_KEY.

const NONCE_SECS = 120;
export const TOKEN_SECS = 3600;

async function hmac(env, text) {
  if (!env.TOKEN_KEY) throw new HttpError(500, "server", "The service has no TOKEN_KEY");
  const key = await crypto.subtle.importKey(
    "raw", enc.encode(env.TOKEN_KEY), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  return b64url(new Uint8Array(await crypto.subtle.sign("HMAC", key, enc.encode(text))).subarray(0, 16));
}

function sameText(a, b) {
  if (a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}

export function now() {
  return Math.floor(Date.now() / 1000);
}

export async function makeNonce(env, pubkey) {
  const ts = now();
  const salt = b64url(randomBytes(9));
  return `${ts}.${salt}.${await hmac(env, `n|${pubkey}|${ts}|${salt}`)}`;
}

export async function checkNonce(env, pubkey, nonce) {
  const parts = typeof nonce === "string" ? nonce.split(".") : [];
  if (parts.length !== 3) return false;
  const [ts, salt, mac] = parts;
  const age = now() - Number(ts);
  if (!(age >= 0 && age <= NONCE_SECS)) return false;
  return sameText(mac, await hmac(env, `n|${pubkey}|${ts}|${salt}`));
}

export function sessionMessage(nonce, pubkey) {
  return enc.encode(`xeleenet-session-v1\n${nonce}\n${pubkey}`);
}

export async function verifySignature(pubkeyBytes, sigBytes, message) {
  try {
    const key = await crypto.subtle.importKey("raw", pubkeyBytes, { name: "Ed25519" }, false, ["verify"]);
    return await crypto.subtle.verify({ name: "Ed25519" }, key, sigBytes, message);
  } catch {
    return false;
  }
}

export async function makeToken(env, pubkey, expires) {
  return `${pubkey}.${expires}.${await hmac(env, `t|${pubkey}|${expires}`)}`;
}

// The caller's key (base64url) and tag, or an HttpError.
export async function authorize(request, env) {
  const header = request.headers.get("Authorization") || "";
  const token = header.startsWith("Bearer ") ? header.slice(7) : "";
  const parts = token.split(".");
  if (parts.length === 3) {
    const [pubkey, expires, mac] = parts;
    const bytes = unb64url(pubkey);
    if (bytes && bytes.length === 32 && Number(expires) > now() &&
        sameText(mac, await hmac(env, `t|${pubkey}|${expires}`))) {
      return { pubkey, tag: await tagOf(bytes) };
    }
  }
  throw new HttpError(401, "unauthorized", "Sign in to XeleeNet again");
}

// --- field checks ------------------------------------------------------------

// What Name Entry can type: letters, digits, a space and a few symbols.
const NAME_CHARS = /^[A-Za-z0-9 .!?&'#+\-]+$/;

function blocked(text, env) {
  const words = (env.NAME_BLOCKLIST || "").split(",").map((w) => w.trim().toLowerCase()).filter(Boolean);
  if (!words.length) return false;
  const flat = text.toLowerCase().replace(/[^a-z0-9]/g, "");
  return words.some((w) => flat.includes(w.replace(/[^a-z0-9]/g, "")));
}

function checkName(value, max, what, env) {
  const text = typeof value === "string" ? value.trim().replace(/\s+/g, " ") : "";
  if (!text || text.length > max || !NAME_CHARS.test(text)) {
    throw new HttpError(400, "bad_name", `${what} must be 1-${max} letters, digits or simple symbols`);
  }
  if (blocked(text, env)) {
    throw new HttpError(400, "bad_name", `Pick a different ${what.toLowerCase()}`);
  }
  return text;
}

// TODO: make the cap 15; the stage select's draw already fits 15 (xeleenet-sss.rcss).
export const playerName = (v, env) => checkName(v, 12, "Name", env);
export const lobbyName = (v, env) => checkName(v, 20, "Lobby name", env);

// The build a player runs. Lobbies only match the exact same build: the
// native wire version, the app revision and the disc image.
export function compat(value) {
  const c = value || {};
  const ok = Number.isInteger(c.proto) && c.proto > 0 && c.proto < 65536 &&
    typeof c.rev === "string" && /^[A-Za-z0-9._+-]{1,40}$/.test(c.rev) &&
    typeof c.disc === "string" && /^[A-Za-z0-9]{1,32}$/.test(c.disc);
  if (!ok) throw new HttpError(400, "bad_request", "Missing or malformed build info");
  return { proto: c.proto, rev: c.rev, disc: c.disc };
}

export function sameCompat(a, b) {
  return a.proto === b.proto && a.rev === b.rev && a.disc === b.disc;
}

// Where a player can be reached: up to four "ip:port" (IPv6 in brackets),
// their public mapping from STUN and their LAN address.
const ADDR = /^(?:(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})|\[[0-9A-Fa-f:.]{2,45}\]):(\d{1,5})$/;

export function candidates(value) {
  const list = Array.isArray(value) ? value : [];
  if (list.length < 1 || list.length > 4) {
    throw new HttpError(400, "bad_request", "Send 1-4 addresses");
  }
  for (const a of list) {
    const m = typeof a === "string" ? ADDR.exec(a) : null;
    const port = m ? Number(m[5]) : 0;
    const octets = m && m[1] !== undefined ? [m[1], m[2], m[3], m[4]].map(Number) : [];
    if (!m || port < 1 || port > 65535 || octets.some((o) => o > 255)) {
      throw new HttpError(400, "bad_request", `Malformed address ${String(a).slice(0, 60)}`);
    }
  }
  return list.slice();
}

function pick(value, allowed, what) {
  if (!allowed.includes(value)) throw new HttpError(400, "bad_settings", `Bad ${what}`);
  return value;
}

function int(value, lo, hi, what) {
  if (!Number.isInteger(value) || value < lo || value > hi) throw new HttpError(400, "bad_settings", `Bad ${what}`);
  return value;
}

// The settings the browser shows. The full set travels between the players'
// games (the host owns it); the service keeps this summary only.
export function settings(value) {
  const s = value || {};
  const out = {
    preset: pick(s.preset, ["1v1", "doubles", "ffa", "custom"], "preset"),
    seats: int(s.seats, 2, 4, "seat count"),
    mode: pick(s.mode, ["ffa", "teams"], "mode"),
    win: pick(s.win, ["stock", "time"], "win condition"),
    stocks: int(s.stocks, 1, 99, "stock count"),
    time_min: int(s.time_min, 0, 99, "time limit"),
    items: pick(s.items, ["off", "very_low", "low", "medium", "high", "very_high"], "item frequency"),
    stage_list: pick(s.stage_list, ["competitive", "all", "custom"], "stage list"),
    pick: pick(s.pick, ["loser", "everyone", "random", "host"], "stage pick"),
    format: pick(s.format, ["endless", "first_to"], "set format"),
    first_to: int(s.first_to ?? 2, 2, 5, "set length"),
    place_secs: pick(s.place_secs, [10, 15, 20, 30, 45, 60], "placement time"),
  };
  // Loser picks needs a loser: one side in a 1v1, or a team.
  if (out.pick === "loser" && out.mode === "ffa" && out.seats > 2) {
    throw new HttpError(400, "bad_settings", "Loser picks needs two players or Teams");
  }
  return out;
}

// Where a request came from, as far as Cloudflare can tell: the country is
// shown, the coordinates only sort the list and are never sent out.
export function geo(request) {
  const cf = request.cf || {};
  const lat = Number(cf.latitude);
  const lon = Number(cf.longitude);
  return {
    country: typeof cf.country === "string" ? cf.country : "",
    lat: Number.isFinite(lat) ? lat : null,
    lon: Number.isFinite(lon) ? lon : null,
  };
}

export function km(a, b) {
  if (a.lat === null || b.lat === null) return null;
  const r = Math.PI / 180;
  const h = Math.sin((b.lat - a.lat) * r / 2) ** 2 +
    Math.cos(a.lat * r) * Math.cos(b.lat * r) * Math.sin((b.lon - a.lon) * r / 2) ** 2;
  return 12742 * Math.asin(Math.sqrt(h));
}

export function secs(env, name, fallback) {
  const v = Number(env[name]);
  return Number.isFinite(v) && v > 0 ? v : fallback;
}
