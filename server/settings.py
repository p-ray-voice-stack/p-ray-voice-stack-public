import os

try:
    from pydantic_settings import BaseSettings, SettingsConfigDict
except ModuleNotFoundError:
    BaseSettings = None
    SettingsConfigDict = None


if BaseSettings is not None:
    class Settings(BaseSettings):
        base_url: str = "http://127.0.0.1:8000"
        device_id: str = "esp-vocat-v1p2-001"

        model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")


    settings = Settings()
else:
    class Settings:
        def __init__(self) -> None:
            self.base_url = os.getenv("BASE_URL", "http://127.0.0.1:8000")
            self.device_id = os.getenv("DEVICE_ID", "esp-vocat-v1p2-001")


    settings = Settings()
