#!/bin/bash
# Packages the built runner as a double-clickable macOS app and a drag-to-Applications DMG (GOALS X2).
#
# Usage: game/tools/make_app.sh [runner] [outdir]
#   runner  default: build the release runner in <port>/PS2Recomp/out/app (configured on first use):
#           same sources as the dev build (work/generated, work/vu1cat), plus
#           CMAKE_OSX_DEPLOYMENT_TARGET=13.0 (the dev build targets the build machine's macOS) and
#           PS2X_ENABLE_FFMPEG=OFF (FFmpeg only backs sceMpeg*/PSS movies; this game has no PSS files
#           and its code never calls sceMpeg, so the app needs no FFmpeg dylibs). Pass a runner path to
#           package an existing build instead (its non-system dylibs are bundled either way).
#   outdir  default: <port>/work/dist   (the runner contains recompiled game code, so the bundle and
#           DMG are disc-derived: keep them in work/, never commit or publish them)
# Output: "<outdir>/Road Trip Adventure.app" and "<outdir>/Road Trip Adventure.dmg".
#
# The app carries no disc data: on first launch it asks for the user's own disc image (see
# ps2_disc_setup.cpp). Every non-system dylib (FFmpeg and its dependencies) is copied into
# Contents/Frameworks and relinked to @rpath; the bundle is ad-hoc signed (no Developer ID, not
# notarised; see game/docs/INSTALL_MAC.md for the Gatekeeper steps).
# Needs only Xcode command line tools (otool, install_name_tool, codesign, sips, iconutil, hdiutil) and perl.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="$(cd "$HERE/../.." && pwd)"
RUNNER="${1:-}"
OUTDIR="${2:-$PORT/work/dist}"
if [ -z "$RUNNER" ]; then
  BUILD="$PORT/PS2Recomp/out/app"
  if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S "$PORT/PS2Recomp" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DPS2X_ENABLE_FFMPEG=OFF \
      -DPS2X_RUNNER_DIR="$PORT/work/generated" -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
      -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_LTO=OFF \
      -DPS2X_VU1_CATALOG_DIR="$PORT/work/vu1cat" -DPS2X_VU1_GENERATOR="$PORT/game/tools/vu1recomp/vu1recomp.py" \
      -DPS2X_VU1GEN_DIR="$PORT/work/vu1gen_app"
  fi
  cmake --build "$BUILD" --target ps2EntryRunner
  RUNNER="$BUILD/ps2xRuntime/ps2EntryRunner"
fi
NAME="Road Trip Adventure"
EXE="RoadTripAdventure"
BUNDLE_ID="io.github.catsandsoup.roadtripadventure"
VERSION="${RTA_APP_VERSION:-0.1.0}"

[ -x "$RUNNER" ] || { echo "runner not found: $RUNNER" >&2; exit 1; }
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"
APP="$OUTDIR/$NAME.app"
WORK="$(mktemp -d "$OUTDIR/.make_app.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$RUNNER" "$APP/Contents/MacOS/$EXE"
chmod 755 "$APP/Contents/MacOS/$EXE"

# ---------------------------------------------------------------- Info.plist
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundleDisplayName</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleExecutable</key><string>$EXE</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
  <key>NSHumanReadableCopyright</key><string>Unofficial fan port. Requires your own PAL disc (SLES-51356).</string>
</dict>
</plist>
EOF
plutil -lint "$APP/Contents/Info.plist" >/dev/null

# ---------------------------------------------------------------- icon (original placeholder art, no game assets)
# A 1024x1024 BMP drawn procedurally: rounded tile, sky gradient, sun, hills and a road with a dashed centre line.
perl - "$WORK/icon.bmp" <<'PERL'
use strict; use warnings;
my ($out) = @ARGV; my $N = 1024;
open(my $fh, '>:raw', $out) or die $!;
my $rowbytes = $N * 4;
print $fh pack('A2 V v v V', 'BM', 54 + $rowbytes * $N, 0, 0, 54);
print $fh pack('V l l v v V V l l V V', 40, $N, $N, 1, 32, 0, $rowbytes * $N, 2835, 2835, 0, 0);
my $R = 180; my ($m0, $m1) = (100, $N - 100);
sub inside { my ($x, $y) = @_;
  return 0 if $x < $m0 || $x >= $m1 || $y < $m0 || $y >= $m1;
  my $cx = $x < $m0 + $R ? $m0 + $R : ($x >= $m1 - $R ? $m1 - $R - 1 : $x);
  my $cy = $y < $m0 + $R ? $m0 + $R : ($y >= $m1 - $R ? $m1 - $R - 1 : $y);
  return (($x - $cx) ** 2 + ($y - $cy) ** 2) <= $R * $R; }
