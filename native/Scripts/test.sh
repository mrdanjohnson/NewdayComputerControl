#!/bin/zsh
set -euo pipefail
native_root="$(cd "$(dirname "$0")/.." && pwd)"
project_root="$(cd "$native_root/.." && pwd)"
mkdir -p "$native_root/build"
MACCONTROL_CAPTURE="$native_root/build/swift-wire-events.jsonl" swift test --package-path "$native_root/Packages/MacControlCore" > "$native_root/build/tests.log" 2>&1
json_headers="$project_root/firmware/.pio/libdeps/native/ArduinoJson/src"
if [[ ! -f "$json_headers/ArduinoJson.h" ]]; then
  print -u2 "Swift tests passed. Firmware parser check needs the existing native PlatformIO dependencies. Run the firmware native setup, then rerun this script."
  exit 1
fi
xcrun clang++ -std=c++17 -I"$project_root/firmware/lib/maccontrol_core" -I"$json_headers" "$native_root/Tests/validate_wire.cpp" "$project_root/firmware/lib/maccontrol_core/mc_agent_events.cpp" -o "$native_root/build/validate-wire"
"$native_root/build/validate-wire" < "$native_root/build/swift-wire-events.jsonl"
python3 "$native_root/Scripts/generate_project.py"
xcodebuild -project "$native_root/MacControl.xcodeproj" -scheme MacControl -destination 'platform=macOS' -derivedDataPath "$native_root/build/DerivedData" build > "$native_root/build/debug-build.log" 2>&1
python3 "$native_root/Scripts/startup_smoke.py" "$native_root/build/DerivedData/Build/Products/Debug/MacControl.app"
python3 "$native_root/Scripts/ipc_smoke.py" "$native_root/build/DerivedData/Build/Products/Debug/MacControl.app" > "$native_root/build/ipc-smoke.log" 2>&1
cat "$native_root/build/ipc-smoke.log"
