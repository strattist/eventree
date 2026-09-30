# 02: Record the ADR for the time-major event tree

**What to build:** A written decision record explaining why the event tree is time-major with compile-time layouts and a packed layout first, so a future reader understands why masks under y and base x were dropped.

**Blocked by:** None (can start immediately).

**Status:** done

- [x] An ADR exists in the repo's ADR directory following the ADR format, numbered 0001
- [x] It records the alternatives considered: per-event structs, a mask tree under y and base x, and the time-major tree with layouts
- [x] It cites the measurements from the spec: events per µs timestamp, and how few events fill an EVT2 mask
- [x] It states the consequences: masks return as a second layout when real EVT2.1 files are available
- [x] It uses the terms from `CONTEXT.md`
