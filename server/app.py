from fastapi import FastAPI

from server.api.v2 import router as v2_router
from server.api.v3 import router as v3_router
from server.settings import settings


async def healthz() -> dict[str, str]:
    return {"status": "ok"}


def create_app() -> FastAPI:
    app = FastAPI(title="P-Ray Voice Stack", version="0.1.0")
    app.state.settings = settings
    app.add_api_route("/healthz", healthz, methods=["GET"])
    app.include_router(v2_router)
    app.include_router(v3_router)
    return app


app = create_app()
