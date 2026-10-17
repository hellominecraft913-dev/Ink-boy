"""Fetch the upstream Agnes NES core on X4 Pro builds if it is not present.

Agnes is intentionally fetched only for x4-pro and x4-pro-debug. The upstream
repository is MIT-licensed and consists of agnes.c and agnes.h.
"""
from pathlib import Path
import urllib.error
import urllib.request

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
env_name = env.subst("$PIOENV")
if env_name not in ("x4-pro", "x4-pro-debug"):
    Return()

vendor_dir = project_dir / "src" / "activities" / "nes" / "vendor"
vendor_dir.mkdir(parents=True, exist_ok=True)
base_url = "https://raw.githubusercontent.com/kgabis/agnes/master/"
files = ("agnes.c", "agnes.h", "LICENSE")

for filename in files:
    target = vendor_dir / filename
    if target.exists() and target.stat().st_size > 100:
        continue
    url = base_url + filename
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "Ink-boy-Agnes-Integration/1.0"})
        with urllib.request.urlopen(request, timeout=30) as response:
            content = response.read()
        if len(content) < 100:
            raise RuntimeError("downloaded file was unexpectedly small")
        temporary = target.with_suffix(target.suffix + ".download")
        temporary.write_bytes(content)
        temporary.replace(target)
        print("Fetched upstream Agnes source: {} ({} bytes)".format(filename, len(content)))
    except (OSError, urllib.error.URLError, RuntimeError) as exc:
        raise RuntimeError(
            "Unable to fetch Agnes {} from {}. Check the build machine's internet access, "
            "then retry `pio run -e {}`.".format(filename, url, env_name)
        ) from exc
