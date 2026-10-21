
"""Pins the contrast tool: ratio maths, palette parsing, and the gate verdict."""
import importlib.util, pathlib, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("contrast", HERE / "contrast.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

failures = []
def check(ok, label, detail=""):
    print(("PASS  " if ok else "FAIL  ") + label + (f"  [{detail}]" if detail else ""))
    if not ok:
        failures.append(label)

# --- 1. the ratio maths, against WCAG reference pairs ---------------------
cases = [
    ("#ffffff", "#000000", 21.0, "black on white"),
    ("#ffffff", "#ffffff", 1.0, "no contrast"),
    ("#767676", "#ffffff", 4.54, "classic AA boundary pass"),
    ("#777777", "#ffffff", 4.48, "classic AA boundary fail"),
    ("#3390ec", "#ffffff", 3.31, "the app accent"),
]
for fg, bg, expected, label in cases:
    got = mod.contrast_ratio(fg, bg)
    check(abs(got - expected) < 0.02, f"ratio {fg}/{bg} = {got:.2f} ({label})",
          f"expected {expected}")

# --- 2. dark palette: mixed QColor(r,g,b) and Qt::white spellings ----------
dark = mod.parse_palette(mod.THEME_HPP.read_text(encoding="utf-8"), "inline QPalette darkPalette()")
for role, expected in [("Window", "#000000"), ("WindowText", "#ffffff"), ("Text", "#f2f2f7"),
                       ("Button", "#1c1c1e"), ("Highlight", "#2672c8"),
                       ("HighlightedText", "#ffffff"), ("PlaceholderText", "#8e8e93")]:
    check(dark.get(role) == expected, f"dark {role} parsed", f"{dark.get(role)} expected {expected}")
check("Disabled" not in dark, "dark has no bare 'Disabled' key (group/role not inverted)")

# --- 3. light palette: group:role keys, the inversion bug ------------------
light = mod.parse_palette(mod.THEME_CPP.read_text(encoding="utf-8"), "QPalette lightPalette()")
for key, expected in [("Window", "#f2f2f7"), ("Base", "#ffffff"), ("Text", "#1c1c1e"),
                      ("Disabled:Text", "#8e8e93"), ("Disabled:ButtonText", "#8e8e93")]:
    check(light.get(key) == expected, f"light {key} parsed", f"{light.get(key)} expected {expected}")
check("Disabled" not in light, "light has no bare 'Disabled' key (group/role not inverted)")

# --- 4. the gate: fails on a bad palette, and its verdict list is right ----
# A fixture, not this tree: the tree is expected to pass, so asserting failure against it only
# held while the accent was broken.
fixture = pathlib.Path(tempfile.mkdtemp(prefix="contrast-fixture-"))
bad_dark = fixture / "theme_dark.hpp"
bad_light = fixture / "theme_light.cpp"
bad_dark.write_text("""
inline QPalette darkPalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0, 0, 0));
    palette.setColor(QPalette::WindowText, Qt::white);
    palette.setColor(QPalette::Text, QColor(242, 242, 245));
    palette.setColor(QPalette::Button, QColor(28, 28, 30));
    palette.setColor(QPalette::Highlight, QColor(51, 144, 236));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::PlaceholderText, QColor(109, 109, 114));
    return palette;
}
""", encoding="utf-8")
bad_light.write_text("""
QPalette lightPalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(242, 242, 247));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Text, QColor(28, 28, 30));
    palette.setColor(QPalette::Button, Qt::white);
    palette.setColor(QPalette::ButtonText, QColor(28, 28, 30));
    palette.setColor(QPalette::Highlight, QColor(51, 144, 236));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::PlaceholderText, QColor(142, 142, 147));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(176, 176, 181));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(176, 176, 181));
    return palette;
}
""", encoding="utf-8")
run = subprocess.run([sys.executable, str(HERE / "contrast.py"),
                      "--hpp", str(bad_dark), "--cpp", str(bad_light)],
                     capture_output=True, text=True)
check(run.returncode == 1, "exit 1 while pairs are below threshold", f"got {run.returncode}")
listed = run.stdout.split("below threshold:")[-1]
for expected in ("primary button label", "selection label", "dark: placeholder"):
    check(expected in listed, f"verdict names {expected}")
check("disabled" not in listed, "disabled text never fails the gate")

# --- 4b. and the real tree passes, which is the invariant that matters -----
real = subprocess.run([sys.executable, str(HERE / "contrast.py")],
                      capture_output=True, text=True)
check(real.returncode == 0, "the tree's own palette meets AA", f"got {real.returncode}")

# --- 5. the same tree passes when the bar is lowered -----------------------
loose = subprocess.run([sys.executable, str(HERE / "contrast.py"), "--min", "3.0"],
                       capture_output=True, text=True)
check(loose.returncode == 0, "exit 0 when every pair meets a 3.0 bar", f"got {loose.returncode}")

print(f"\n{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
