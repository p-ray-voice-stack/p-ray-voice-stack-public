# Hardware V3

`hardware-v3` is the advanced public hardware path that builds on the official `ESP-VoCat v1.2` baseline.

## Use This After Hardware V2

Start here only after:

1. `local-v2` is already clear
2. the `hardware-v2` baseline is understood
3. the board can already reach the public server path

## What V3 Adds

- a lower-latency direction for advanced teams
- a public place to explain the path beyond `v2 async`
- alignment with the `/api/v3/` server namespace
- a public-safe contract endpoint at `/api/v3/contract`

## Current Public Contract

The current public server-side step for `v3` is `GET /api/v3/contract`.
It describes the advanced-path boundary, the public routes that exist today,
and the items that remain out of scope for this repository stage.

The next public-safe step is `POST /api/v3/session`.
It now creates an in-process, in-memory, ephemeral accepted session record.
That record is not persisted, will be lost on process restart, and does not
indicate that audio or events transport is connected.

The matching read step is `GET /api/v3/session/{session_id}`.
It now returns a runtime-shaped public-preview status contract. It does not
read persisted runtime state. When the session was created in the current
process through `POST /api/v3/session`, it can read back that in-process
accepted session record and apply a single in-process progression under
controlled conditions inside the `GET` route. That progression can update the
public `updated_at` field, but it still does not indicate that audio, events
transport, or any provider is connected.

The close step is `DELETE /api/v3/session/{session_id}`.
It now applies minimal in-process close semantics for sessions that exist in
the current process store. Those sessions move to a retained closed terminal state
with a neutral server-side `final_reason` such as `client-closed`, and
`GET /api/v3/session/{session_id}` remains readable after close. Unknown
session IDs still stay within the public-preview close contract boundary. This
still does not terminate persisted runtime state and does not indicate that
audio or events transport is connected.

The events step is `GET /api/v3/session/{session_id}/events`.
It now returns a canonical public-preview event snapshot contract with an
in-process snapshot projection path for current-process records. That
projection is still not history / replay / stream: an active record only maps
to `session.active`, and a closed record only maps to a single terminal
projection event. All event fields are derived from a fixed minimal mapping of
the current in-process record, and the endpoint still excludes device-side
uplink control such as `listen`.

## What Stays Out Of Scope Here

- production deployment
- additional public hardware baselines
- a full board bring-up guide beyond the `ESP-VoCat v1.2` public baseline
