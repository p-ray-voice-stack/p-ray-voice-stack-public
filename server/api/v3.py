from datetime import datetime, timezone
from threading import RLock
from typing import Literal
from uuid import uuid4

from fastapi import APIRouter, status
from pydantic import BaseModel


class V3ContractResponse(BaseModel):
    path: str
    stage: str
    audience: str
    baseline_board: str
    interaction_shape: str
    public_routes: list[str]
    capabilities: list[str]
    out_of_scope: list[str]
    next_step: str


class V3SessionCreateRequest(BaseModel):
    mode: Literal["hardware-v3-preview"] = "hardware-v3-preview"


class V3RuntimeSessionRecord(BaseModel):
    session_id: str
    mode: str
    status: Literal["accepted", "active", "closed"]
    step: Literal["accepted", "progressing", "closed"]
    created_at: str
    updated_at: str
    final_reason: Literal["client-closed"] | None = None
    read_progression_applied: bool = False


class V3SessionResponse(BaseModel):
    session_id: str
    mode: str
    stage: str
    status: str
    step: str
    final_reason: str | None
    audio_transport: str
    events_transport: str
    supported_capabilities: list[str]
    unsupported_capabilities: list[str]
    note: str


class V3SessionStatusResponse(BaseModel):
    session_id: str
    mode: str
    stage: str
    status: str
    step: str
    updated_at: str | None
    final_reason: str | None
    audio_transport: str
    events_transport: str
    supported_capabilities: list[str]
    unsupported_capabilities: list[str]
    note: str


class V3SessionEventSnapshot(BaseModel):
    event_id: str
    event_type: str
    phase: str
    state: str
    summary: str
    sequence: int


class V3SessionEventsResponse(BaseModel):
    session_id: str
    stage: str
    status: str
    events_transport: str
    events: list[V3SessionEventSnapshot]
    supported_capabilities: list[str]
    unsupported_capabilities: list[str]
    note: str


router = APIRouter(prefix="/api/v3", tags=["v3"])
runtime_session_store: dict[str, V3RuntimeSessionRecord] = {}
runtime_session_store_lock = RLock()


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def create_v3_runtime_session_record(mode: str) -> V3RuntimeSessionRecord:
    now = _now_iso()
    record = V3RuntimeSessionRecord(
        session_id=f"v3s_{uuid4().hex[:12]}",
        mode=mode,
        status="accepted",
        step="accepted",
        created_at=now,
        updated_at=now,
    )
    with runtime_session_store_lock:
        runtime_session_store[record.session_id] = record
    return record.model_copy()


def get_v3_runtime_session_record(session_id: str) -> V3RuntimeSessionRecord | None:
    with runtime_session_store_lock:
        record = runtime_session_store.get(session_id)
    return record.model_copy() if record else None


def advance_v3_runtime_session_record_for_read(
    session_id: str,
) -> V3RuntimeSessionRecord | None:
    with runtime_session_store_lock:
        record = runtime_session_store.get(session_id)
        if not record:
            return None
        if record.status != "closed" and not record.read_progression_applied:
            record = record.model_copy(
                update={
                    "status": "active",
                    "step": "progressing",
                    "updated_at": _now_iso(),
                    "read_progression_applied": True,
                }
            )
            runtime_session_store[session_id] = record
    return record.model_copy()


def close_v3_runtime_session_record(session_id: str) -> V3RuntimeSessionRecord | None:
    with runtime_session_store_lock:
        record = runtime_session_store.get(session_id)
        if not record:
            return None
        if record.status != "closed":
            record = record.model_copy(
                update={
                    "status": "closed",
                    "step": "closed",
                    "updated_at": _now_iso(),
                    "final_reason": "client-closed",
                    "read_progression_applied": True,
                }
            )
            runtime_session_store[session_id] = record
    return record.model_copy()


