# Smart Track (AZ-MODS)

Touch the SMART TRACK button at the top right of Browse and an overlay lists the tracks that fit the
playing one: BPM (+/- 3, 6, 8, 10), key (same, +/-1 Camelot, +/-1 and relative) and playlist (the ones you
watch), best match first, with a proposed set name. The browse knob scrolls (or touch the UP / DN buttons),
BACK closes.

**Select in library (experimental).** Double-tap a track in the list (a single tap only highlights it), or press the browse knob on it: the overlay
closes and EP147's track list scrolls to that track; press LOAD on the deck to load it. The SDK has no
call to select a track, so the mod turns the browse knob for you (a mixer frame filter): it goes to the
top of the list, then down to the track's position. For the landing to be right, set
Mod Menu > Smart Track > Library order to the way your deck sorts its track list (Title A-Z, Artist A-Z or
as the stick lists them) and open the TRACK list of the right USB first. Turning the knob yourself stops a
jump. Scroll speed: Careful (1 step at a time), Normal, Fast. Turn the whole thing off with
"Select in library"; the tap then only shows the full title.

A status strip at the top left of Browse shows the running version and the library state. Touch its
X HIDE button to remove it (Mod Menu > Smart Track > Status strip brings it back).

Playlists to watch: in the overlay, touch the PLAYLIST > button, then touch the playlists to tick
(hip-hop, house...; leave ALL out). Mod Menu > Smart Track > Playlists to watch opens the same list.

Build (this folder next to `az-mods-sdk-main`):

    ../az-mods-sdk-main/tools/azmod-pack build .
    ../az-mods-sdk-main/tools/azmod-pack package .

GPL-3.0-only.
