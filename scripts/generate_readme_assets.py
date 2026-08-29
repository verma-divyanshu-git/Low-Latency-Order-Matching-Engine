#!/usr/bin/env python3
"""Generate deterministic technical SVG assets for the README."""

from __future__ import annotations

import argparse
import pathlib
import xml.etree.ElementTree as ET

SVG = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG)


def element(parent: ET.Element, tag: str, **attributes: str) -> ET.Element:
    return ET.SubElement(parent, f"{{{SVG}}}{tag}", attributes)


def text(parent: ET.Element, x: int, y: int, value: str, css_class: str) -> None:
    node = element(parent, "text", x=str(x), y=str(y), **{"class": css_class})
    node.text = value


def base_svg(width: int, height: int, title_value: str, description: str) -> ET.Element:
    root = ET.Element(
        f"{{{SVG}}}svg",
        {"width": str(width), "height": str(height), "viewBox": f"0 0 {width} {height}"},
    )
    title = element(root, "title")
    title.text = title_value
    desc = element(root, "desc")
    desc.text = description
    style = element(root, "style")
    style.text = """
      .bg { fill: #f7f7f4; }
      .panel { fill: #ffffff; stroke: #253238; stroke-width: 2; }
      .durable { fill: #e7f2ea; stroke: #176b45; stroke-width: 2; }
      .warning { fill: #fff3d6; stroke: #9a6500; stroke-width: 2; }
      .title { font: 700 22px ui-monospace, SFMono-Regular, Menlo, monospace; fill: #172126; }
      .label { font: 700 15px ui-monospace, SFMono-Regular, Menlo, monospace; fill: #172126; }
      .small { font: 13px ui-monospace, SFMono-Regular, Menlo, monospace; fill: #425157; }
      .arrow { stroke: #253238; stroke-width: 2; fill: none; marker-end: url(#arrow); }
    """
    defs = element(root, "defs")
    marker = element(
        defs,
        "marker",
        id="arrow",
        markerWidth="8",
        markerHeight="8",
        refX="7",
        refY="4",
        orient="auto",
    )
    element(marker, "path", d="M0,0 L8,4 L0,8 Z", fill="#253238")
    element(root, "rect", x="0", y="0", width=str(width), height=str(height), **{"class": "bg"})
    return root


def architecture_svg() -> ET.Element:
    root = base_svg(
        960,
        360,
        "Matching engine architecture",
        "Gateway, durable ingress, single-writer matching, ordered publication, journal, and snapshots.",
    )
    text(root, 40, 45, "DETERMINISTIC DURABLE PIPELINE", "title")
    boxes = [
        (40, 110, 170, 90, "Gateway", "validate before sequence", "panel"),
        (260, 110, 170, 90, "Ingress", "append before publish", "durable"),
        (480, 110, 190, 90, "Matching", "single writer", "panel"),
        (720, 110, 190, 90, "Publication", "canonical events", "panel"),
    ]
    for x, y, width, height, heading, detail, css_class in boxes:
        element(root, "rect", x=str(x), y=str(y), width=str(width), height=str(height), rx="6", **{"class": css_class})
        text(root, x + 18, y + 35, heading, "label")
        text(root, x + 18, y + 62, detail, "small")
    for start, end in ((210, 260), (430, 480), (670, 720)):
        element(root, "path", d=f"M{start},155 L{end - 10},155", **{"class": "arrow"})
    element(root, "rect", x="260", y="255", width="170", height="65", rx="6", **{"class": "durable"})
    text(root, 278, 282, "Journal segments", "label")
    text(root, 278, 305, "CRC32C + fsync", "small")
    element(root, "path", d="M345,200 L345,245", **{"class": "arrow"})
    element(root, "rect", x="500", y="255", width="150", height="65", rx="6", **{"class": "durable"})
    text(root, 518, 282, "Snapshots", "label")
    text(root, 518, 305, "atomic replace", "small")
    element(root, "path", d="M575,200 L575,245", **{"class": "arrow"})
    return root


def evidence_svg() -> ET.Element:
    root = base_svg(
        960,
        300,
        "Performance evidence status",
        "Regression evidence is retained while qualified latency remains unavailable.",
    )
    text(root, 40, 45, "EVIDENCE STATUS", "title")
    rows = [
        (85, "Correctness", "debug, sanitizer, differential, replay", "durable"),
        (145, "Throughput", "75.3-77.2M ops/s · batch median · M4 Pro", "durable"),
        (205, "Latency", "not publishable: host unqualified + below resolution", "warning"),
    ]
    for y, heading, detail, css_class in rows:
        element(root, "rect", x="40", y=str(y - 28), width="880", height="48", rx="6", **{"class": css_class})
        text(root, 62, y, heading, "label")
        text(root, 245, y, detail, "small")
    text(root, 40, 275, "Darwin arm64 report: qualified=false", "small")
    return root


def write_svg(path: pathlib.Path, root: ET.Element) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.indent(root, space="  ")
    ET.ElementTree(root).write(path, encoding="unicode", xml_declaration=False)
    with path.open("a", encoding="utf-8") as destination:
        destination.write("\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=pathlib.Path, default=pathlib.Path("docs/assets"))
    arguments = parser.parse_args()
    write_svg(arguments.output_dir / "architecture.svg", architecture_svg())
    write_svg(arguments.output_dir / "evidence-status.svg", evidence_svg())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