def build_v3_session_lifecycle_contract(
    session_id: str,
    mode: str,
    step: str,
    supported_capabilities: list[str],
    status_text: str,
    final_reason: str | None,
    note: str,
) -> V3SessionResponse:
    return V3SessionResponse(
        session_id=session_id,
        mode=mode,
        stage="public-preview",
        status=status_text,
        step=step,
        final_reason=final_reason,
        audio_transport="reserved-not-connected",
        events_transport="snapshot-lookup",
        supported_capabilities=supported_capabilities,
        unsupported_capabilities=[
            "persistent session state",
            "real-time audio transport",
            "real-time event streaming",
            "production session orchestration",
            "multi-board hardware support",
        ],
        note=note,
    )


def build_v3_session_events_snapshot_contract(
    session_id: str,
    record: V3RuntimeSessionRecord | None = None,
) -> V3SessionEventsResponse:
    if record:
        projection_map = {
            "accepted": V3SessionEventSnapshot(
                event_id=f"{record.session_id}_evt_current_accepted",
                event_type="session.accepted",
                phase="session",
                state="accepted",
                summary=(
                    "Fixed projection derived from the current in-process session "
                    "record while it remains at the accepted boundary."
                ),
                sequence=1,
            ),
            "active": V3SessionEventSnapshot(
                event_id=f"{record.session_id}_evt_current_active",
                event_type="session.active",
                phase="session",
                state="active",
                summary=(
                    "Fixed projection derived from the current in-process session "
                    "record after the minimal read-side progression."
                ),
                sequence=1,
            ),
            "closed": V3SessionEventSnapshot(
                event_id=f"{record.session_id}_evt_current_closed",
                event_type="session.closed",
                phase="session",
                state="closed",
                summary=(
                    "Fixed terminal projection derived from the current in-process "
                    "session record with neutral final_reason "
                    f"{record.final_reason or 'client-closed'}."
                ),
                sequence=1,
            ),
        }
        projected_event = projection_map.get(record.status)
        if projected_event is None:
            projected_event = V3SessionEventSnapshot(
                event_id=f"{record.session_id}_evt_current_state",
                event_type="session.state",
                phase="session",
                state=record.status,
                summary=(
                    "Fixed minimal projection derived from the current in-process "
                    "session record using the public-preview fallback mapping for "
                    "an unmapped status."
                ),
                sequence=1,
            )
        return V3SessionEventsResponse(
            session_id=record.session_id,
            stage="public-preview",
            status="snapshot-projection",
            events_transport="snapshot-lookup",
            events=[projected_event],
            supported_capabilities=[
                "canonical public-preview event snapshot contract",
                "in-process event snapshot projection",
                "preview contract discovery",
            ],
            unsupported_capabilities=[
                "real-time event streaming",
                "persistent event history",
                "persistent event replay",
                "device-side uplink control events",
            ],
            note=(
                "This endpoint can return a fixed minimal projection derived from "
                "the current in-process, in-memory, ephemeral session record. It "
                "is not history, not replay, not a real-time event transport, and "
                "not a provider event stream."
            ),
        )
    return V3SessionEventsResponse(
        session_id=session_id,
        stage="public-preview",
        status="snapshot-contract",
        events_transport="snapshot-lookup",
        events=[
            V3SessionEventSnapshot(
                event_id=f"{session_id}_evt_0001",
                event_type="session.accepted",
                phase="session",
                state="accepted",
                summary="Canonical public-preview snapshot showing the session acceptance boundary.",
                sequence=1,
            ),
            V3SessionEventSnapshot(
                event_id=f"{session_id}_evt_0002",
                event_type="response.started",
                phase="response",
                state="started",
                summary="Canonical public-preview snapshot showing response generation has conceptually started.",
                sequence=2,
            ),
        ],
        supported_capabilities=[
            "canonical public-preview event snapshot contract",
            "preview contract discovery",
        ],
        unsupported_capabilities=[
            "real-time event streaming",
            "persistent event history",
            "persistent event replay",
            "device-side uplink control events",
        ],
        note=(
            "This endpoint returns canonical public-preview event snapshots only. "
            "It is not a real-time event transport, not a persistent event replay, "
            "and excludes device-side uplink control such as listen."
        ),
    )


