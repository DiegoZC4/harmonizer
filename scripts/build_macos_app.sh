#!/bin/zsh

set -euo pipefail

root="${0:A:h:h}"
version="$(<"$root/VERSION.txt")"
build_number="${GITHUB_RUN_NUMBER:-1}"
binary="${HARMONIZER_BINARY:-$root/harmonizer_web_portable}"
output_dir="${HARMONIZER_OUTPUT_DIR:-$root/dist}"
label="${HARMONIZER_MAC_LABEL:-$(/usr/bin/uname -m)}"
identity="${APPLE_SIGNING_IDENTITY:--}"
app="$output_dir/Harmonizer.app"
archive="$output_dir/Harmonizer-macOS-${label}.zip"
dmg="$output_dir/Harmonizer-macOS-${label}.dmg"
staging="$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/harmonizer-macos.XXXXXX")"
app_staging="$staging/Harmonizer.app"
dmg_staging="$staging/dmg"

cleanup() {
  /bin/rm -rf "$staging"
}
trap cleanup EXIT

if [[ ! -x "$binary" ]]; then
  print -u2 "Missing executable: $binary"
  exit 1
fi

/bin/mkdir -p "$output_dir"

/bin/mkdir -p \
  "$app_staging/Contents/MacOS" \
  "$app_staging/Contents/Resources/web"

/usr/bin/sed \
  -e "s/@HARMONIZER_VERSION@/$version/g" \
  -e "s/@HARMONIZER_BUILD@/$build_number/g" \
  "$root/packaging/macos/Info.plist" > "$app_staging/Contents/Info.plist"
/bin/cp "$root/packaging/macos/Harmonizer" "$app_staging/Contents/MacOS/Harmonizer"
/bin/cp "$binary" "$app_staging/Contents/MacOS/harmonizer_web"
/bin/cp "$root/web/index.html" "$app_staging/Contents/Resources/web/index.html"
/bin/cp "$root/README.md" "$app_staging/Contents/Resources/README.md"
/bin/cp "$root/QUICKSTART.md" "$app_staging/Contents/Resources/QUICKSTART.md"
/bin/cp "$root/LICENSE" "$app_staging/Contents/Resources/LICENSE"
/bin/cp "$root/THIRD_PARTY_NOTICES.md" \
  "$app_staging/Contents/Resources/THIRD_PARTY_NOTICES.md"
/bin/chmod +x \
  "$app_staging/Contents/MacOS/Harmonizer" \
  "$app_staging/Contents/MacOS/harmonizer_web"

if [[ "$identity" == "-" ]]; then
  /usr/bin/codesign --force --deep --sign - "$app_staging"
else
  /usr/bin/codesign \
    --force \
    --options runtime \
    --timestamp \
    --entitlements "$root/packaging/macos/Harmonizer.entitlements" \
    --sign "$identity" \
    "$app_staging/Contents/MacOS/harmonizer_web"
  /usr/bin/codesign \
    --force \
    --options runtime \
    --timestamp \
    --entitlements "$root/packaging/macos/Harmonizer.entitlements" \
    --sign "$identity" \
    "$app_staging"
fi

/usr/bin/codesign --verify --deep --strict --verbose=2 "$app_staging"

/bin/rm -rf "$app" "$archive" "$dmg"
/usr/bin/ditto "$app_staging" "$app"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$app" "$archive"

/bin/mkdir -p "$dmg_staging"
/usr/bin/ditto "$app" "$dmg_staging/Harmonizer.app"
/bin/ln -s /Applications "$dmg_staging/Applications"
/usr/bin/hdiutil create \
  -volname "Harmonizer" \
  -srcfolder "$dmg_staging" \
  -ov \
  -format UDZO \
  "$dmg"

print "Built $app"
print "Archived $archive"
print "Created $dmg"
