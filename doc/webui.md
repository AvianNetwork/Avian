# Web UI

`aviand` (and `avian-qt`, which runs the same node internally) can optionally
serve a browser-based wallet and node management interface directly — no
separate process, reverse proxy, or Tor hidden service required for local
use. It is built as a single-page React app and embedded into the binary at
compile time.

The Web UI is off by default and must be explicitly enabled.

## Enabling

The feature is compiled in via a CMake option:

```
cmake -B build -DWITH_WEBUI=ON
```

and enabled at runtime with `-webui`, on either binary:

```
aviand -webui=1
```
```
avian-qt -webui=1
```

`avian-qt` goes through the same startup path as `aviand` (`AppInitMain` /
`AppInitServers`), so setting `-webui=1` there starts the real HTTP server
too — it isn't only the "Open Web UI…" menu item described below.

By default the Web UI shares the existing RPC HTTP server and port (so on
mainnet that's the same port as `-rpcport`). Related options:

| Option | Default | Description |
|---|---|---|
| `-webui` | `0` | Enable the Web UI endpoint on the RPC server port. |
| `-webuibind=<addr>` | unset | Bind address for a **dedicated** Web UI port. |
| `-webuiport=<port>` | `7897` | Dedicated port for the Web UI. Setting either `-webuiport` or `-webuibind` (or both) opens a dedicated listener; whichever of the two you don't set falls back to its own default (`7897` / `127.0.0.1`). Leave both unset to keep sharing the RPC port. |
| `-webuiassets=<dir>` | unset | Serve static files from `<dir>` on disk instead of the embedded bundle. Intended for frontend development only — never set this in production. |
| `-webuipassword=<password>` | unset | Use a fixed password instead of the per-session cookie token (see [Authentication](#authentication)). Marked sensitive — set it in `avian.conf`, not on the command line, so it doesn't leak via `ps`. |

Once running, open `http://127.0.0.1:<port>/webui/` in a browser.

If you're running `avian-qt` with `-webui=1`, **File → Open Web UI…** saves
you the manual step — it reads the bind address/port from your config and
opens the page with the session token already attached (see below), so
there's no copy-pasting a cookie file by hand. The menu item is only enabled
when `-webui=1` is set, since that's what makes the server exist in the
first place.

## Authentication

The Web UI is meant for local/trusted-network use, alongside `avian-cli` and
the RPC server it talks to. It supports two mutually exclusive auth modes:

**Cookie mode (default).** On startup, 32 random bytes are generated,
hex-encoded, and written to `<datadir>/webui.cookie` (one token per process
lifetime, file permissions restricted to the owner). The frontend reads this
value as a bearer token. This mirrors the existing `.cookie` file the RPC
server itself uses for `avian-cli`.

**Password mode.** Set `-webuipassword=<password>` (in `avian.conf`) to use a
fixed password instead of the generated cookie. The password itself is never
used as a bearer token: the login page exchanges it once, via `POST
/webui/api/auth/login`, for a random 32-byte session token, and only that
token is stored in the browser and sent on subsequent requests. The
configured password is verified against a salted PBKDF2-HMAC-SHA256 hash (not
a single fast hash) — since this check only runs at login, not on every
request, it can afford to be deliberately slow without making the UI feel
sluggish. "Log out" calls `POST /webui/api/auth/logout` to revoke that
session token server-side, in addition to clearing it from the browser.

Every request to `/webui/api/*` must include `Authorization: Bearer <token>`.
Missing or incorrect tokens get a `401` with a deliberate delay (to slow down
brute-forcing) — they are otherwise indistinguishable from a non-existent
endpoint. Two more checks happen before auth is even considered:

- **Host header allowlist** — rejects requests whose `Host` header isn't a
  recognized local address, as a defense against DNS-rebinding attacks from a
  malicious webpage open in the same browser.
- **Origin allowlist / CORS** — only same-origin or explicitly allowed local
  origins get `Access-Control-Allow-Origin` echoed back; everything else is
  refused before it reaches any handler.

Static files (the HTML/JS/CSS bundle itself) are served without auth, since
they contain no user data — only the `/webui/api/*` calls the page makes
after loading are gated.

The live event stream (`/webui/api/events`, used for block/mempool push
notifications) is the one exception to "always send a bearer token": browsers'
`EventSource` API cannot set an `Authorization` header, so the token can't be
attached the normal way. Rather than put the real, long-lived token in that
URL — where it could end up in a reverse proxy's access log, browser history,
or a screenshot — the frontend first calls `POST /webui/api/events/ticket`
(itself authenticated normally) to mint a random, single-use ticket good for
one connection attempt within 30 seconds, and puts that in the `/events` URL
instead. A leaked ticket is worthless almost immediately; the actual token
never appears in it.

## ⚠️ The RPC Console can run anything

The Web UI includes an RPC Console page that accepts arbitrary RPC method
names and parameters, and executes them with the same privileges as
`avian-cli` — including wallet-unlocking, key-dumping, and node-shutdown
commands. This is intentional and matches the trust model of `avian-cli`
itself: anyone who can authenticate to the Web UI already has the
`webui.cookie` (or password) and could just run `avian-cli` directly with the
same effect.

In other words: treat access to the Web UI as equivalent to having a local
RPC credential. Don't expose `-webuibind` to an untrusted network, and don't
set `-webuipassword` to something weak or shared.

## Frontend development

The frontend source lives in `src/webui/ui/` (React + TypeScript + Vite). It
is **not** built automatically by CMake — the compiled output is checked into
the repo as a generated header, `src/webui/assets.h`, and must be
regenerated by hand after any frontend change:

```
cd src/webui/ui
npm install
npm run build    # type-check + production build to dist/
npm run embed    # build + regenerate ../assets.h from dist/
```

For iterating on the UI without recompiling `aviand` after every change, run
`npm run dev` for a hot-reloading Vite dev server, or point a running
`aviand -webui=1` at an unpacked build with `-webuiassets=<path-to-dist>` to
serve files straight off disk.