for (my $row = $N - 1; $row >= 0; $row--) {          # BMP rows bottom-up; $y counts from the top
  my $y = $row; my $line = '';
  for my $x (0 .. $N - 1) {
    my ($r, $g, $b, $a) = (0, 0, 0, 0);
    if (inside($x, $y)) {
      $a = 255; my $t = ($y - $m0) / ($m1 - $m0);
      ($r, $g, $b) = (int(70 + 120 * $t), int(150 + 70 * $t), int(235 - 40 * $t));        # sky
      if ((($x - 700) ** 2 + ($y - 330) ** 2) < 95 ** 2) { ($r, $g, $b) = (255, 214, 90); } # sun
      my $hill = 600 + 40 * sin($x / 90.0);
      if ($y > $hill) { ($r, $g, $b) = (86, 168, 82); }                                   # hills
      if ($y > 560) {                                                                     # road
        my $half = 30 + ($y - 560) * 0.95; my $dx = abs($x - 512);
        if ($dx < $half) {
          ($r, $g, $b) = (70, 74, 82);
          my $lane = 4 + ($y - 560) * 0.06;
          ($r, $g, $b) = (255, 255, 255) if $dx > $half - $lane * 1.4 && $dx < $half - $lane * 0.4;
          ($r, $g, $b) = (250, 200, 40) if $dx < $lane && (int(($y - 560) ** 0.8 / 22) % 2 == 0);
        }
      }
    }
    $line .= pack('C4', $b, $g, $r, $a);
  }
  print $fh $line;
}
close $fh;
PERL
ICONSET="$WORK/AppIcon.iconset"
mkdir -p "$ICONSET"
sips -s format png "$WORK/icon.bmp" --out "$WORK/icon1024.png" >/dev/null
for s in 16 32 128 256 512; do
  sips -z $s $s "$WORK/icon1024.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
  d=$((s * 2)); sips -z $d $d "$WORK/icon1024.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"

