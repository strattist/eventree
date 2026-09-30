# The event tree is time-major, with compile-time layouts and the packed layout first

The **event tree** is organised **time high** → **time low** → the **CD events** sharing that timestamp. What a time node holds is set by a **layout** chosen at compile time through a C++20 concept. Milestone 1 ships only the **packed layout**: one 32-bit word per CD event (y, x, polarity). The **mask layout** (rows of **base x** with a positive and a negative **polarity mask** under each time node) is deferred.

## Considered options

- **Per-event structs** (openeb's `EventCD`, 16 bytes each). Simple, but repeats the full timestamp in every event. Rejected: this is the memory and latency cost the library exists to remove.
- **A mask tree under y and base x.** Group events by pixel row and base x, store polarity masks. Rejected for now: it only pays off if many events land in the same mask.
- **A time-major tree with layouts.** Timestamps are stored once per time node; the per-node storage is a layout. Chosen.

## Measurements behind the decision

From the first 30–40 M words of the user's recordings (numpy analysis, not a built library):

- **EVT2:** 2–11 CD events share one µs timestamp, so factoring out time pays off. But even ideal grouping fills a 32-pixel polarity mask with only about 1–1.5 events, so mask levels do not pay off.
- **EVT3:** 54–98% of events are single-pixel words. Vector words average about one event; this figure is unvalidated.

The packed layout is estimated at 4.5–6.5 bytes per event against 16 for `EventCD`. These are estimates; milestone 1 replaces them with measured bytes per event.

## Consequences

- Time is the top of the tree for every wire format: each **decoder** converts its own time words into the same time high / time low split (6 low bits of µs; time loops unrolled).
- Layouts are compile-time parameters, so the hot path has no runtime dispatch. Iterators and getters are written once against the layout concept, so adding a layout later does not change the tree's public API.
- The mask layout returns as a second layout when real EVT2.1 files are available, since EVT2.1 words are natively vector-shaped and the EVT2 measurements above do not apply to them. Converting between layouts stays out of scope until then.
- The EVT3 vector-word statistic should be validated; if it holds, the mask layout may also be worth revisiting for EVT3.
