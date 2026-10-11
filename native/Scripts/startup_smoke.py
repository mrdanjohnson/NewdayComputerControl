#!/usr/bin/env python3
"""Check signed helper startup with absolute and SMAppService-style argv[0].

Requires a Debug app. Does not register a service, load user state, access
Keychain, or connect to a controller.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

app = Path(sys.argv[1]).resolve()
helper = app / 'Contents/MacOS/MacControlAgentService'
with tempfile.TemporaryDirectory(prefix='maccontrol-startup-smoke-') as root:
    for argument in [str(helper), 'Contents/MacOS/MacControlAgentService']:
        result = subprocess.run(
            [argument, '--startup-signature-check'], executable=str(helper),
            cwd=root, capture_output=True, text=True, timeout=10)
        if result.returncode or 'STARTUP_SIGNATURE_VALIDATED' not in result.stdout:
            raise SystemExit('Helper startup check failed: ' + result.stderr.strip())
    if list(Path(root).iterdir()):
        raise SystemExit('Startup check unexpectedly wrote state')
    # Exercise the main run loop and real workspace/telemetry sampling. The
    # helper itself refuses success if this creates an NSApplication.
    result = subprocess.run([str(helper), '--probe'], cwd=root,
                            capture_output=True, text=True, timeout=10)
    if result.returncode or 'protocol_version' not in result.stdout:
        raise SystemExit('Background run-loop check failed: ' + result.stderr.strip())
print('Helper frontend-signature lookup passed with absolute and relative argv[0].')
print('Background run loop and system sampling passed without a UI application.')
