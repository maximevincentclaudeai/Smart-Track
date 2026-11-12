# Smart Track (AZ-MODS)

Touch the Tag List tab and an overlay lists the tracks that fit the playing one: BPM (+/- 3, 6, 8, 10),
key (same, +/-1 Camelot, +/-1 and relative) and playlist (the ones you watch), best match first, with a
proposed set name. The browse knob scrolls, CLICK shows the full title, BACK closes.

First use: Mod Menu > Smart Track > Calibrate Tag List tab, close the menu, open Browse and touch the
TAG LIST tab once. The SDK cannot see which Browse tab is showing, so the tab's position is learned once
and saved. Until then (or with "Open with: SMART button") a SMART button on Browse opens the overlay.

A status strip at the top of Browse shows the running version, the library state and the last touch
(Mod Menu > Smart Track > Status strip turns it off).

Playlists to watch: Mod Menu > Smart Track > Playlists to watch. Tick hip-hop, house...; leave ALL out.

Build (this folder next to `az-mods-sdk-main`):

    ../az-mods-sdk-main/tools/azmod-pack build .
    ../az-mods-sdk-main/tools/azmod-pack package .

GPL-3.0-only.
