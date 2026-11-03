"""Compile caller-supplied game artwork into a tvOS asset catalog."""

import json
from pathlib import Path

from PIL import Image, ImageOps


INFO = {"author": "xcode", "version": 1}


def metadata(directory, **values):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "Contents.json").write_text(json.dumps(dict(info=INFO, **values), indent=2) + "\n")


def image_set(directory, source, size, *, fit=False, scales=(1, 2), background="#123c3c"):
    images = []
    directory.mkdir(parents=True, exist_ok=True)
    for scale in scales:
        dimensions = tuple(value * scale for value in size)
        if fit:
            picture = Image.new("RGB", dimensions, background)
            content = ImageOps.contain(source, dimensions, Image.Resampling.LANCZOS)
            picture.paste(content, ((dimensions[0] - content.width) // 2, (dimensions[1] - content.height) // 2))
        else:
            picture = ImageOps.fit(source, dimensions, Image.Resampling.LANCZOS)
        name = f"image@{scale}x.png"
        picture.save(directory / name)
        images.append({"idiom": "tv", "filename": name, "scale": f"{scale}x"})
    metadata(directory, images=images)


def generate(directory: Path, icon: Path, cover: Path, colors=None):
    """Keep copyrighted artwork outside source control and in the build output."""
    icon_image = Image.open(icon).convert("RGB")
    cover_image = Image.open(cover).convert("RGB")
    from brand_resources import COLORS
    colors = colors or COLORS
    background = colors["art_background"]
    metadata(directory)
    for name, key in (("LoadingBackground", "background"), ("LoadingShade", "shade"),
                      ("LoadingAccent", "accent"), ("LoadingShadow", "shadow")):
        value = colors[key].lstrip("#")
        components = {component: f"{int(value[index:index + 2], 16) / 255:.6f}"
                      for component, index in (("red", 0), ("green", 2), ("blue", 4))}
        components["alpha"] = "1.000000"
        metadata(directory / (name + ".colorset"), colors=[{
            "idiom": "universal", "color": {"color-space": "srgb", "components": components}}])
    brand = directory / "Game Brand.brandassets"
    assets = []
    for name, size, scales in (("App Icon", (400, 240), (1, 2)),
                               ("App Store Icon", (1280, 768), (1,))):
        stack = brand / f"{name}.imagestack"
        metadata(stack, layers=[{"filename": "Artwork.imagestacklayer"}, {"filename": "Background.imagestacklayer"}])
        for layer_name, source in (("Artwork", icon_image), ("Background", Image.new("RGB", size, background))):
            layer = stack / f"{layer_name}.imagestacklayer"
            metadata(layer)
            image_set(layer / "Content.imageset", source, size, scales=scales, background=background)
        assets.append({"idiom": "tv", "filename": stack.name, "role": "primary-app-icon", "size": f"{size[0]}x{size[1]}"})
    for name, width, role in (("Top Shelf", 1920, "top-shelf-image"), ("Top Shelf Wide", 2320, "top-shelf-image-wide")):
        image_set(brand / f"{name}.imageset", cover_image, (width, 720), background=background)
        assets.append({"idiom": "tv", "filename": f"{name}.imageset", "role": role, "size": f"{width}x720"})
    metadata(brand, assets=assets)
    image_set(directory / "GameCover.imageset", cover_image, (1920, 1080), scales=(1,))
