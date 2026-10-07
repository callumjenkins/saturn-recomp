"""Where a tester's sessions and settings live: beside the runtime's saves, in the user's data directory."""
import os
import sys


def data_dir():
    """The runtime's choice of data directory (bios.cpp), so the sessions sit next to the saves."""
    if sys.platform == "win32":
        return os.environ.get("APPDATA", "")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support")
    return os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")


def game_dir(product):
    """saturn-recomp/PRODUCT_VERSION: the runtime keeps backup.bin here, and the launcher its settings and sessions."""
    return os.path.join(data_dir(), "saturn-recomp", product)
