# Smart Track (AZ-MODS)

Touch the Tag List tab and an overlay lists the tracks that fit the playing one: BPM (+/- 3, 6, 8, 10),
key (same, +/-1 Camelot, +/-1 and relative) and playlist (the ones you watch), best match first, with a
proposed set name. The browse knob scrolls (or touch the UP / DN buttons), CLICK shows the full title, BACK closes.

First use: open Browse, touch the SET TAG LIST TAB button at the top, then touch the TAG LIST tab once
(Mod Menu > Smart Track > Calibrate Tag List tab does the same). The SDK cannot see which Browse tab is showing, so the tab's position is learned once
and saved. Until then (or with "Open with: SMART button") a SMART button on Browse opens the overlay.

A status strip at the top of Browse shows the running version, the library state and the last touch
(Mod Menu > Smart Track > Status strip turns it off).

Playlists to watch: in the overlay, touch the PLAYLIST > button, then touch the playlists to tick
(hip-hop, house...; leave ALL out). Mod Menu > Smart Track > Playlists to watch opens the same list.

Build (this folder next to `az-mods-sdk-main`):

    ../az-mods-sdk-main/tools/azmod-pack build .
    ../az-mods-sdk-main/tools/azmod-pack package .

GPL-3.0-only.
