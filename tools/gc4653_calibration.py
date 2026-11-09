#!/usr/bin/env python3
"""Host-side calibration helper for the LicheeRV Nano GC4653 camera.

The board keeps running the existing camera program and exposes /snapshot.jpg.
This tool runs on the host PC, detects chessboard corners with OpenCV, computes
intrinsics/distortion, and can optionally write fx/fy/cx/cy back to both board
configuration files.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np


@dataclass
class Sample:
    path: Path
    corners: np.ndarray
    image_size: tuple[int, int]


def detect_corners(image: np.ndarray, pattern: tuple[int, int]) -> tuple[bool, np.ndarray]:
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    if hasattr(cv2, "findChessboardCornersSB"):
        flags = cv2.CALIB_CB_EXHAUSTIVE | cv2.CALIB_CB_ACCURACY
        found, corners = cv2.findChessboardCornersSB(gray, pattern, flags)
        if found:
            return True, corners.astype(np.float32)

    found, corners = cv2.findChessboardCorners(
        gray,
        pattern,
        cv2.CALIB_CB_ADAPTIVE_THRESH
        | cv2.CALIB_CB_NORMALIZE_IMAGE
        | cv2.CALIB_CB_FAST_CHECK,
    )
    if found:
        corners = cv2.cornerSubPix(
            gray,
            corners,
            (11, 11),
            (-1, -1),
            (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001),
        )
    return bool(found), corners


def fetch_snapshot(url: str, timeout_s: float = 2.0) -> np.ndarray:
    request = urllib.request.Request(url, headers={"Cache-Control": "no-cache"})
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        data = np.frombuffer(response.read(), dtype=np.uint8)
    image = cv2.imdecode(data, cv2.IMREAD_COLOR)
    if image is None:
        raise RuntimeError("snapshot.jpg 无法解码")
    return image


def load_samples(save_dir: Path, pattern: tuple[int, int]) -> list[Sample]:
    samples: list[Sample] = []
    if not save_dir.exists():
        raise RuntimeError(f"图片目录不存在: {save_dir}")
    for path in sorted(save_dir.iterdir()):
        if not path.is_file() or path.suffix.lower() not in {".png", ".jpg", ".jpeg", ".bmp"}:
            continue
        image = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if image is None:
            print(f"跳过无法读取的图片: {path}")
            continue
        found, corners = detect_corners(image, pattern)
        if not found:
            print(f"未找到角点: {path}")
            continue
        samples.append(Sample(path, corners, (image.shape[1], image.shape[0])))
        print(f"加载: {path} corners={len(corners)}")
    return samples


def make_object_points(pattern: tuple[int, int], square_size: float) -> np.ndarray:
    points = np.zeros((pattern[0] * pattern[1], 3), dtype=np.float32)
    grid = np.mgrid[0 : pattern[0], 0 : pattern[1]].T.reshape(-1, 2)
    points[:, :2] = grid * square_size
    return points


def calibrate(samples: list[Sample], pattern: tuple[int, int], square_size: float):
    object_template = make_object_points(pattern, square_size)
    object_points = [object_template.copy() for _ in samples]
    image_points = [sample.corners for sample in samples]
    image_size = samples[0].image_size

    rms, camera_matrix, distortion, rvecs, tvecs = cv2.calibrateCamera(
        object_points,
        image_points,
        image_size,
        None,
        None,
    )

    per_view: list[float] = []
    for obj, img, rvec, tvec in zip(object_points, image_points, rvecs, tvecs):
        projected, _ = cv2.projectPoints(
            obj, rvec, tvec, camera_matrix, distortion
        )
        error = cv2.norm(img, projected, cv2.NORM_L2) / np.sqrt(len(projected))
        per_view.append(float(error))
    return rms, camera_matrix, distortion, per_view, image_size


def save_result(
    output: Path,
    samples: list[Sample],
    pattern: tuple[int, int],
    square_size: float,
    rms: float,
    camera_matrix: np.ndarray,
    distortion: np.ndarray,
    per_view: list[float],
    image_size: tuple[int, int],
) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    storage = cv2.FileStorage(str(output), cv2.FILE_STORAGE_WRITE)
    if not storage.isOpened():
        raise RuntimeError(f"无法写入标定结果: {output}")
    storage.write("image_width", image_size[0])
    storage.write("image_height", image_size[1])
    storage.write("board_width", pattern[0])
    storage.write("board_height", pattern[1])
    storage.write("square_size", square_size)
    storage.write("rms_reprojection_error", float(rms))
    storage.write("mean_per_image_reprojection_error", float(np.mean(per_view)))
    storage.write("camera_matrix", camera_matrix)
    storage.write("distortion_coefficients", distortion)
    storage.startWriteStruct("images", cv2.FileNode_SEQ)
    for sample, error in zip(samples, per_view):
        storage.startWriteStruct("", cv2.FileNode_MAP)
        storage.write("file", str(sample.path))
        storage.write("reprojection_error", float(error))
        storage.endWriteStruct()
    storage.endWriteStruct()
    storage.release()


def replace_yaml_scalar(path: Path, key: str, value: float) -> None:
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(rf"^(\s*{re.escape(key)}\s*:\s*).*$", re.MULTILINE)
    replacement = rf"\g<1>{value:.6f}"
    updated, count = pattern.subn(replacement, text, count=1)
    if count == 0:
        raise RuntimeError(f"{path} 中没有找到配置键: {key}")
    path.write_text(updated, encoding="utf-8")


def apply_intrinsics(config_paths: list[Path], camera_matrix: np.ndarray) -> None:
    values = {
        "cam_fx": float(camera_matrix[0, 0]),
        "cam_fy": float(camera_matrix[1, 1]),
        "cam_cx": float(camera_matrix[0, 2]),
        "cam_cy": float(camera_matrix[1, 2]),
    }
    for path in config_paths:
        backup = path.with_name(path.name + ".bak-calib")
        if not backup.exists():
            backup.write_bytes(path.read_bytes())
        for key, value in values.items():
            replace_yaml_scalar(path, key, value)
        print(f"已写入内参: {path}")


def capture_samples(args: argparse.Namespace, pattern: tuple[int, int]) -> list[Sample]:
    save_dir = Path(args.save_dir)
    save_dir.mkdir(parents=True, exist_ok=True)
    samples: list[Sample] = []
    next_index = 1
    while True:
        try:
            image = fetch_snapshot(args.url)
        except Exception as exc:
            print(f"获取 {args.url} 失败: {exc}", file=sys.stderr)
            time.sleep(0.2)
            continue

        found, corners = detect_corners(image, pattern)
        display = image.copy()
        cv2.drawChessboardCorners(display, pattern, corners, found)
        status = f"saved={len(samples)} corners={'found' if found else 'not found'}"
        cv2.putText(
            display,
            status,
            (20, 40),
            cv2.FONT_HERSHEY_SIMPLEX,
            1.0,
            (0, 255, 0),
            2,
        )
        cv2.imshow(args.window_name, display)
        key = cv2.waitKey(1) & 0xFF

        if key == 27:
            break
        if key in (ord("s"), ord("S")):
            if not found:
                print("当前帧未找到完整棋盘格，未保存。")
                continue
            path = save_dir / f"calib_{next_index:04d}.png"
            next_index += 1
            if not cv2.imwrite(str(path), image):
                print(f"保存失败: {path}", file=sys.stderr)
                continue
            samples.append(Sample(path, corners, (image.shape[1], image.shape[0])))
            print(f"已保存: {path} corners={len(corners)}")
        elif key in (ord("d"), ord("D")):
            if not samples:
                print("没有可删除的样本。")
                continue
            last = samples.pop()
            last.path.unlink(missing_ok=True)
            print(f"已删除: {last.path}")

    cv2.destroyAllWindows()
    return samples


def main() -> int:
    parser = argparse.ArgumentParser(
        description="LicheeRV Nano GC4653 主机侧棋盘格标定工具"
    )
    parser.add_argument(
        "--calib",
        action="store_true",
        help="运行标定模块；不带此参数时只显示帮助",
    )
    parser.add_argument(
        "--url",
        default="http://10.133.124.1:8081/snapshot.jpg",
        help="板端单帧抓图 URL",
    )
    parser.add_argument("--offline", action="store_true", help="只读取 save-dir 中的图片")
    parser.add_argument("--save-dir", default="calibration_images")
    parser.add_argument("--output", default="calibration/camera_intrinsics.yaml")
    parser.add_argument("--board-width", type=int, default=11, help="横向内角点数")
    parser.add_argument("--board-height", type=int, default=8, help="纵向内角点数")
    parser.add_argument("--square-size", type=float, default=0.045, help="方格边长，单位米")
    parser.add_argument("--min-images", type=int, default=20)
    parser.add_argument("--window-name", default="GC4653 Calibration")
    parser.add_argument(
        "--apply-config",
        action="store_true",
        help="将 fx/fy/cx/cy 写入 config.yaml 和 config_boot.yaml，并先创建 .bak-calib",
    )
    parser.add_argument("--config", default="config.yaml")
    parser.add_argument("--boot-config", default="config_boot.yaml")
    args = parser.parse_args()

    if not args.calib:
        parser.print_help()
        return 2
    if args.board_width <= 1 or args.board_height <= 1:
        parser.error("board_width 和 board_height 必须大于 1")
    if args.square_size <= 0:
        parser.error("square-size 必须大于 0")
    if args.min_images < 3:
        parser.error("min-images 至少为 3")

    pattern = (args.board_width, args.board_height)
    if args.offline:
        samples = load_samples(Path(args.save_dir), pattern)
    else:
        samples = capture_samples(args, pattern)

    if len(samples) < args.min_images:
        print(
            f"有效图片只有 {len(samples)} 张，至少需要 {args.min_images} 张，跳过标定。",
            file=sys.stderr,
        )
        return 1

    rms, camera_matrix, distortion, per_view, image_size = calibrate(
        samples, pattern, args.square_size
    )
    save_result(
        Path(args.output),
        samples,
        pattern,
        args.square_size,
        rms,
        camera_matrix,
        distortion,
        per_view,
        image_size,
    )

    print("\nCalibration finished")
    print(f"Images: {len(samples)}")
    print(f"Image size: {image_size[0]}x{image_size[1]}")
    print(f"RMS reprojection error: {rms:.4f} px")
    print(f"Mean per-image error: {np.mean(per_view):.4f} px")
    print("Camera matrix:")
    print(camera_matrix)
    print("Distortion coefficients:")
    print(distortion.reshape(-1))
    print(f"Saved calibration result to: {args.output}")

    if args.apply_config:
        apply_intrinsics(
            [Path(args.config), Path(args.boot_config)],
            camera_matrix,
        )
        print("注意：当前板端代码只读取 fx/fy/cx/cy；畸变系数已保存到结果文件，但尚未进入实时校正链路。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
