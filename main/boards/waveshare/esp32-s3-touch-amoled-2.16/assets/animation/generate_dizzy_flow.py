#!/usr/bin/env python3
"""Generate a crisp dizzy animation using motion measured from a reference video.

The character pixels come from neutral.png. The video supplies dense motion and
the independently moving gold halo; it is never used as the character texture.
"""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

import cv2
import numpy as np


HERE = Path(__file__).resolve().parent
EMOJI_DIR = HERE.parent / "emoji"
NEUTRAL_PATH = EMOJI_DIR / "neutral.png"
EFFECTS_PATH = HERE / "dizzy-effects-reference.png"
FRAME_DIR = HERE / "dizzy_frames"
WIDTH = 108
NEUTRAL_HEIGHT = 108
TOP_PADDING = 12
VISIBLE_OVERSCAN = 8
SOURCE_OVERSCAN = 16
HEIGHT = TOP_PADDING + NEUTRAL_HEIGHT + VISIBLE_OVERSCAN
SOURCE_HEIGHT = TOP_PADDING + NEUTRAL_HEIGHT + SOURCE_OVERSCAN
FPS = 10
FRAME_COUNT = 20
START_MS = 1300


def read_video_frames(video: Path) -> list[np.ndarray]:
    cap = cv2.VideoCapture(str(video))
    frames: list[np.ndarray] = []
    for index in range(FRAME_COUNT):
        cap.set(cv2.CAP_PROP_POS_MSEC, START_MS + index * 1000 / FPS)
        ok, frame = cap.read()
        if not ok:
            raise RuntimeError(f"cannot read frame {index} from {video}")
        side = min(frame.shape[:2])
        square = frame[:side, :side]
        frames.append(cv2.resize(square, (WIDTH * 2, WIDTH * 2), interpolation=cv2.INTER_AREA))
    return frames


def align_for_flow(frame: np.ndarray) -> np.ndarray:
    # Align the video's hairline and eyes to neutral.png before measuring local motion.
    center = (WIDTH, WIDTH)
    matrix = cv2.getRotationMatrix2D(center, 0, 1.06)
    matrix[1, 2] -= 10
    return cv2.warpAffine(frame, matrix, (WIDTH * 2, WIDTH * 2), borderValue=(255, 255, 255))


def remove_halo_for_flow(frame: np.ndarray) -> np.ndarray:
    hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
    gold = cv2.inRange(hsv, (5, 80, 70), (42, 255, 255))
    gold[72:, :] = 0
    halo = cv2.dilate(gold, np.ones((13, 13), np.uint8))
    cleaned = frame.copy()
    cleaned[halo != 0] = 255
    return cleaned


def dense_inverse_flow(reference: np.ndarray, target: np.ndarray) -> np.ndarray:
    ref_gray = cv2.cvtColor(remove_halo_for_flow(reference), cv2.COLOR_BGR2GRAY)
    target_gray = cv2.cvtColor(remove_halo_for_flow(target), cv2.COLOR_BGR2GRAY)
    # target -> reference creates the inverse map needed by cv2.remap.
    flow = cv2.calcOpticalFlowFarneback(
        target_gray, ref_gray, None, 0.5, 5, 25, 4, 7, 1.5, 0
    )
    flow = cv2.GaussianBlur(flow, (0, 0), 1.25)
    flow = np.clip(flow, -16, 16)
    flow = cv2.resize(flow, (WIDTH, NEUTRAL_HEIGHT), interpolation=cv2.INTER_AREA) * 0.5
    return flow


def make_dizzy_base() -> np.ndarray:
    neutral = cv2.imread(str(NEUTRAL_PATH), cv2.IMREAD_UNCHANGED)
    effects = cv2.imread(str(EFFECTS_PATH), cv2.IMREAD_UNCHANGED)[:, :WIDTH]
    if neutral.shape != (NEUTRAL_HEIGHT, WIDTH, 4):
        raise RuntimeError(f"unexpected neutral size: {neutral.shape}")

    base = np.zeros((SOURCE_HEIGHT, WIDTH, 4), np.uint8)
    base[TOP_PADDING:TOP_PADDING + NEUTRAL_HEIGHT] = neutral
    # Reflect real source rows below the crop. These pixels remain off-screen and
    # prevent a transparent wedge when the complete body follows the video sway.
    for row in range(SOURCE_OVERSCAN):
        base[TOP_PADDING + NEUTRAL_HEIGHT + row] = neutral[NEUTRAL_HEIGHT - 2 - row]

    # Copy only the two eye areas from the existing crisp dizzy pixel artwork.
    yy, xx = np.mgrid[0:NEUTRAL_HEIGHT, 0:WIDTH]
    eye_mask = (((xx - 49) / 7.0) ** 2 + ((yy - 50) / 7.0) ** 2 <= 1.0)
    eye_mask |= (((xx - 67) / 7.0) ** 2 + ((yy - 51) / 7.0) ** 2 <= 1.0)
    body = base[TOP_PADDING:TOP_PADDING + NEUTRAL_HEIGHT]
    body[eye_mask] = effects[eye_mask]
    return base


