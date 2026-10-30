"""Bar-chart styling shared by the paper figures: colors, speedup labels, geomeans."""

from __future__ import annotations

import math

STATELESS_COLOR = "#4C72B0"
GENDB_COLOR = "#D4A020"
BESPOKE_COLOR = "#55A868"
STATEFUL_COLOR = "#2A9D8F"
VERIFIED_COLOR = "#3B1F6B"
PG_COLOR = "#C44E64"


def geomean(values: list[float | None]) -> float:
    present = [v for v in values if v is not None]
    return math.exp(sum(math.log(v) for v in present) / len(present))


def fmt(value: float) -> str:
    if value >= 10000:
        return f"{value / 1000:.0f}k"
    if value >= 1000:
        return f"{value / 1000:.1f}k"
    if value >= 100:
        return f"{value:.0f}"
    return f"{value:.1f}"


LABEL_SIZE = 19
PG_LABEL_SIZE = 26
TICK_SIZE = 28
AXIS_LABEL_SIZE = 31


def label_bars(ax, bars, values, size: float = LABEL_SIZE) -> list:
    return [ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() * 1.15, fmt(value),
                    ha="center", va="bottom", rotation=90, fontsize=size, color="#333333")
            if value is not None else None
            for bar, value in zip(bars, values)]


def place_labels(fig, ax, groups: list[list[tuple]]) -> int:
    """Keep each label on its own bar. Where a label above its bar would collide
    with its left neighbour's, move one of the two inside the top of its bar
    (white text) when that bar is tall enough. Returns collisions left over."""
    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()

    def extent(text):
        return text.get_window_extent(renderer)

    def bar_px(bar):
        return ax.transData.transform((0, bar.get_height()))[1] - ax.transData.transform((0, ax.get_ylim()[0]))[1]

    def move_inside(text, bar) -> bool:
        if bar_px(bar) < extent(text).height + 8:
            return False
        text.set_y(bar.get_height() / 1.12)
        text.set_va("top")
        text.set_color("white")
        return True

    def collide(a, b) -> bool:
        # rotated labels: the right edge of each box is the font's descent, empty for digits, so it may overlap
        ea, eb = extent(a), extent(b)
        slack = 0.15 * a.get_fontsize() * fig.dpi / 72
        return ea.x1 > eb.x0 + slack and eb.x1 > ea.x0 + slack and ea.y1 > eb.y0 and eb.y1 > ea.y0

    left = 0
    for group in groups:
        group = [(t, b) for t, b in group if t is not None]
        for (prev, prev_bar), (cur, cur_bar) in zip(group, group[1:]):
            if not collide(prev, cur):
                continue
            # Prefer moving the taller bar's label inside; it has the room.
            order = [(cur, cur_bar), (prev, prev_bar)]
            if prev_bar.get_height() > cur_bar.get_height():
                order.reverse()
            if not any(move_inside(t, b) for t, b in order):
                left += 1
    return left


def style_axis(ax, n: int) -> None:
    ax.set_yscale("log")
    ax.axhline(1.0, color="#888888", linestyle="--", linewidth=0.8, zorder=1)
    ax.grid(axis="y", linestyle=":", alpha=0.5, zorder=0)
    ax.set_facecolor("#FAFAFA")
    ax.set_xlim(-0.55, n - 0.45)
    ax.tick_params(axis="y", labelsize=TICK_SIZE)
    ax.tick_params(axis="x", labelsize=TICK_SIZE)


