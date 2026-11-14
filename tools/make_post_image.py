"""Render the LinkedIn benchmark image from the measured numbers in README.md.

    python tools/make_post_image.py

Every number is parsed from README.md's Benchmarks section and shown exactly as
written there. The script exits with an error if any value is missing.
"""

import re
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import PathPatch  # noqa: E402
from matplotlib.path import Path as MplPath  # noqa: E402
from matplotlib.ticker import FixedLocator, NullLocator  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
README = ROOT / "README.md"
OUT = ROOT / "images" / "linkedin" / "codescope-benchmarks.png"

# Brand colours sampled from images/codescope-brand. The two bar fills are the
# brand amber and a mauve, adjusted to pass the dark-mode lightness and chroma checks.
BG = "#1b0218"
TEXT = "#fde9d1"
MUTED = "#c9a9bd"
GRID = "#3d1a36"
ACCENT = "#f9614e"
FILL_CODESCOPE = "#c7840a"
FILL_BASELINE = "#8f588a"

SIZE_PX = 1200
DPI = 100


def fail(message):
    sys.exit(f"make_post_image: {message}")


def pt(px):
    """Pixel size to font points at our DPI."""
    return px * 72 / DPI


# ---- README parsing -----------------------------------------------------------

def benchmarks_section(text):
    match = re.search(r"^## Benchmarks\n(.*?)(?=^## )", text, re.S | re.M)
    if not match:
        fail("README.md has no '## Benchmarks' section")
    return match.group(1)


def table_row(section, required_headers, row_key):
    """Cells of the row starting with `row_key` in the table having `required_headers`."""
    lines = section.splitlines()
    for i, line in enumerate(lines):
        if not line.startswith("|"):
            continue
        headers = [c.strip() for c in line.strip().strip("|").split("|")]
        if not all(h in headers for h in required_headers):
            continue
        for row in lines[i + 2:]:
            if not row.startswith("|"):
                break
            cells = [c.strip() for c in row.strip().strip("|").split("|")]
            if row_key in cells[0]:
                return dict(zip(headers, cells))
        fail(f"table with {required_headers} has no row for {row_key!r}")
    fail(f"no table with columns {required_headers}")


def measured(cells, column, unit):
    """(exact text, numeric value) for a cell such as '2.41 ms'."""
    text = cells.get(column, "")
    match = re.fullmatch(r"([\d,]+(?:\.\d+)?) " + re.escape(unit), text)
    if not match:
        fail(f"column {column!r} should be a number in {unit}, found {text!r}")
    return text, float(match.group(1).replace(",", ""))


def find(pattern, text, what):
    match = re.search(pattern, text)
    if not match:
        fail(f"could not find {what} in README.md")
    return match.group(1)


def load_numbers():
    readme = README.read_text(encoding="utf-8")
    bench = benchmarks_section(readme)

    main = table_row(bench, ["Repository", "Files indexed", "Query p50", "`grep -rFi` median"], "nlohmann/json")
    files = main["Files indexed"]
    if not re.fullmatch(r"[\d,]+", files):
        fail(f"'Files indexed' should be a count, found {files!r}")

    size = table_row(bench, ["Fixed 4-byte ids", "Delta + varint", "Change"], "Index on disk")
    change = size["Change"]
    if not re.fullmatch(r"[−-]\d+%", change):
        fail(f"'Change' for index size should look like −63%, found {change!r}")

    return {
        "files": files,
        "p50": measured(main, "Query p50", "ms"),
        "grep": measured(main, "`grep -rFi` median", "ms"),
        "fixed": measured(size, "Fixed 4-byte ids", "MB"),
        "varint": measured(size, "Delta + varint", "MB"),
        "change": change,
        "date": find(r"Measured on (\d{4}-\d{2}-\d{2})", bench, "the measurement date"),
        "cpu": find(r"Intel Core (i\d-\w+)", bench, "the CPU model"),
        "os": find(r"(Windows \d+)", bench, "the Windows version"),
        "grep_tool": find(r"(Git for Windows)", bench, "the grep variant"),
        "repo": find(r"github\.com/([\w.-]+/[\w.-]+)/actions", readme, "the GitHub repository"),
    }


# ---- drawing --------------------------------------------------------------------

def rounded_bar(ax, start, end, y, height_px, radius_px, color):
    """Bar square at the baseline, rounded at the value end (drawn in pixels)."""
    to_px = ax.transData.transform
    x0, yc = to_px((start, y))
    x1, _ = to_px((end, y))
    top, bottom = yc + height_px / 2, yc - height_px / 2
    r = min(radius_px, (x1 - x0) / 2, height_px / 2)
    verts = [
        (x0, bottom), (x1 - r, bottom), (x1, bottom), (x1, bottom + r),
        (x1, top - r), (x1, top), (x1 - r, top), (x0, top), (x0, bottom),
    ]
    codes = [
        MplPath.MOVETO, MplPath.LINETO, MplPath.CURVE3, MplPath.CURVE3,
        MplPath.LINETO, MplPath.CURVE3, MplPath.CURVE3, MplPath.LINETO, MplPath.CLOSEPOLY,
    ]
    ax.add_patch(PathPatch(MplPath(verts, codes), facecolor=color, edgecolor="none",
                           transform=None, clip_on=False))


