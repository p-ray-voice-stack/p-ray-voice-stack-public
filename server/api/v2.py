from fastapi import APIRouter


router = APIRouter(prefix="/api/v2", tags=["v2"])


@router.get("/healthz")
async def v2_healthz() -> dict[str, str]:
    return {"status": "ok"}