# ---------------------------------------------------------------- bundle dylibs
FW="$APP/Contents/Frameworks"
SEEN="$WORK/seen.txt"; : > "$SEEN"           # "<bundled name>\t<source realpath>"
QUEUE="$WORK/queue.txt"; : > "$QUEUE"        # "<bundled file>\t<original dir of that file>"
is_system() { case "$1" in /System/*|/usr/lib/*) return 0 ;; *) return 1 ;; esac; }
rpaths_of() { otool -l "$1" | awk '/cmd LC_RPATH/{f=1;next} f&&/ path /{print $2; f=0}'; }
deps_of() {  # load commands of $1 (skips a dylib's own id line)
  local id; id="$(otool -D "$1" | sed -n 2p)"
  otool -L "$1" | tail -n +2 | sed -E 's/^[[:space:]]+//; s/ \(compatibility.*$//' | while read -r d; do
    [ "$d" = "$id" ] && continue; echo "$d"; done
}
resolve() {  # $1 dep, $2 bundled file that references it, $3 that file's original dir
  local dep="$1" file="$2" odir="$3" rel cand rp
  case "$dep" in
    @loader_path/*) cand="$odir/${dep#@loader_path/}" ;;
    @executable_path/*) cand="$(dirname "$RUNNER")/${dep#@executable_path/}" ;;
    @rpath/*) rel="${dep#@rpath/}"; cand=""
      while read -r rp; do
        rp="${rp/@loader_path/$odir}"; rp="${rp/@executable_path/$(dirname "$RUNNER")}"
        if [ -f "$rp/$rel" ]; then cand="$rp/$rel"; break; fi
      done < <(rpaths_of "$file")
      [ -n "$cand" ] || { echo "cannot resolve $dep for $file" >&2; return 1; } ;;
    *) cand="$dep" ;;
  esac
  [ -f "$cand" ] || { echo "missing $cand (from $file)" >&2; return 1; }
  (cd "$(dirname "$cand")" && echo "$(pwd -P)/$(basename "$cand")")
}
printf '%s\t%s\n' "$APP/Contents/MacOS/$EXE" "$(dirname "$RUNNER")" >> "$QUEUE"
i=1
while :; do
  line="$(sed -n "${i}p" "$QUEUE")"; [ -n "$line" ] || break
  file="${line%%$'\t'*}"; odir="${line#*$'\t'}"
  while read -r dep; do
    [ -n "$dep" ] || continue
    is_system "$dep" && continue
    src="$(resolve "$dep" "$file" "$odir")"
    real="$src"
    base="$(basename "$dep")"
    if ! grep -q "^$base	" "$SEEN"; then
      printf '%s\t%s\n' "$base" "$real" >> "$SEEN"
      cp -L "$src" "$FW/$base"; chmod 644 "$FW/$base"
      install_name_tool -id "@rpath/$base" "$FW/$base" 2>/dev/null
      printf '%s\t%s\n' "$FW/$base" "$(dirname "$src")" >> "$QUEUE"
    fi
    [ "$dep" = "@rpath/$base" ] || install_name_tool -change "$dep" "@rpath/$base" "$file" 2>/dev/null
  done < <(deps_of "$file")
  i=$((i + 1))
done
# rpaths: the executable finds Frameworks; every bundled dylib finds its siblings. Drop build-machine paths.
fix_rpaths() {  # $1 file, $2 wanted rpath
  local rp have=0
  while read -r rp; do
    [ -n "$rp" ] || continue
    if [ "$rp" = "$2" ]; then have=1; else install_name_tool -delete_rpath "$rp" "$1" 2>/dev/null || true; fi
  done < <(rpaths_of "$1")
  [ $have = 1 ] || install_name_tool -add_rpath "$2" "$1" 2>/dev/null
}
fix_rpaths "$APP/Contents/MacOS/$EXE" "@executable_path/../Frameworks"
for f in "$FW"/*.dylib; do [ -e "$f" ] && fix_rpaths "$f" "@loader_path"; done

# ---------------------------------------------------------------- sign (ad hoc) and verify
for f in "$FW"/*.dylib; do [ -e "$f" ] && codesign --force --sign - --timestamp=none "$f" 2>/dev/null; done
codesign --force --deep --sign - --timestamp=none "$APP" 2>/dev/null
codesign --verify --deep --strict "$APP"

bad=0
for f in "$APP/Contents/MacOS/$EXE" "$FW"/*.dylib; do
  [ -e "$f" ] || continue
  while read -r dep; do
    is_system "$dep" && continue
    case "$dep" in
      @rpath/*) [ -f "$FW/${dep#@rpath/}" ] || { echo "UNBUNDLED $dep in $f" >&2; bad=1; } ;;
      *) echo "NON-BUNDLE DEP $dep in $f" >&2; bad=1 ;;
    esac
  done < <(deps_of "$f")
  if otool -l "$f" | grep -q "path /opt/\|path /usr/local/\|path /Users/"; then echo "BUILD-MACHINE RPATH in $f" >&2; bad=1; fi
done
[ $bad = 0 ] || { echo "bundle is not self-contained" >&2; exit 1; }
for f in "$APP/Contents/MacOS/$EXE" "$FW"/*.dylib; do  # LSMinimumSystemVersion 13.0 must be true
  [ -e "$f" ] || continue
  minos="$(otool -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit}')"
  [ "${minos%%.*}" -le 13 ] 2>/dev/null || echo "WARNING: $(basename "$f") needs macOS $minos (Info.plist says 13.0)" >&2
done
echo "bundled $(ls "$FW" | wc -l | tr -d ' ') dylibs; signature OK"

# ---------------------------------------------------------------- DMG
STAGE="$WORK/dmg"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
DMG="$OUTDIR/$NAME.dmg"
rm -f "$DMG"
hdiutil create -quiet -volname "$NAME" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG"
echo "app: $APP"
echo "dmg: $DMG ($(du -h "$DMG" | cut -f1))"
