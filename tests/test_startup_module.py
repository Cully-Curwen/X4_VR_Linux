from pathlib import Path
import os
import subprocess
import sys
import tempfile

watch, child, module, native = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="x4vr-startup-") as temp:
    for label, dll, fail, expected in [("success", module, False, 0),
                                       ("reject", module, True, 5),
                                       ("missing_export", child, False, 5),
                                       ("unsupported_game", native, False, 5)]:
        output = Path(temp)/label
        env = os.environ.copy()
        env.pop("X4VR_STARTUP_TEST_READY", None)
        env.pop("X4VR_STARTUP_TEST_FAIL", None)
        if fail:
            env["X4VR_STARTUP_TEST_FAIL"] = "1"
        result = subprocess.run([watch, child, str(output), "--startup-module", dll],
                                env=env, timeout=40, capture_output=True, text=True)
        events = (output/"debug-events.log").read_text()
        assert result.returncode == expected, (result.returncode, events, result.stderr)
        if expected == 0:
            assert events.index("primary_held_at_entry") < events.index("exported initialization outside DllMain")
            assert events.index("primary_resumed") < events.index("executable entry reached")
            assert "exit code=0" in events
        else:
            assert "startup_module_failed" in events
            assert "executable entry reached" not in events
            if label == "unsupported_game":
                assert "unsupported executable; no hook installed" in events
print("Startup-only module: export initialization precedes executable entry; rejection aborts owned child")
