Import("env")

import os
import subprocess
from pathlib import Path


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))


def detect_version() -> str:
    override = os.environ.get("RSVP_FIRMWARE_VERSION", "").strip()
    if override:
        return override

    try:
        value = subprocess.check_output(
            ["git", "describe", "--tags", "--exact-match"],
            cwd=PROJECT_DIR,
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
        if value:
            return value
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass

    try:
        short_sha = subprocess.check_output(
            ["git", "rev-parse", "--short=7", "HEAD"],
            cwd=PROJECT_DIR,
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
        dirty = subprocess.call(
            ["git", "diff", "--quiet"],
            cwd=PROJECT_DIR,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ) != 0
        suffix = "-dirty" if dirty else ""
        return f"dev-{short_sha}{suffix}" if short_sha else "dev"
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "dev"


version = detect_version().replace('"', "")
env.Append(CPPDEFINES=[("RSVP_FIRMWARE_VERSION", '\\"%s\\"' % version)])
