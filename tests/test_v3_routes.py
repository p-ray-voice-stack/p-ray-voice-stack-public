import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from server.api.v3 import (
    V3ContractResponse,
    V3SessionCreateRequest,
    V3SessionEventSnapshot,
    V3SessionEventsResponse,
    V3SessionResponse,
    V3RuntimeSessionRecord,
    V3SessionStatusResponse,
    get_v3_runtime_session_record,
    v3_close_session,
    v3_get_session_events,
    v3_get_session,
    v3_contract,
    v3_create_session,
    v3_healthz,
)
from server.app import app


@pytest.mark.anyio
async def test_v3_healthz_returns_ok():
    assert await v3_healthz() == {"status": "ok"}


@pytest.mark.anyio
async def test_v3_contract_describes_public_safe_advanced_path():
    contract = await v3_contract()

    assert isinstance(contract, V3ContractResponse)
    assert contract.path == "hardware-v3"
    assert contract.stage == "public-preview"
    assert contract.baseline_board == "ESP-VoCat v1.2"
    assert "/api/v3/contract" in contract.public_routes
    assert "/api/v3/session" in contract.public_routes
    assert "/api/v3/session/{session_id}" in contract.public_routes
    assert "DELETE /api/v3/session/{session_id}" in contract.public_routes
    assert "/api/v3/session/{session_id}/events" in contract.public_routes
    assert "public advanced-path contract" in contract.capabilities
    assert "public-safe minimal runtime session acceptance" in contract.capabilities
    assert "public-safe runtime-shaped session status contract" in contract.capabilities
    assert "public-safe in-process session read progression" in contract.capabilities
    assert "public-safe in-process session close semantics" in contract.capabilities
    assert "public-safe canonical event snapshot contract" in contract.capabilities
    assert "public-safe in-process event snapshot projection" in contract.capabilities
    assert "production deployment" in contract.out_of_scope
    assert "persistent session storage" in contract.out_of_scope


@pytest.mark.anyio
async def test_v3_session_create_returns_runtime_shaped_public_preview_contract():
    session = await v3_create_session(V3SessionCreateRequest())
    stored_record = get_v3_runtime_session_record(session.session_id)

    assert isinstance(session, V3SessionResponse)
    assert session.session_id.startswith("v3s_")
    assert session.mode == "hardware-v3-preview"
    assert session.stage == "public-preview"
    assert session.status == "accepted"
    assert session.step == "accepted"
    assert session.final_reason is None
    assert session.audio_transport == "reserved-not-connected"
    assert session.events_transport == "snapshot-lookup"
    assert "minimal runtime session acceptance" in session.supported_capabilities
    assert "in-process ephemeral accepted session record" in session.supported_capabilities
    assert "persistent session state" in session.unsupported_capabilities
    assert "real-time audio transport" in session.unsupported_capabilities
    assert "real-time event streaming" in session.unsupported_capabilities
    assert isinstance(stored_record, V3RuntimeSessionRecord)
    assert stored_record.mode == "hardware-v3-preview"
    assert stored_record.status == "accepted"
    assert stored_record.step == "accepted"
    assert stored_record.created_at == stored_record.updated_at
    assert "in-process, in-memory, ephemeral accepted session record" in session.note
    assert "will be lost on process restart" in session.note
    assert "does not indicate that audio or events transport is connected" in session.note


@pytest.mark.anyio
async def test_v3_session_create_uses_default_request_when_missing():
    session = await v3_create_session()

    assert session.mode == "hardware-v3-preview"
    assert session.status == "accepted"
    assert session.step == "accepted"


@pytest.mark.anyio
async def test_v3_session_read_returns_runtime_shaped_public_preview_contract():
    session = await v3_get_session("v3s_demo123")

    assert isinstance(session, V3SessionStatusResponse)
    assert session.session_id == "v3s_demo123"
    assert session.mode == "hardware-v3-preview"
    assert session.stage == "public-preview"
    assert session.status == "accepted"
    assert session.step == "status-contract"
    assert session.updated_at is None
    assert session.final_reason is None
    assert session.audio_transport == "reserved-not-connected"
    assert session.events_transport == "snapshot-lookup"
    assert "runtime-shaped session status contract" in session.supported_capabilities
    assert "persistent session state read" in session.unsupported_capabilities
    assert "real-time audio transport" in session.unsupported_capabilities
    assert "real-time event streaming" in session.unsupported_capabilities
    assert "runtime-shaped public-preview status contract" in session.note
    assert "does not read persisted runtime state" in session.note
    assert "does not indicate that audio or events transport is connected" in session.note


