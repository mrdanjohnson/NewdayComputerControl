#!/bin/zsh
# Release through a Developer ID identity and an existing notarytool profile.
set -euo pipefail
native_root="$(cd "$(dirname "$0")/.." && pwd)"
: "${MACCONTROL_DEVELOPER_ID:?Set MACCONTROL_DEVELOPER_ID to your Developer ID Application certificate name}"
: "${MACCONTROL_TEAM_ID:?Set MACCONTROL_TEAM_ID to your Apple Developer team ID}"
: "${MACCONTROL_NOTARY_PROFILE:?Set MACCONTROL_NOTARY_PROFILE to an existing notarytool Keychain profile}"
if [[ "$MACCONTROL_DEVELOPER_ID" != 'Developer ID Application:'* ]]; then
  print -u2 'Client distribution requires a Developer ID Application identity, not Apple Development or ad hoc signing.'
  exit 1
fi
"$native_root/Scripts/build.sh" CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY="$MACCONTROL_DEVELOPER_ID" DEVELOPMENT_TEAM="$MACCONTROL_TEAM_ID"
app_path="$native_root/build/MacControl.app"
archive_path="$native_root/build/MacControl-2.1.5.zip"
xcrun notarytool submit "$archive_path" --keychain-profile "$MACCONTROL_NOTARY_PROFILE" --wait
xcrun stapler staple "$app_path"
xcrun stapler validate "$app_path"
codesign --verify --deep --strict "$app_path"
spctl --assess --type execute --verbose=2 "$app_path"
ditto -c -k --sequesterRsrc --keepParent "$app_path" "$archive_path"
print "Notarized client package: $archive_path"
