// XeleeNet, the lobby service (README.md; docs/xeleenet-plan.md §3.1). It
// lists public lobbies, turns a join code into a seat, and passes on the
// addresses players need to reach each other directly. It never carries
// match traffic and never decides game state.
//
//   POST   /v1/session                 {pubkey}               -> {nonce}
//   POST   /v1/session/verify          {pubkey, nonce, sig}   -> {token, expires, tag}
//   GET    /v1/lobbies?proto=&rev=&disc=[&full=1]            -> {lobbies, other_versions}
//   POST   /v1/lobbies                 host a lobby           -> {id, code}
//   POST   /v1/lobbies/<id>/heartbeat  host, every few s      -> {state, members, pending}
//   PUT    /v1/lobbies/<id>/settings   host
//   POST   /v1/lobbies/<id>/admit      host {key, accept}     (Ask before joining)
//   POST   /v1/lobbies/<id>/kick       host {key}
//   DELETE /v1/lobbies/<id>            host (or POST /v1/lobbies/<id>/close)
//   POST   /v1/join                    {code, ...}            -> joined | pending | refusal
//   GET    /v1/lobbies/<id>/join       joiner, after "pending" -> joined | pending | refusal
//   POST   /v1/lobbies/<id>/leave      member
//
// Everything but the two session calls carries "Authorization: Bearer <token>".
// A refusal is {error, reason}: `reason` is a stable word for the game,
// `error` a line it can show.

import { Directory } from "./directory.js";
import { Lobby } from "./lobby.js";
import {
  HttpError, TOKEN_SECS, authorize, candidates, checkNonce, compat, geo, json, lobbyName,
  makeNonce, makeToken, normalizeCode, now, playerName, randomBytes, sessionMessage, settings, tagOf,
  unb64url, verifySignature,
} from "./common.js";

export { Directory, Lobby };

const MAX_BODY = 4096;
const LOBBY_PATH = /^\/v1\/lobbies\/([0-9a-f]{32})(?:\/(heartbeat|settings|admit|kick|join|leave|close))?$/;

export default {
  async fetch(request, env) {
    try {
      return await route(request, env);
    } catch (e) {
      if (e instanceof HttpError) return json(e.body, e.status);
      console.error(e && e.stack ? e.stack : e);
      return json({ error: "XeleeNet had a problem; try again", reason: "server" }, 500);
    }
  },
};

async function route(request, env) {
  const url = new URL(request.url);
  const method = request.method;
  await limit(env.PER_IP, ip(request), "Too many requests; wait a minute");

  if (url.pathname === "/v1/session" && method === "POST") return session(request, env);
  if (url.pathname === "/v1/session/verify" && method === "POST") return verify(request, env);
  if (url.pathname === "/v1/lobbies" && method === "GET") return list(request, env, url);
  if (url.pathname === "/v1/lobbies" && method === "POST") return host(request, env);
  if (url.pathname === "/v1/join" && method === "POST") return join(request, env);

  const m = LOBBY_PATH.exec(url.pathname);
  if (!m) throw new HttpError(404, "not_found", "Not found");
  const [, id, action] = m;
  const me = await authorize(request, env);
  const lobby = env.LOBBY.get(env.LOBBY.idFromName(id));
  const call = `${method} ${action || ""}`;
  switch (call) {
    case "POST heartbeat": {
      const b = await body(request);
      const roster = Array.isArray(b.roster) ? b.roster.filter((k) => typeof k === "string").slice(0, 3) : [];
      const state = b.state === "playing" ? "playing" : "open";
      const score = Array.isArray(b.score) ? b.score.slice(0, 4).map((n) => Number(n) | 0) : undefined;
      return reply(await lobby.heartbeat(me.pubkey, { state, roster, score, xr: b.xr === true }));
    }
    case "PUT settings": {
      const b = await body(request);
      return reply(await lobby.settings(me.pubkey, {
        name: lobbyName(b.name, env),
        visibility: visibility(b.visibility),
        ask: b.ask === true,
        settings: settings(b.settings),
      }));
    }
    case "POST admit": {
      const b = await body(request);
      return reply(await lobby.admit(me.pubkey, String(b.key || ""), b.accept === true));
    }
    case "POST kick": {
      const b = await body(request);
      return reply(await lobby.kick(me.pubkey, String(b.key || "")));
    }
    case "DELETE ":
    case "POST close": // the same, for clients that only speak GET and POST (the game)
      return reply(await lobby.close("closed by host", me.pubkey));
    case "GET join":
      return reply(await lobby.joinStatus(me.pubkey));
    case "POST leave":
      return reply(await lobby.leave(me.pubkey));
  }
  throw new HttpError(405, "bad_request", "Not allowed");
}