@pytest.mark.anyio
async def test_v3_session_read_can_reflect_created_ephemeral_record():
    created_session = await v3_create_session(V3SessionCreateRequest())
    created_record = get_v3_runtime_session_record(created_session.session_id)
    session = await v3_get_session(created_session.session_id)
    progressed_record = get_v3_runtime_session_record(created_session.session_id)
    repeated_session = await v3_get_session(created_session.session_id)

    assert isinstance(session, V3SessionStatusResponse)
    assert session.session_id == created_session.session_id
    assert session.mode == "hardware-v3-preview"
    assert created_record is not None
    assert created_record.status == "accepted"
    assert created_record.step == "accepted"
    assert session.status == "active"
    assert session.step == "progressing"
    assert session.updated_at is not None
    assert progressed_record is not None
    assert progressed_record.status == "active"
    assert progressed_record.step == "progressing"
    assert progressed_record.updated_at != created_record.updated_at
    assert session.updated_at == progressed_record.updated_at
    assert repeated_session.updated_at == progressed_record.updated_at
    assert "in-process accepted session readback" in session.supported_capabilities
    assert "in-process read-side progression" in session.supported_capabilities
    assert "persistent session state read" in session.unsupported_capabilities
    assert "advance an in-process, in-memory, ephemeral session record" in session.note
    assert "inside GET /api/v3/session/{session_id}" in session.note
    assert "will be lost on process restart" in session.note
    assert "or any provider is connected" in session.note


@pytest.mark.anyio
async def test_v3_session_close_returns_runtime_shaped_public_preview_contract():
    session = await v3_close_session("v3s_demo123")

    assert isinstance(session, V3SessionResponse)
    assert session.session_id == "v3s_demo123"
    assert session.mode == "hardware-v3-preview"
    assert session.stage == "public-preview"
    assert session.status == "close-acknowledged"
    assert session.step == "close-contract"
    assert session.final_reason is None
    assert session.audio_transport == "reserved-not-connected"
    assert session.events_transport == "snapshot-lookup"
    assert "runtime-shaped session close contract" in session.supported_capabilities
    assert "persistent session state" in session.unsupported_capabilities
    assert "real-time audio transport" in session.unsupported_capabilities
    assert "real-time event streaming" in session.unsupported_capabilities
    assert "runtime-shaped public-preview session close contract" in session.note
    assert "not present in the current in-process store" in session.note
    assert "does not terminate persisted runtime state" in session.note
    assert "does not indicate that audio or events transport is connected" in session.note


@pytest.mark.anyio
async def test_v3_session_close_can_apply_in_process_close_semantics():
    created_session = await v3_create_session(V3SessionCreateRequest())
    closed_session = await v3_close_session(created_session.session_id)
    stored_record = get_v3_runtime_session_record(created_session.session_id)
    read_after_close = await v3_get_session(created_session.session_id)
    repeated_read = await v3_get_session(created_session.session_id)

    assert isinstance(closed_session, V3SessionResponse)
    assert closed_session.session_id == created_session.session_id
    assert closed_session.mode == "hardware-v3-preview"
    assert closed_session.status == "closed"
    assert closed_session.step == "closed"
    assert closed_session.final_reason == "client-closed"
    assert "in-process close semantics" in closed_session.supported_capabilities
    assert "retained closed terminal state readback" in closed_session.supported_capabilities
    assert "current-process, in-memory, ephemeral session record" in closed_session.note
    assert "remains readable through GET /api/v3/session/{session_id}" in closed_session.note
    assert "will be lost on process restart" in closed_session.note
    assert stored_record is not None
    assert stored_record.status == "closed"
    assert stored_record.step == "closed"
    assert stored_record.final_reason == "client-closed"
    assert isinstance(read_after_close, V3SessionStatusResponse)
    assert read_after_close.status == "closed"
    assert read_after_close.step == "closed"
    assert read_after_close.final_reason == "client-closed"
    assert read_after_close.updated_at == stored_record.updated_at
    assert "retained closed terminal state readback" in read_after_close.supported_capabilities
    assert "previously closed by DELETE /api/v3/session/{session_id}" in read_after_close.note
    assert repeated_read.status == "closed"
    assert repeated_read.step == "closed"
    assert repeated_read.final_reason == "client-closed"
    assert repeated_read.updated_at == read_after_close.updated_at


@pytest.mark.anyio
async def test_v3_session_events_returns_canonical_public_preview_snapshot_contract():
    events = await v3_get_session_events("v3s_demo123")

    assert isinstance(events, V3SessionEventsResponse)
    assert events.session_id == "v3s_demo123"
    assert events.stage == "public-preview"
    assert events.status == "snapshot-contract"
    assert events.events_transport == "snapshot-lookup"
    assert isinstance(events.events[0], V3SessionEventSnapshot)
    assert events.events[0].event_type == "session.accepted"
    assert events.events[0].phase == "session"
    assert events.events[0].state == "accepted"
    assert events.events[0].sequence == 1
    assert events.events[1].event_type == "response.started"
    assert events.events[1].phase == "response"
    assert events.events[1].state == "started"
    assert events.events[1].sequence == 2
    assert "canonical public-preview event snapshot contract" in events.supported_capabilities
    assert "real-time event streaming" in events.unsupported_capabilities
    assert "persistent event replay" in events.unsupported_capabilities
    assert "device-side uplink control events" in events.unsupported_capabilities
    assert "not a real-time event transport" in events.note
    assert "not a persistent event replay" in events.note
    assert "excludes device-side uplink control such as listen" in events.note