def build_v3_session_status_contract(session_id: str) -> V3SessionStatusResponse:
    record = advance_v3_runtime_session_record_for_read(session_id)
    if record:
        supported_capabilities = [
            "runtime-shaped session status contract",
            "in-process accepted session readback",
            "preview contract discovery",
        ]
        note = (
            "This endpoint can read back and, under controlled conditions inside "
            "GET /api/v3/session/{session_id}, advance an in-process, in-memory, "
            "ephemeral session record created by POST /api/v3/session. The record "
            "is not persisted, will be lost on process restart, and does not "
            "indicate that audio, events transport, or any provider is connected."
        )
        if record.status == "active":
            supported_capabilities.insert(2, "in-process read-side progression")
        if record.status == "closed":
            supported_capabilities.insert(2, "retained closed terminal state readback")
            note = (
                "This endpoint can read back a retained closed terminal state for an "
                "in-process, in-memory, ephemeral session record previously closed by "
                "DELETE /api/v3/session/{session_id}. The record is not persisted, "
                "will be lost on process restart, and does not indicate that audio, "
                "events transport, or any provider is connected."
            )
        return V3SessionStatusResponse(
            session_id=record.session_id,
            mode=record.mode,
            stage="public-preview",
            status=record.status,
            step=record.step,
            updated_at=record.updated_at,
            final_reason=record.final_reason,
            audio_transport="reserved-not-connected",
            events_transport="snapshot-lookup",
            supported_capabilities=supported_capabilities,
            unsupported_capabilities=[
                "persistent session state read",
                "real-time audio transport",
                "real-time event streaming",
                "production session orchestration",
                "multi-board hardware support",
            ],
            note=note,
        )
    return V3SessionStatusResponse(
        session_id=session_id,
        mode="hardware-v3-preview",
        stage="public-preview",
        status="accepted",
        step="status-contract",
        updated_at=None,
        final_reason=None,
        audio_transport="reserved-not-connected",
        events_transport="snapshot-lookup",
        supported_capabilities=[
            "runtime-shaped session status contract",
            "preview contract discovery",
        ],
        unsupported_capabilities=[
            "persistent session state read",
            "real-time audio transport",
            "real-time event streaming",
            "production session orchestration",
            "multi-board hardware support",
        ],
        note=(
            "This endpoint returns a runtime-shaped public-preview status contract only. "
            "It does not read persisted runtime state and does not indicate that audio "
            "or events transport is connected."
        ),
    )


@router.get("/healthz")
async def v3_healthz() -> dict[str, str]:
    return {"status": "ok"}


@router.get("/contract", response_model=V3ContractResponse)
async def v3_contract() -> V3ContractResponse:
    return V3ContractResponse(
        path="hardware-v3",
        stage="public-preview",
        audience="advanced developers and hardware teams",
        baseline_board="ESP-VoCat v1.2",
        interaction_shape="session-oriented lower-latency direction",
        public_routes=[
            "/api/v3/healthz",
            "/api/v3/contract",
            "/api/v3/session",
            "/api/v3/session/{session_id}",
            "DELETE /api/v3/session/{session_id}",
            "/api/v3/session/{session_id}/events",
        ],
        capabilities=[
            "public advanced-path contract",
            "public-safe minimal runtime session acceptance",
            "public-safe runtime-shaped session status contract",
            "public-safe in-process session read progression",
            "public-safe in-process session close semantics",
            "public-safe canonical event snapshot contract",
            "public-safe in-process event snapshot projection",
            "v3 namespace reserved for future session-oriented expansion",
        ],
        out_of_scope=[
            "real-time audio transport",
            "production deployment",
            "persistent session storage",
            "multi-board support",
        ],
        next_step=(
            "Review examples/hardware-v3/README.md, then use "
            "POST /api/v3/session, GET /api/v3/session/{session_id}, "
            "DELETE /api/v3/session/{session_id}, and "
            "GET /api/v3/session/{session_id}/events "
            "as the current public preview steps, with "
            "POST /api/v3/session serving as the minimal runtime acceptance seed "
            "that creates an in-process ephemeral accepted session record, "
            "DELETE /api/v3/session/{session_id} serving as the minimal "
            "in-process close path that can retain a closed terminal state for "
            "follow-up reads, "
            "GET /api/v3/session/{session_id} serving as the runtime-shaped "
            "status contract with a controlled in-process read-side progression path, "
            "and GET /api/v3/session/{session_id}/events serving as the canonical "
            "event snapshot contract with a fixed in-process projection path for "
            "current-process session records."
        ),
    )


