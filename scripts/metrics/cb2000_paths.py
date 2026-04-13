#!/usr/bin/env python3
"""Shared path defaults for CB2000 operational scripts."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Iterable


PROJECT_ROOT = Path(__file__).resolve().parent.parent


def _expand_env_path(name: str) -> Path | None:
    value = os.environ.get(name)
    if not value:
        return None
    return Path(value).expanduser()


def _default_dbx_name() -> str:
    return os.environ.get("DBX_NAME", "canvasbio")


def container_config_root() -> Path:
    home = Path.home()
    dbx_name = _default_dbx_name()
    if home.name == dbx_name and home.parent.name == ".ContainerConfigs":
        return home
    if home.name == ".ContainerConfigs":
        return home / dbx_name
    return home / ".ContainerConfigs" / dbx_name


DEFAULT_SESSION_LOGS_ROOT = _expand_env_path("CB2000_LOG_ROOT") or (
    PROJECT_ROOT.parent / "dev_logs" / "sessions"
)
DEFAULT_RUNTIME_ROOT = _expand_env_path("CB2000_RUNTIME_ROOT") or (
    container_config_root() / "cb2000_runtime"
)
DEFAULT_RUNTIME_LATEST_ROOT = DEFAULT_RUNTIME_ROOT / "latest"
DEFAULT_RUNS_ROOT = (
    DEFAULT_RUNTIME_ROOT / "runs"
    if (DEFAULT_RUNTIME_ROOT / "runs").exists()
    else DEFAULT_SESSION_LOGS_ROOT
)


def pick_latest_dir(root: Path, required_files: Iterable[str]) -> Path | None:
    if not root.exists():
        return None

    dirs = sorted((p for p in root.iterdir() if p.is_dir()), key=lambda p: p.name)
    for candidate in reversed(dirs):
        if any((candidate / name).exists() for name in required_files):
            return candidate
    return None


def default_enroll_log_input() -> Path:
    for candidate in (
        DEFAULT_RUNTIME_LATEST_ROOT / "enroll_log.txt",
        DEFAULT_RUNTIME_LATEST_ROOT / "enroll.log",
    ):
        if candidate.exists():
            return candidate

    latest_run = pick_latest_dir(DEFAULT_RUNS_ROOT, ("enroll_log.txt", "enroll.log"))
    if latest_run is not None:
        for name in ("enroll_log.txt", "enroll.log"):
            candidate = latest_run / name
            if candidate.exists():
                return candidate

    latest_session = pick_latest_dir(
        DEFAULT_SESSION_LOGS_ROOT,
        ("enroll.log", "enroll_log.txt"),
    )
    if latest_session is not None:
        for name in ("enroll.log", "enroll_log.txt"):
            candidate = latest_session / name
            if candidate.exists():
                return candidate

    return DEFAULT_RUNTIME_LATEST_ROOT / "enroll_log.txt"
