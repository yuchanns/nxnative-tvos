"""Apply game-specific presentation without duplicating controller behavior."""

import html
import json
from pathlib import Path
import re
import shutil


RESOURCES = Path(__file__).resolve().parents[1] / "Resources"
COLORS = {
    "art_background": "#123c3c",
    "background": "#0a2121",
    "shade": "#051212",
    "accent": "#8cffd6",
    "shadow": "#33ffb3",
}
CONTROLLER = {
    "title": "Hyrule Pad",
    "brand": "HYRULE",
    "subtitle": "WIRELESS CONTROLLER",
    "motto": "ADVENTURE AWAITS",
    "eyebrow": "YOUR COMPANION IN HYRULE",
    "welcome": "Ready for an adventure?",
    "rotate": "Turn to adventure",
}
OVERRIDES = {"controller-icon.png", "controller-skin.jpg", "controller-theme.css"}


def load_theme(path=None):
    config = json.loads(path.read_text()) if path else {}
    if not isinstance(config, dict) or set(config) - {"colors", "controller", "resources"}:
        raise ValueError("Unexpected theme settings")
    if any(not isinstance(config.get(key, {}), dict) for key in ("colors", "controller", "resources")):
        raise ValueError("Theme sections must be objects")
    colors = COLORS | config.get("colors", {})
    controller = CONTROLLER | config.get("controller", {})
    if set(colors) != set(COLORS) or any(not isinstance(value, str) or
            not re.fullmatch(r"#[0-9a-fA-F]{6}", value) for value in colors.values()):
        raise ValueError("Theme colors must be six-digit RGB values")
    if set(controller) != set(CONTROLLER) or any(not isinstance(value, str) or not value.strip()
                                               for value in controller.values()):
        raise ValueError("Controller labels must be nonempty strings")
    overrides = {}
    for name, relative in config.get("resources", {}).items():
        if name not in OVERRIDES or not isinstance(relative, str) or not path:
            raise ValueError("Unexpected controller resource")
        source = (path.parent / relative).resolve()
        if not source.is_relative_to(path.parent.resolve()) or not source.is_file():
            raise ValueError("Controller resources must be files inside the theme directory")
        overrides[name] = source
    return {"colors": colors, "controller": controller, "resources": overrides}


def generate_resources(directory, theme):
    shutil.copytree(RESOURCES, directory)
    for name, source in theme["resources"].items():
        shutil.copyfile(source, directory / name)
    page = directory / "gamepad.html"
    content = page.read_text()
    for key, value in theme["controller"].items():
        content = content.replace("{{" + key.upper() + "}}", html.escape(value, quote=True))
    content = content.replace("{{BACKGROUND}}", theme["colors"]["background"])
    if re.search(r"\{\{[A-Z_]+\}\}", content):
        raise ValueError("Unresolved controller template value")
    page.write_text(content)
    manifest = directory / "controller.webmanifest"
    metadata = json.loads(manifest.read_text())
    metadata.update(name=theme["controller"]["title"], short_name=theme["controller"]["title"],
                    background_color=theme["colors"]["background"],
                    theme_color=theme["colors"]["accent"])
    manifest.write_text(json.dumps(metadata, indent=2) + "\n")
