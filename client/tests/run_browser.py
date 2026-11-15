"""Run the browser check with local public runtime libraries on Nix hosts."""

import os
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
env = os.environ.copy()
env["PLAYWRIGHT_BROWSERS_PATH"] = str(root / ".cache/playwright")
temporary = root / ".cache/browser-temp"
temporary.mkdir(parents=True, exist_ok=True)
env["TMPDIR"] = str(temporary)
paths = root / ".cache/browser-libraries.txt"
if paths.exists():
    libraries = []
    for line in paths.read_text().splitlines():
        if (Path(line) / "lib").is_dir():
            libraries.append(str(Path(line) / "lib"))
    env["LD_LIBRARY_PATH"] = ":".join(libraries + [env.get("LD_LIBRARY_PATH", "")])
os.execvpe(
    "node",
    [
        "node",
        str(root / "client/tests" / env.get("AMB_BROWSER_SCRIPT", "browser-smoke.mjs")),
        *sys.argv[1:],
    ],
    env,
)
