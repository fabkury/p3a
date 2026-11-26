"""Makes github_releases_2x_made.json from github_releases_1x.json.

github_releases_1x.json is GitHub's own answer to
    GET https://api.github.com/repos/fabkury/p3a/releases?per_page=3
recorded on 2026-11-25, when only 1.x had shipped. A 2.x device must find
nothing to install in it.

The made file is that answer as it will look once 2.x ships beside 1.x in
the same repository: every field GitHub sends is kept, some releases get a
2.x tag and their `p3a.bin` becomes `p3a-v2.bin` (size and digest as
recorded). tests/host/net/test_update_release.c names what it expects of
both files.

Run from the repository root:  python tests/assets/update/make_releases.py
"""
import copy
import json
import pathlib

HERE = pathlib.Path(__file__).parent
KEEP = ("README.md", "p3a-flasher.exe", "bootloader.bin", "partition-table.bin", "ota_data_initial.bin",
        "network_adapter.bin")


def as_2x(release, tag, prerelease=False, body=None):
    r = copy.deepcopy(release)
    old = r["tag_name"]
    r["tag_name"] = r["name"] = tag
    r["prerelease"] = prerelease
    if body is not None:
        r["body"] = body
    assets = []
    for a in r["assets"]:
        if a["name"] == "p3a.bin":
            a["name"] = "p3a-v2.bin"
            a["browser_download_url"] = f"https://github.com/fabkury/p3a/releases/download/{tag}/p3a-v2.bin"
            assets.append(a)
        elif a["name"] in KEEP:
            a["browser_download_url"] = a["browser_download_url"].replace(old, tag)
            assets.append(a)
    r["assets"] = assets
    r["html_url"] = f"https://github.com/fabkury/p3a/releases/tag/{tag}"
    return r


def main():
    recorded = json.loads((HERE / "github_releases_1x.json").read_text(encoding="utf-8"))
    made = [
        as_2x(recorded[0], "v2.1.0-rc.1", True, "Release candidate.\n\n- Try the new thing"),
        recorded[0],  # a 1.x release published after 2.0: nothing in it for a 2.x device
        as_2x(recorded[1], "v2.0.1", False, "## What's new\n\n- Fixes — café ☕\n- More"),
        as_2x(recorded[2], "v2.0.0", False),
        recorded[1],
    ]
    (HERE / "github_releases_2x_made.json").write_text(json.dumps(made, ensure_ascii=False, indent=1),
                                                         encoding="utf-8", newline="\n")
    for r in made:
        for a in r["assets"]:
            if a["name"] == "p3a-v2.bin":
                print(r["tag_name"], a["size"], a["digest"])


if __name__ == "__main__":
    main()