// --- session ------------------------------------------------------------------

function parseKey(text) {
  const bytes = unb64url(text);
  if (!bytes || bytes.length !== 32) throw new HttpError(400, "bad_request", "Malformed key");
  return bytes;
}

async function session(request, env) {
  const b = await body(request);
  parseKey(b.pubkey);
  return json({ nonce: await makeNonce(env, b.pubkey) });
}

async function verify(request, env) {
  const b = await body(request);
  const key = parseKey(b.pubkey);
  const sig = unb64url(b.sig);
  if (!(await checkNonce(env, b.pubkey, b.nonce)) || !sig || sig.length !== 64 ||
      !(await verifySignature(key, sig, sessionMessage(b.nonce, b.pubkey)))) {
    throw new HttpError(401, "unauthorized", "Couldn't sign in to XeleeNet");
  }
  const expires = now() + TOKEN_SECS;
  return json({ token: await makeToken(env, b.pubkey, expires), expires, tag: await tagOf(key) });
}

// --- lobbies --------------------------------------------------------------------

async function list(request, env, url) {
  await authorize(request, env);
  const c = compat({
    proto: Number(url.searchParams.get("proto")),
    rev: url.searchParams.get("rev") || "",
    disc: url.searchParams.get("disc") || "",
  });
  return json(await directory(env).list({ compat: c, full: url.searchParams.get("full") === "1", from: geo(request) }));
}

async function host(request, env) {
  const me = await authorize(request, env);
  await limit(env.HOSTS, me.pubkey, "You've opened a lot of lobbies; wait a minute");
  const b = await body(request);
  const init = {
    name: lobbyName(b.name, env),
    public: visibility(b.visibility) === "public",
    ask: b.ask === true,
    compat: compat(b.compat),
    settings: settings(b.settings),
    host: {
      key: me.pubkey, tag: me.tag, name: playerName(b.player, env), xr: b.xr === true,
      candidates: candidates(b.candidates), geo: geo(request), port: 0,
    },
  };
  const id = [...randomBytes(16)].map((x) => x.toString(16).padStart(2, "0")).join("");
  const { code, replaced } = await directory(env).reserve(id, me.pubkey);
  if (replaced) await env.LOBBY.get(env.LOBBY.idFromName(replaced)).close("host opened another lobby");
  return reply(await env.LOBBY.get(env.LOBBY.idFromName(id)).create({ ...init, id, code }));
}

async function join(request, env) {
  const me = await authorize(request, env);
  await limit(env.JOINS, ip(request), "Too many join attempts; wait a minute");
  const b = await body(request);
  const code = normalizeCode(b.code);
  if (!code) throw new HttpError(400, "bad_code", "Lobby codes are six letters and digits");
  const joiner = {
    key: me.pubkey, tag: me.tag, name: playerName(b.player, env), xr: b.xr === true,
    compat: compat(b.compat), candidates: candidates(b.candidates), geo: geo(request),
  };
  const id = await directory(env).resolve(code);
  if (!id) throw new HttpError(404, "not_found", "No lobby with that code");
  return reply(await env.LOBBY.get(env.LOBBY.idFromName(id)).join(joiner));
}

// --- helpers ----------------------------------------------------------------------

function directory(env) {
  return env.DIRECTORY.get(env.DIRECTORY.idFromName("directory"));
}

function reply({ status, body: b }) {
  return json(b, status);
}

function visibility(v) {
  if (v !== "public" && v !== "private") throw new HttpError(400, "bad_request", "Visibility is public or private");
  return v;
}

function ip(request) {
  return request.headers.get("CF-Connecting-IP") || "local";
}

async function limit(binding, key, message) {
  if (!binding) return;
  const { success } = await binding.limit({ key });
  if (!success) throw new HttpError(429, "rate_limited", message);
}

async function body(request) {
  const text = await request.text();
  if (text.length > MAX_BODY) throw new HttpError(413, "bad_request", "Request too large");
  try {
    const v = JSON.parse(text || "{}");
    if (v && typeof v === "object" && !Array.isArray(v)) return v;
  } catch {
    // fall through
  }
  throw new HttpError(400, "bad_request", "Malformed JSON");
}
