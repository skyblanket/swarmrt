# Encrypted, replay-protected distribution (#40)

Status: step 1 landed (opt-in TLS 1.3 with a cookie-derived PSK). The
plaintext protocol is unchanged and stays the default.

## Problem

Node links (`src/swarmrt_node.c`) carry length-prefixed frames over plain
TCP. With `SW_NODE_COOKIE` each frame has a SHA-256 MAC over
`cookie || header || payload`, which authenticates the frame. Nothing hides
the contents, and nothing binds a frame to a connection or a position in the
stream. Anyone on the path can read every message and replay any recorded
frame, on the same link or a new one, and it is accepted again.

## Options

| | What it is | For | Against |
|---|---|---|---|
| A. TLS 1.3, external PSK from the cookie | OpenSSL with `psk_dhe_ke` and no certificates. PSK = SHA-256(label ‖ cookie). | No new configuration: the cookie clusters already set becomes the key. Each handshake mixes fresh randoms and an ECDHE share from both ends, so a recorded session replayed on a new connection fails. TLS record sequence numbers stop in-link replay and reordering. Forward secrecy comes from ECDHE. A standard protocol, not ours. | A weak cookie can be brute-forced offline from one captured handshake, like any PSK. Needs OpenSSL, which is not in the default macOS build. |
| B. TLS 1.3 with certificates (mTLS) | Per-node cert and key, plus a cluster CA. | Strongest identity: per-node keys can be revoked. No offline guessing. | Every deployment must issue and rotate certificates before it can turn this on. |
| C. Noise (e.g. `Noise_NNpsk0`) | A small handshake on libsodium or a vendored implementation. | Compact, with no ASN.1 or X.509. | A new dependency, or new crypto code to maintain. OpenSSL is already linked. |
| D. Hand-rolled nonce handshake plus AEAD frames | Per-link nonces, HKDF, AES-GCM, sequence numbers. | No dependency on OpenSSL's TLS stack. | Homemade protocol design. This is what TLS already is. |

## Recommendation and what landed

**A first, B as an add-on later.** A turns the existing cookie into real
transport security with one environment variable, and it uses the library we
already link. B can be layered on later: set `SW_NODE_TLS_CERT`,
`SW_NODE_TLS_KEY` and `SW_NODE_TLS_CA` in the same contexts and keep the PSK
for authorisation.

How step 1 works:

- **Opt-in** with `SW_NODE_TLS=1`, set on every node. It requires
  `SW_NODE_COOKIE`. Without the cookie, or in a build without OpenSSL,
  `node_start` and `node_connect` fail and print the reason. There is never a
  silent plaintext fallback.
- **Protocol.** TLS 1.3 only. Suites are `TLS_AES_128_GCM_SHA256` and
  `TLS_CHACHA20_POLY1305_SHA256` (the PSK is bound to SHA-256). No session
  tickets and no resumption, so every link does a full PSK+ECDHE handshake.
  0-RTT is off, the OpenSSL default.
- **PSK** = SHA-256(`"swarmrt distribution tls psk v1\0"` ‖ cookie). The
  identity is `swarmrt-dist-v1`. The per-frame cookie MAC still runs inside
  TLS. It is redundant there but harmless.
- **Data path.** The IO layer still owns the socket. Each peer has an `SSL`
  with two memory BIOs:
  - `PORT_DATA` bytes are written into the read BIO;
  - plaintext from `SSL_read` goes into the existing frame buffer;
  - the write BIO is flushed to the socket.
  The frame parser is unchanged.
- **Handshakes.** The connecting side handshakes synchronously inside
  `node_connect`, which already blocks on `connect()`. A failed handshake
  (wrong cookie, a plaintext peer, a hang-up, or a 5 s timeout) is
  `node_connect`'s error. The accepting side handshakes from `dist_handler`
  as bytes arrive. If that fails, it closes the link and prints why.
- **Locking.** All `SSL` calls happen under `g_dist_lock`.
- **Mixed clusters are refused.** A TLS node drops plaintext bytes, and a
  plaintext node cannot parse a ClientHello.

Tests:

- `make test-node-tls` (`src/test_node_tls.c`) sends traffic through a
  recording proxy and checks four things:
  - the message is delivered;
  - the secret is absent from the captured bytes;
  - replaying the captured stream on a new connection delivers nothing;
  - a plaintext frame with a valid cookie MAC is refused, and so is a peer
    that hangs up on the handshake.
- `tests/sw/test_node_tls.sw` runs three OS processes. A round trip over TLS
  works, and a node with a different cookie gets an error from
  `node_connect`.

## Known limits of step 1

- **Weak cookies.** A captured handshake allows offline guessing of a weak
  cookie. Use 32 or more random bytes, e.g. `openssl rand -hex 32`.
- **Plaintext by default.** Distribution is still plaintext unless the flag
  is set. Making TLS the default is a later, breaking decision.
- **Blocking handshake.** The connecting handshake blocks one scheduler
  thread for up to one round trip (worst case 5 s). The accepting side
  handshakes in `dist_handler`, which needs a scheduler thread of its own,
  so do not connect a TLS link from a process on a runtime with
  `SW_SCHEDULERS=1`.
- **macOS.** Needs `make SWARMRT_TLS=1` with Homebrew `openssl@3`. Without
  it, the flag is refused loudly.
- **No per-node identity.** There is no per-node identity or revocation
  (option B). Any holder of the cookie is a full member.

## Decisions for the maintainer

1. TLS is opt-in (`SW_NODE_TLS=1`), not the default.
2. The key is a PSK from the cookie (option A), not certificates. B can be
   added later.
3. A TLS node refuses plaintext peers. There is no mixed or upgrade mode.
4. The connecting handshake is synchronous inside `node_connect`, with a 5 s
   timeout.
5. The per-frame cookie MAC is kept inside TLS, so frame handling is
   identical in both modes.