@router.post(
    "/session",
    response_model=V3SessionResponse,
    status_code=status.HTTP_201_CREATED,
)
async def v3_create_session(
    request: V3SessionCreateRequest | None = None,
) -> V3SessionResponse:
    request = request or V3SessionCreateRequest()
    record = create_v3_runtime_session_record(request.mode)
    return build_v3_session_lifecycle_contract(
        session_id=record.session_id,
        mode=record.mode,
        step=record.step,
        supported_capabilities=[
            "minimal runtime session acceptance",
            "in-process ephemeral accepted session record",
            "preview contract discovery",
        ],
        status_text=record.status,
        final_reason=None,
        note=(
            "This endpoint creates an in-process, in-memory, ephemeral accepted "
            "session record. The record is not persisted, will be lost on process "
            "restart, and does not indicate that audio or events transport is "
            "connected."
        ),
    )


@router.get("/session/{session_id}", response_model=V3SessionStatusResponse)
async def v3_get_session(session_id: str) -> V3SessionStatusResponse:
    return build_v3_session_status_contract(session_id)


@router.delete(
    "/session/{session_id}",
    response_model=V3SessionResponse,
    status_code=status.HTTP_202_ACCEPTED,
)
async def v3_close_session(session_id: str) -> V3SessionResponse:
    record = close_v3_runtime_session_record(session_id)
    if record:
        return build_v3_session_lifecycle_contract(
            session_id=record.session_id,
            mode=record.mode,
            step=record.step,
            supported_capabilities=[
                "runtime-shaped session close contract",
                "in-process close semantics",
                "retained closed terminal state readback",
                "preview contract discovery",
            ],
            status_text=record.status,
            final_reason=record.final_reason,
            note=(
                "This endpoint can apply a minimal in-process close semantics to a "
                "current-process, in-memory, ephemeral session record. The record "
                "remains readable through GET /api/v3/session/{session_id} as a "
                "closed terminal state, is not persisted, will be lost on process "
                "restart, and does not indicate that audio or events transport is "
                "connected."
            ),
        )
    return build_v3_session_lifecycle_contract(
        session_id=session_id,
        mode="hardware-v3-preview",
        step="close-contract",
        supported_capabilities=[
            "runtime-shaped session close contract",
            "preview contract discovery",
        ],
        status_text="close-acknowledged",
        final_reason=None,
        note=(
            "This endpoint returns a runtime-shaped public-preview session close "
            "contract when the session is not present in the current in-process "
            "store. It does not terminate persisted runtime state and does not "
            "indicate that audio or events transport is connected."
        ),
    )


@router.get(
    "/session/{session_id}/events",
    response_model=V3SessionEventsResponse,
)
async def v3_get_session_events(session_id: str) -> V3SessionEventsResponse:
    return build_v3_session_events_snapshot_contract(
        session_id,
        record=get_v3_runtime_session_record(session_id),
    )
