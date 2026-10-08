# Installing Road Trip Adventure on a Mac

This is an unofficial native port of *Road Trip Adventure* (PAL, SLES-51356). It needs an Apple Silicon Mac running macOS 13 Ventura or later, plus **your own copy of the PAL disc** as a disc image. The app contains no game data. On first launch it asks for your disc.

## 1. Install

1. Open `Road Trip Adventure.dmg`.
2. Drag **Road Trip Adventure** onto the **Applications** folder in the same window.
3. Eject the disk image.

## 2. First launch (Gatekeeper)

The app is ad-hoc signed but not notarised, because the project has no Apple Developer ID. The first time you open it, macOS blocks it with a message such as *"Road Trip Adventure" cannot be opened because Apple cannot check it for malicious software* or *"Road Trip Adventure" Not Opened*. You only have to allow it once.

**macOS 15 Sequoia and later**
1. Double-click the app and choose **Done** (or **OK**) in the warning.
2. Open **System Settings > Privacy & Security**.
3. Scroll down to the message about "Road Trip Adventure" and click **Open Anyway**.
4. Confirm with your password or Touch ID, then click **Open Anyway** again.

**macOS 13 Ventura and 14 Sonoma**
1. In Finder, right-click (or Control-click) the app in Applications and choose **Open**.
2. Click **Open** in the dialog.

**Any version (Terminal)**
```sh
xattr -dr com.apple.quarantine "/Applications/Road Trip Adventure.app"
```

Building from source (see the main README) avoids the warning completely.

## 3. Choose your disc (first run only)

1. The app explains that it needs your disc. Click **Choose Disc…**.
2. Select a disc image of the PAL release, SLES-51356 (Europe/Australia). Supported formats:
   - an `.iso`;
   - a `.cue` together with its `.bin` (raw 2352-byte sectors, as written by most disc-dumping tools). Keep both files in the same folder.
3. The app checks the disc: it must be a PlayStation 2 disc, its `SYSTEM.CNF` must boot `SLES_513.56`, and the image must be complete. A different game, region or a damaged image shows an error, and you can choose another file.
4. The app copies the game files once into its settings folder (about 520 MB; a BIN image is also converted once into an ISO, about another 520 MB). This takes a few seconds. After that, the game starts.

To switch to a different disc image, quit the app and delete `general.json` (see below). The next launch asks again. If the image you chose earlier is moved or deleted, the app also asks again.

## 4. Where things are stored

Everything is in `~/Library/Application Support/RoadTripAdventure/`. In Finder, choose **Go > Go to Folder…** and paste that path.

| Path | What |
| --- | --- |
| `general.json` | The path of your disc image |
| `graphics.json` | Window settings: resolution scale, aspect, full screen (View menu) |
| `mc0/` | Memory card 1: **your saves**. Back this folder up |
| `disc/`, `disc.iso` | Files prepared from your disc. You can delete them; they are recreated from your image |

Shader caches live in `~/Library/Caches/RoadTripAdventure`. They are safe to delete.

To uninstall, move the app to the Bin. Delete the Application Support folder only if you no longer want your saves.

## 5. Controls

| Action | Keyboard | Controller |
| --- | --- | --- |
| Steer / menus | Arrow keys | Left stick / D-pad |
| Cross (accelerate / confirm) | X | South button (A / Cross) |
| Circle (back) | C or Esc | East button (B / Circle) |
| Square | Z | West button |
| Triangle | V | North button |
| Start | Return | Start / Menu |
| Full screen | Cmd+F | |

Game controllers (Xbox, PlayStation, Switch Pro and other MFi pads) are picked up automatically, including when you plug them in while the game is running. The **View** menu sets the resolution (1x to 8x; it applies on the next launch) and the aspect ratio (4:3 or Stretch).