@pytest.mark.anyio
async def test_v3_session_events_can_project_current_accepted_record():
    created_session = await v3_create_session(V3SessionCreateRequest())
    events = await v3_get_session_events(created_session.session_id)

    assert isinstance(events, V3SessionEventsResponse)
    assert events.session_id == created_session.session_id
    assert events.stage == "public-preview"
    assert events.status == "snapshot-projection"
    assert events.events_transport == "snapshot-lookup"
    assert len(events.events) == 1
    assert events.events[0].event_type == "session.accepted"
    assert events.events[0].phase == "session"
    assert events.events[0].state == "accepted"
    assert events.events[0].sequence == 1
    assert "in-process event snapshot projection" in events.supported_capabilities
    assert "persistent event history" in events.unsupported_capabilities
    assert "fixed minimal projection" in events.note
    assert "not history, not replay" in events.note
    assert "not a provider event stream" in events.note


@pytest.mark.anyio
async def test_v3_session_events_can_project_current_active_record():
    created_session = await v3_create_session(V3SessionCreateRequest())
    await v3_get_session(created_session.session_id)
    events = await v3_get_session_events(created_session.session_id)

    assert isinstance(events, V3SessionEventsResponse)
    assert events.status == "snapshot-projection"
    assert len(events.events) == 1
    assert events.events[0].event_type == "session.active"
    assert events.events[0].phase == "session"
    assert events.events[0].state == "active"
    assert events.events[0].sequence == 1


@pytest.mark.anyio
async def test_v3_session_events_can_project_current_closed_record():
    created_session = await v3_create_session(V3SessionCreateRequest())
    await v3_close_session(created_session.session_id)
    events = await v3_get_session_events(created_session.session_id)

    assert isinstance(events, V3SessionEventsResponse)
    assert events.status == "snapshot-projection"
    assert len(events.events) == 1
    assert events.events[0].event_type == "session.closed"
    assert events.events[0].phase == "session"
    assert events.events[0].state == "closed"
    assert events.events[0].sequence == 1
    assert "client-closed" in events.events[0].summary


def test_app_registers_v3_routes():
    paths = {route.path for route in app.routes}
    assert "/api/v3/healthz" in paths
    assert "/api/v3/contract" in paths
    assert "/api/v3/session" in paths
    assert "/api/v3/session/{session_id}" in paths
    assert "/api/v3/session/{session_id}/events" in paths

    session_detail_methods = set()
    for route in app.routes:
        if route.path == "/api/v3/session/{session_id}":
            session_detail_methods |= route.methods or set()

    assert "GET" in session_detail_methods
    assert "DELETE" in session_detail_methods


def test_v3_docs_exist_and_describe_advanced_path():
    hardware_v3 = (ROOT / "examples/hardware-v3/README.md").read_text(encoding="utf-8")
    comparison = (ROOT / "docs/v2-v3-comparison.md").read_text(encoding="utf-8")

    assert "advanced public hardware path" in hardware_v3
    assert "official `ESP-VoCat v1.2` baseline" in hardware_v3
    assert "/api/v3/contract" in hardware_v3
    assert "/api/v3/session" in hardware_v3
    assert "/api/v3/session/{session_id}" in hardware_v3
    assert "in-process, in-memory, ephemeral accepted session record" in hardware_v3
    assert "runtime-shaped public-preview status contract" in hardware_v3
    assert "single in-process progression" in hardware_v3
    assert "`updated_at`" in hardware_v3
    assert "DELETE /api/v3/session/{session_id}" in hardware_v3
    assert "in-process close semantics" in hardware_v3
    assert "`client-closed`" in hardware_v3
    assert "closed terminal state" in hardware_v3
    assert "/api/v3/session/{session_id}/events" in hardware_v3
    assert "canonical public-preview event snapshot contract" in hardware_v3
    assert "in-process snapshot projection" in hardware_v3
    assert "`session.active`" in hardware_v3
    assert "not history / replay / stream" in hardware_v3
    assert "# V2 vs V3" in comparison
    assert "| `v2` |" in comparison
    assert "| `v3` |" in comparison
    assert "/api/v3/" in comparison
    assert "/api/v3/contract" in comparison
    assert "/api/v3/session" in comparison
    assert "/api/v3/session/{session_id}" in comparison
    assert "minimal real runtime session acceptance" in comparison
    assert "runtime-shaped public-preview status contract" in comparison
    assert "single in-process progression" in comparison
    assert "`updated_at`" in comparison
    assert "DELETE /api/v3/session/{session_id}" in comparison
    assert "in-process close semantics" in comparison
    assert "`client-closed`" in comparison
    assert "closed terminal state" in comparison
    assert "/api/v3/session/{session_id}/events" in comparison
    assert "canonical public-preview event snapshot contract" in comparison
    assert "in-process snapshot projection" in comparison
    assert "`session.active`" in comparison
    assert "not history / replay / stream" in comparison
