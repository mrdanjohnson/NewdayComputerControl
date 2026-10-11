#!/bin/zsh
set -euo pipefail
native_root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$native_root/build"
python3 "$native_root/Scripts/generate_project.py" > "$native_root/build/project-generation.log"
xcodebuild -project "$native_root/MacControl.xcodeproj" -scheme MacControl -configuration Release -destination 'generic/platform=macOS' -derivedDataPath "$native_root/build/ReleaseDerivedData" ARCHS='arm64 x86_64' ONLY_ACTIVE_ARCH=NO ENABLE_DEBUG_DYLIB=NO OTHER_CODE_SIGN_FLAGS='--options runtime' "$@" build > "$native_root/build/release-build.log" 2>&1
app_path="$native_root/build/ReleaseDerivedData/Build/Products/Release/MacControl.app"
codesign --verify --deep --strict "$app_path"
ditto "$app_path" "$native_root/build/MacControl.app"
ditto -c -k --sequesterRsrc --keepParent "$native_root/build/MacControl.app" "$native_root/build/MacControl-2.1.3.zip"
python3 "$native_root/Scripts/verify_bundle.py" "$native_root/build/MacControl.app"
