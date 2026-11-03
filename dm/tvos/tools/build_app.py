#!/usr/bin/env python3
"""Build an unsigned tvOS app outside the repository; optionally pack its IPA."""

import argparse
import json
import os
import plistlib
import pwd
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

DM = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--profiles", type=Path, required=True)
    parser.add_argument("--patched", type=Path, required=True)
    parser.add_argument("--romfs", type=Path, required=True)
    parser.add_argument("--shaders", type=Path, required=True)
    parser.add_argument("--astc-cache", type=Path)
    parser.add_argument("--asset-manifest", type=Path)
    parser.add_argument("--shader-manifest", type=Path)
    parser.add_argument("--astc-manifest", type=Path)
    parser.add_argument("--gofile-resources", type=Path)
    parser.add_argument("--env-file", type=Path)
    parser.add_argument("--icon", type=Path)
    parser.add_argument("--cover", type=Path)
    parser.add_argument("--theme", type=Path, help="Per-game presentation settings and controller resources")
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    if bool(args.icon) != bool(args.cover):
        parser.error("Game artwork requires both --icon and --cover")
    if args.theme and not args.icon:
        parser.error("A game theme requires --icon and --cover")
    if args.gofile_resources and not args.asset_manifest:
        parser.error("Gofile downloads require an asset manifest")
    for name in ("work", "profiles", "patched", "romfs", "shaders", "astc_cache", "asset_manifest", "shader_manifest", "astc_manifest", "gofile_resources", "env_file", "output"):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve())
    build = args.work / "tvos/app"
    build.mkdir(parents=True, exist_ok=True)
    report_path = build / "build-result.json"
    previous = json.loads(report_path.read_text()) if report_path.exists() else {}
    build_number = int(previous.get("build_number", 1)) + 1
    gen = build / ("Gen-" + str(time.time_ns()))
    (gen / "dm").mkdir(parents=True)
    from brand_resources import generate_resources, load_theme
    theme = load_theme(args.theme.resolve() if args.theme else None)
    generate_resources(gen / "Resources", theme)
    if args.icon:
        from brand_assets import generate
        generate(gen / "Assets.xcassets", args.icon.resolve(), args.cover.resolve(), theme["colors"])
    else:
        (gen / "Assets.xcassets").mkdir()
    env = dict(os.environ, GAME=args.game, DM_GAME=args.game, DM_GAMES_FIRST="1", DM_GAMES_DIR=str(args.profiles))
    env.setdefault("USER", pwd.getpwuid(os.getuid()).pw_name)
    env.setdefault("LOGNAME", env["USER"])

    def profile_value(key):
        return subprocess.check_output([sys.executable, str(DM / "games/game.py"), args.game, key], env=env, text=True).strip()

    profile = Path(subprocess.check_output([sys.executable, str(DM / "games/game.py"), "dir", args.game], env=env, text=True).strip())
    chain = re.search(r'#define DMSH_CHAIN "([a-f0-9]+)"', (profile / "dmsh_chain.h").read_text()).group(1)
    shader_dir = args.shaders / ("b-" + chain)
    proof = json.loads((shader_dir / "tvos-build.json").read_text())
    if proof["failures"] or proof["shaders"] != len(list((args.work / "shader/cache" / ("b-" + chain)).glob("*_b.metal"))):
        raise RuntimeError("The complete tvOS shader library has not passed validation")
    if not (args.romfs / profile_value("romfs_marker")).exists():
        raise RuntimeError("Missing RomFS marker")
    modules = profile_value("modules").split()
    env.update(NX_DM_SOURCE=str(DM), NX_TV_GEN=str(gen), NX_TV_BUILD=str(build),
               DM_BUNDLE_ID="org.nxnative.tv." + args.game.lower(), DM_DISPLAY_NAME=profile_value("name"),
               DM_APP_VERSION=args.version, DM_APP_BUILD=str(build_number),
               DM_APP_CATEGORY=profile_value("app_category"))
    env["DM_APP_ICON"] = "Game Brand" if args.icon else ""
    config = subprocess.check_output([sys.executable, str(DM / "ios/tools/gen_build.py"), str(DM / "host"), str(gen)], env=env, text=True)
    link = subprocess.check_output([sys.executable, str(DM / "ios/tools/gen_embed.py"), str(args.patched), str(gen / "embed.S"), *modules], env=env, text=True).strip()
    (gen / "dm.xcconfig").write_text(config + f"OTHER_LDFLAGS = $(inherited) $(DM_FRAMEWORKS) {link}\n")
    for pattern in ("*.fixups.tsv", "*.gates.tsv", "*.cuts.tsv", "*.obs.tsv"):
        for path in args.patched.glob(pattern):
            shutil.copy2(path, gen / "dm" / path.name)
    for name in ("shader_map.tsv", "pso_manifest.tsv", "c1_full.tsv", "cb_extent.tsv"):
        shutil.copy2(profile / name, gen / "dm" / name)
    if args.env_file:
        private_keys = ("DM_CLOUD_SAVE_TOKEN=", "DM_TV_ASSET_PASSWORD=", "DM_TV_ASSET_TOKEN=")
        if any(line.strip().startswith(private_keys) for line in args.env_file.read_text().splitlines()):
            raise RuntimeError("Provision private cloud/download credentials on the device; do not embed them in an IPA")
        shutil.copy2(args.env_file, gen / "dm/env.txt")
    if args.asset_manifest:
        if not args.shader_manifest or (args.astc_cache and not args.astc_manifest):
            parser.error("Downloadable assets require shader and ASTC manifests")
        groups = []
        for path, root, source in ((args.shader_manifest, "msl", "shaders"),
                                   (args.astc_manifest, "astc", "astc"),
                                   (args.asset_manifest, "romfs", "assets")):
            if path:
                group = json.loads(path.read_text())
                groups.append(dict(group, root=root, source=source))
        if args.gofile_resources:
            from asset_downloads import add_gofile_downloads
            add_gofile_downloads(groups, args.gofile_resources)
        (gen / "dm/assets-manifest.json").write_text(json.dumps({"version": 2, "groups": groups}) + "\n")
    font = gen / "dm/font"
    font.mkdir()
    shutil.copyfile("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", font / "standard.ttf")
    subprocess.run([sys.executable, str(DM / "tools/make_ext_font.py"), str(font / "nintendo_ext.ttf")], env=env, check=True)
    subprocess.run(["xcodegen", "generate", "--spec", str(DM / "tvos/project.yml"), "--project", str(build), "--project-root", str(build)], env=env, check=True)
    derived = build / "DerivedData"
    with (build / "build.log").open("w") as log:
        result = subprocess.run(["xcodebuild", "-project", str(build / "DmTV.xcodeproj"), "-target", "DmTV",
            "-configuration", "Release", "-sdk", "appletvos", "-arch", "arm64", "-jobs", str(args.jobs),
            "CONFIGURATION_BUILD_DIR=" + str(derived / "Build/Products/Release-appletvos"),
            "CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO", "CODE_SIGN_IDENTITY=", "build"], env=env, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        for line in (build / "build.log").read_text(errors="replace").splitlines():
            if "error:" in line:
                print(line)
        raise SystemExit(result.returncode)
    app = derived / "Build/Products/Release-appletvos/DmTV.app"
    if not app.is_dir():
        raise RuntimeError("Xcode did not produce DmTV.app")
    # Xcode may convert PNG resources to Apple's CgBI format. The HTTP-served
    # controller icon must remain a standard PNG that browsers can decode.
    shutil.copytree(gen / "Resources", app, dirs_exist_ok=True)
    if args.icon:
        # Keep sideloaders from decoding layered tvOS assets just for a preview.
        preview = gen / ("Assets.xcassets/Game Brand.brandassets/App Icon.imagestack/"
                         "Artwork.imagestacklayer/Content.imageset/image@1x.png")
        shutil.copyfile(preview, app / "SideloadlyIcon.png")
        info_path = app / "Info.plist"
        info = plistlib.loads(info_path.read_bytes())
        info["CFBundleIconFile"] = "SideloadlyIcon.png"
        info["CFBundleIconFiles"] = ["SideloadlyIcon"]
        info_path.write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY, sort_keys=False))
    report = {"app": str(app), "chain": chain, "generation": str(gen), "unsigned": True,
              "version": args.version, "build_number": build_number}
    if args.output:
        prefix = "Payload/DmTV.app"
        maps = [f"{prefix}={app}"]
        if not args.asset_manifest:
            maps.append(f"{prefix}/msl/b-{chain}={shader_dir}")
            maps.append(f"{prefix}/romfs={args.romfs}")
        if args.astc_cache and not args.asset_manifest:
            maps.append(f"{prefix}/astc={args.astc_cache}")
        subprocess.run([sys.executable, str(DM / "ios/tools/pack_ipa.py"), str(args.output), "--store", profile_value("zip_store"),
            "--skip", prefix + "/_CodeSignature", "--skip", prefix + "/embedded.mobileprovision", *maps], env=env, check=True)
        report["ipa"] = str(args.output)
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