def bar_panel(fig, top_px, title, bars, xscale, xlim, axis_label=None, ticks=None):
    """A titled panel of labelled horizontal bars; returns the axes."""
    left, right = 80, 80
    height = 250
    fig.text(left / SIZE_PX, 1 - top_px / SIZE_PX, title, color=TEXT, fontsize=pt(40),
             fontweight="bold", va="top", ha="left")
    ax = fig.add_axes([left / SIZE_PX, 1 - (top_px + 90 + height) / SIZE_PX,
                       1 - (left + right) / SIZE_PX, height / SIZE_PX])
    ax.set_facecolor(BG)
    ax.set_xscale(xscale)
    ax.set_xlim(*xlim)
    ax.set_ylim(-0.5, len(bars) - 0.5)
    ax.invert_yaxis()
    for spine in ax.spines.values():
        spine.set_visible(False)
    ax.yaxis.set_major_locator(NullLocator())
    ax.yaxis.set_minor_locator(NullLocator())

    if ticks:
        values, labels = ticks
        ax.xaxis.set_major_locator(FixedLocator(values))
        ax.xaxis.set_minor_locator(NullLocator())
        ax.set_xticklabels(labels)
        ax.tick_params(axis="x", colors=MUTED, labelsize=pt(26), length=0, pad=12)
        ax.grid(axis="x", color=GRID, linewidth=2)
        ax.set_axisbelow(True)
        ax.set_xlabel(axis_label, color=MUTED, fontsize=pt(26), labelpad=14)
    else:
        ax.xaxis.set_major_locator(NullLocator())
        ax.xaxis.set_minor_locator(NullLocator())

    fig.canvas.draw()  # fixes transforms before drawing pixel-space bars
    for i, bar in enumerate(bars):
        # Name sits just above its own bar; the value follows the bar's end.
        ax.text(xlim[0], i - 0.13, bar["name"], color=TEXT, fontsize=pt(30), va="bottom", ha="left")
        rounded_bar(ax, xlim[0], bar["value"], i + 0.15, height_px=50, radius_px=10, color=bar["color"])
        x_end, y_mid = ax.transData.transform((bar["value"], i + 0.15))
        label = ax.annotate(bar["label"], xy=(x_end, y_mid), xycoords="figure pixels",
                            xytext=(16, 0), textcoords="offset pixels", color=TEXT,
                            fontsize=pt(34), fontweight="bold", va="center", ha="left")
        if bar.get("note"):
            fig.canvas.draw()
            extent = label.get_window_extent()
            ax.annotate(bar["note"], xy=(extent.x1, y_mid), xycoords="figure pixels",
                        xytext=(22, 0), textcoords="offset pixels", color=ACCENT,
                        fontsize=pt(34), fontweight="bold", va="center", ha="left")
    return ax


def render(n):
    plt.rcParams["font.family"] = "sans-serif"
    plt.rcParams["font.sans-serif"] = ["Segoe UI", "Inter", "Helvetica", "Arial", "DejaVu Sans"]
    fig = plt.figure(figsize=(SIZE_PX / DPI, SIZE_PX / DPI), dpi=DPI, facecolor=BG)

    fig.text(80 / SIZE_PX, 1 - 90 / SIZE_PX, "CodeScope: code search in C++20", color=TEXT,
             fontsize=pt(60), fontweight="bold", va="top", ha="left")
    fig.add_artist(plt.Line2D([80 / SIZE_PX, 160 / SIZE_PX], [1 - 192 / SIZE_PX] * 2,
                              color=ACCENT, linewidth=pt(8), solid_capstyle="round"))

    p50_text, p50 = n["p50"]
    grep_text, grep = n["grep"]
    bar_panel(
        fig, 270, f"Median search time on nlohmann/json ({n['files']} files)",
        [
            {"name": "CodeScope (p50)", "value": p50, "label": p50_text, "color": FILL_CODESCOPE},
            {"name": "grep -rFi (median)", "value": grep, "label": grep_text, "color": FILL_BASELINE},
        ],
        xscale="log", xlim=(1, 3000), axis_label="ms (log scale)",
        ticks=([1, 10, 100, 1000], ["1", "10", "100", "1,000"]),
    )

    fixed_text, fixed = n["fixed"]
    varint_text, varint = n["varint"]
    bar_panel(
        fig, 750, "Index size on disk",
        [
            {"name": "Fixed 4-byte IDs", "value": fixed, "label": fixed_text, "color": FILL_BASELINE},
            {"name": "Delta + varint", "value": varint, "label": varint_text, "color": FILL_CODESCOPE,
             "note": n["change"]},
        ],
        xscale="linear", xlim=(0, fixed * 1.45),
    )

    footer = (f"Measured {n['date']} · Intel {n['cpu']}, {n['os']}, {n['grep_tool']} grep · "
              f"github.com/{n['repo']}")
    fig.text(80 / SIZE_PX, 50 / SIZE_PX, footer, color=MUTED, fontsize=pt(20), va="bottom", ha="left")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT, dpi=DPI, facecolor=BG, metadata={"Software": None})
    plt.close(fig)


def main():
    numbers = load_numbers()
    render(numbers)
    print(f"wrote {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
