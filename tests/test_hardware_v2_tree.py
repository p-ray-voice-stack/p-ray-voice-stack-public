from pathlib import Path


def test_hardware_v2_has_baseline_readme_and_firmware_dir():
    root = Path(__file__).resolve().parents[1]
    assert (root / "examples/hardware-v2/README.md").exists()
    assert (root / "hardware/esp-vocat-v1.2/README.md").exists()
    assert (root / "hardware/esp-vocat-v1.2/firmware").exists()


def test_hardware_v2_readmes_describe_single_public_baseline():
    root = Path(__file__).resolve().parents[1]
    example_readme = (root / "examples/hardware-v2/README.md").read_text(encoding="utf-8")
    hardware_readme = (root / "hardware/esp-vocat-v1.2/README.md").read_text(encoding="utf-8")

    assert "single official public hardware baseline" in example_readme
    assert "one board baseline only" in example_readme
    assert "default transport for this snapshot: `v2 async`" in hardware_readme
    assert "committed Wi-Fi credentials, device IDs, or server URLs" in hardware_readme


def test_hardware_v2_firmware_tree_has_minimal_public_baseline_files():
    root = Path(__file__).resolve().parents[1]
    firmware_root = root / "hardware/esp-vocat-v1.2/firmware"
    required = [
        "CMakeLists.txt",
        ".gitignore",
        "dependencies.lock",
        "partitions.csv",
        "sdkconfig.defaults",
        "main/CMakeLists.txt",
        "main/idf_component.yml",
        "main/config.h",
        "main/main.c",
        "main/cloud_client.c",
        "main/audio_in.c",
        "main/audio_out.c",
        "main/trigger_input.c",
    ]
    missing = [item for item in required if not (firmware_root / item).exists()]
    assert missing == []


def test_hardware_v2_firmware_defaults_stay_in_public_safe_v2_scope():
    root = Path(__file__).resolve().parents[1]
    firmware_root = root / "hardware/esp-vocat-v1.2/firmware"
    config_text = (firmware_root / "main/config.h").read_text(encoding="utf-8")

    assert "#define DEMO_AUDIO_MODE DEMO_AUDIO_MODE_V2_ASYNC" in config_text
    assert "#define DEMO_REALTIME_INTRO_ENABLED 0" in config_text
    assert "#define DEMO_RECORD_PROMPT_ENABLED 0" in config_text
    assert '#define DEMO_WIFI_SSID           ""' in config_text
    assert '#define DEMO_SERVER_BASE_URL     ""' in config_text
    assert '#define DEMO_DEVICE_ID           ""' in config_text
    assert not (firmware_root / "spiffs/intro_1.pcm").exists()
    assert not (firmware_root / "spiffs/record_prompt_1.pcm").exists()
