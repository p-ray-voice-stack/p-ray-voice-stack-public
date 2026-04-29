import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from server.api.v2 import v2_healthz
from server.app import app, healthz


@pytest.mark.anyio
async def test_healthz_returns_ok():
    assert await healthz() == {"status": "ok"}


@pytest.mark.anyio
async def test_v2_healthz_returns_ok():
    assert await v2_healthz() == {"status": "ok"}


def test_app_registers_health_routes():
    paths = {route.path for route in app.routes}
    assert "/healthz" in paths
    assert "/api/v2/healthz" in paths
