# Eventree

A standalone library that decodes event-camera raw streams into compact in-memory event trees, for lower resident memory and lower decoding latency than openeb's per-event structs. First scope: EVT2 and EVT3; EVT2.1 and EVT4 later.

## Language

**Event tree**:
The representation a decoder produces: time high → time low → the events sharing that timestamp. What a time node holds is set by its layout. Timestamps are stored split and are normalised to one value per CD event only when read back. Convertible to other, richer formats.
_Avoid_: Intermediate format, event batch, event vector

**Layout**:
The way an event tree stores the events under one time node. A decoder produces the layout that fits its wire format.
_Avoid_: Encoding, schema, node type

**Packed layout**:
A layout that stores each CD event as one packed word (y, x, polarity) under its time node.
_Avoid_: Flat layout

**Mask layout**:
A layout that stores, under each time node, rows of base x with one positive and one negative polarity mask. Deferred until real EVT2.1 files are available.
_Avoid_: Vector layout

**Polarity mask**:
In a mask layout, a packed set of bits over the pixels starting at one base x, where a set bit means a CD event exists at that pixel.
_Avoid_: Valid bits, vector mask, bitmap

**Base x**:
In a mask layout, the first x coordinate covered by a polarity mask; bit i stands for x = base x + i.
_Avoid_: Block index, x offset

**Time high / time low**:
The two time levels of an event tree: time low is the lowest 6 bits of the timestamp in µs, time high is the remaining bits with time loops unrolled. The split is the same for every wire format.
_Avoid_: Timestamp bucket

**Decoder**:
A component that translates one raw wire format into an event tree.
_Avoid_: Parser, reader

**Raw stream**:
The bytes of a recording or live feed in one of the wire formats.
_Avoid_: Raw data, buffer

**CD event**:
A single change-detection event: a pixel position, a polarity, and a timestamp. The only event kind in scope for now.
_Avoid_: Contrast event, pixel event

**Event pool**:
A memory pool that owns the storage of event trees; it can grow when needed, and its memory is reused once allocated.
_Avoid_: Arena, allocator

**Reference decoder**:
openeb's own decoder, used only as the oracle that our output must match exactly and as the benchmark baseline.
_Avoid_: Legacy decoder, original decoder