def warp_body(base: np.ndarray, flow: np.ndarray) -> np.ndarray:
    grid_x, grid_y = np.meshgrid(np.arange(WIDTH, dtype=np.float32),
                                 np.arange(HEIGHT, dtype=np.float32))
    flow_y = np.clip(grid_y - TOP_PADDING, 0, NEUTRAL_HEIGHT - 1).astype(np.int32)
    dx = flow[flow_y, grid_x.astype(np.int32), 0]
    dy = flow[flow_y, grid_x.astype(np.int32), 1]
    map_x = grid_x + dx
    map_y = grid_y + dy
    return cv2.remap(base, map_x, map_y, cv2.INTER_NEAREST,
                     borderMode=cv2.BORDER_CONSTANT, borderValue=(0, 0, 0, 0))


def halo_layer(video_frame: np.ndarray) -> np.ndarray:
    small = cv2.resize(video_frame, (WIDTH, WIDTH), interpolation=cv2.INTER_AREA)
    hsv = cv2.cvtColor(small, cv2.COLOR_BGR2HSV)
    core = cv2.inRange(hsv, (5, 80, 75), (42, 255, 255))
    core[31:, :] = 0
    near = cv2.dilate(core, np.ones((5, 5), np.uint8)) != 0
    b, g, r = cv2.split(small)
    warm = (r > 75) & (r.astype(np.float32) > b * 1.18) & (g.astype(np.float32) > b * 0.68)
    dark_outline = (r < 90) & (g < 82) & (b < 75)
    highlight = (r > 205) & (g > 170) & (b < 190)
    mask = near & (warm | dark_outline | highlight)
    mask[31:, :] = False
    layer = np.zeros((HEIGHT, WIDTH, 4), np.uint8)
    layer[:WIDTH, :, :3][mask] = small[mask]
    layer[:WIDTH, :, 3][mask] = 255
    return layer


def composite_over(bottom: np.ndarray, top: np.ndarray) -> np.ndarray:
    result = bottom.copy()
    mask = top[:, :, 3] != 0
    result[mask] = top[mask]
    return result


def run_ffmpeg(*args: str) -> None:
    subprocess.run(["ffmpeg", "-v", "error", "-y", *args], check=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("video", type=Path)
    args = parser.parse_args()

    video_frames = read_video_frames(args.video)
    aligned = [align_for_flow(frame) for frame in video_frames]
    base = make_dizzy_base()
    frames = []
    for video_frame, target in zip(video_frames, aligned):
        flow = dense_inverse_flow(aligned[0], target)
        body = warp_body(base, flow)
        frames.append(composite_over(body, halo_layer(video_frame)))

    FRAME_DIR.mkdir(parents=True, exist_ok=True)
    for old in FRAME_DIR.glob("dizzy_*.png"):
        old.unlink()
    for index, frame in enumerate(frames):
        cv2.imwrite(str(FRAME_DIR / f"dizzy_{index}.png"), frame)

    palette = ("split[a][b];[a]palettegen=reserve_transparent=1[p];"
               "[b][p]paletteuse=dither=none:alpha_threshold=128")
    gif = EMOJI_DIR / "dizzy.gif"
    run_ffmpeg("-framerate", str(FPS), "-i", str(FRAME_DIR / "dizzy_%d.png"),
               "-filter_complex", palette, "-gifflags", "0", "-loop", "0", str(gif))
    run_ffmpeg("-i", str(gif), "-vf", "select=not(mod(n\\,3)),tile=4x2",
               "-frames:v", "1", str(HERE / "dizzy-preview.png"))
    run_ffmpeg("-i", str(gif), "-filter_complex",
               f"scale={WIDTH * 4}:{HEIGHT * 4}:flags=neighbor,{palette}",
               "-gifflags", "0", "-loop", "0", str(HERE / "dizzy-preview.gif"))
    print(f"Generated {FRAME_COUNT} neutral-texture frames at {FPS} fps ({WIDTH}x{HEIGHT}).")


if __name__ == "__main__":
    main()
