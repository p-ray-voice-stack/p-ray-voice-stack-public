from pathlib import Path


def test_local_v2_demo_has_required_files():
    root = Path(__file__).resolve().parents[1]
    assert (root / "examples/local-v2/README.md").exists()
    assert (root / "examples/local-v2/web/index.html").exists()
    assert (root / "examples/local-v2/web/app.js").exists()


def test_local_v2_readme_describes_minimal_scope():
    root = Path(__file__).resolve().parents[1]
    readme = (root / "examples/local-v2/README.md").read_text(encoding="utf-8")
    assert "default first entry" in readme
    assert "zero hardware requirements" in readme
    assert "real microphone capture" in readme


def test_local_v2_page_has_expected_demo_elements():
    root = Path(__file__).resolve().parents[1]
    html = (root / "examples/local-v2/web/index.html").read_text(encoding="utf-8")
    assert "<h1>Local V2 Demo</h1>" in html
    assert 'id="start-demo"' in html
    assert 'id="answer"' in html
    assert "first-stop pure-software demo" in html


def test_local_v2_script_wires_demo_state():
    root = Path(__file__).resolve().parents[1]
    script = (root / "examples/local-v2/web/app.js").read_text(encoding="utf-8")
    assert 'document.getElementById("start-demo")' in script
    assert "demoTurn" in script
    assert "Simulating a local-v2 turn" in script
