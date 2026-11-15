"""Runtime credential reads; never included in status, configuration or logs."""

import stat
from pathlib import Path


def read_key_file(filename):
    path = Path(filename)
    if stat.S_IMODE(path.stat().st_mode) != 0o600:
        raise ValueError("Key file must have mode 0600: " + str(path))
    key = path.read_text().strip()
    if not key:
        raise ValueError("Key file is empty: " + str(path))
    return key
