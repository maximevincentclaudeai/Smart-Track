# SMART TRACK (AZ-MODS)

**A SMART TRACK button for the Pioneer DJ XDJ-AZ Browse screen.** It opens an overlay showing tracks from the USB library that fit the track currently playing: BPM range (including pitch-fader adjustment), musical key (Camelot compatibility), and selected playlists. It also suggests a name for the set.

## Features

- Find tracks by BPM range.
- Filter by compatible musical key on the Camelot wheel.
- Limit results to playlists selected for monitoring.
- Suggest a set name from the playing track's musical context.
- Configure criteria in **Mod Menu > SMART TRACK**; settings are stored on the USB drive.

The mod is designed to read the rekordbox library and deck state. It does not use network access or patch firmware.

## Repository

- GitHub: https://github.com/maximevincentclaudeai/smart-track
- Maintainer: `maximevincentclaudeai`
- Mod ID: `smart-track`
- Current version: `0.1.0`

## Build and package

Place this repository next to a checkout of the AZ-MODS SDK:

```text
AZ-MODS/
├── az-mods-sdk-main/
└── smart-track/
```

From this repository directory, run:

```bash
../az-mods-sdk-main/tools/azmod-pack build .
../az-mods-sdk-main/tools/azmod-pack package .
```

For publishing, the GitHub Actions workflow builds and packages the mod when a matching version tag is pushed. For version `0.1.0`:

```bash
git tag smart-track-0.1.0
git push origin smart-track-0.1.0
```

The workflow creates a GitHub Release, attaches the `.azmod` package and icon, and updates `azmods-repo.json`.

## Install on the XDJ-AZ

After a successful release, add this repository in the AZ-MODS Mod Menu under **Community Mods > Repositories**:

```text
maximevincentclaudeai/smart-track
```

Then browse the repository and install **SMART TRACK**. Availability depends on a compatible AZ-MODS installation and firmware.

## License

GPL-3.0-only. See [LICENSE](LICENSE).
