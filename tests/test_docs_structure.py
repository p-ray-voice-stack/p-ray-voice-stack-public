from pathlib import Path


def test_public_showcase_repo_has_required_entries():
    root = Path(__file__).resolve().parents[1]
    required = [
        "README.md",
        "LICENSE",
        ".env.example",
        "requirements.txt",
        "docs/quickstart.md",
        "docs/v2-v3-comparison.md",
        "docs/v3-runtime-boundary.md",
        "docs/license-and-commercial-boundary.md",
        "commercial/README.md",
        "commercial/services.md",
        "commercial/intake-form.md",
        "server/api",
        "server/services",
        "examples/local-v2/README.md",
        "examples/local-v2/web",
        "examples/hardware-v2/README.md",
        "examples/hardware-v3/README.md",
        "hardware/esp-vocat-v1.2/README.md",
        "hardware/esp-vocat-v1.2/firmware",
    ]
    missing = [item for item in required if not (root / item).exists()]
    assert missing == []


def test_internal_planning_docs_are_not_part_of_public_showcase():
    root = Path(__file__).resolve().parents[1]
    assert list(root.glob("20*-*.md")) == []
    assert not (root / "docs/content").exists()
    assert not (root / ".codex").exists()
