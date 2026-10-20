
"""Pins the contrast tool: ratio maths, palette parsing, and the gate verdict."""
import importlib.util, subprocess, sys
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
                       ("Button", "#1c1c1e"), ("Highlight", "#3390ec"),
                       ("HighlightedText", "#ffffff"), ("PlaceholderText", "#6d6d72")]:
    check(dark.get(role) == expected, f"dark {role} parsed", f"{dark.get(role)} expected {expected}")
check("Disabled" not in dark, "dark has no bare 'Disabled' key (group/role not inverted)")

# --- 3. light palette: group:role keys, the inversion bug ------------------
light = mod.parse_palette(mod.THEME_CPP.read_text(encoding="utf-8"), "QPalette lightPalette()")
for key, expected in [("Window", "#f2f2f7"), ("Base", "#ffffff"), ("Text", "#1c1c1e"),
                      ("Disabled:Text", "#8e8e93"), ("Disabled:ButtonText", "#8e8e93")]:
    check(light.get(key) == expected, f"light {key} parsed", f"{light.get(key)} expected {expected}")
check("Disabled" not in light, "light has no bare 'Disabled' key (group/role not inverted)")

# --- 4. the gate: fails on this tree, and its verdict list is right --------
run = subprocess.run([sys.executable, str(HERE / "contrast.py")],
                     capture_output=True, text=True)
check(run.returncode == 1, "exit 1 while pairs are below threshold", f"got {run.returncode}")
listed = run.stdout.split("below threshold:")[-1]
for expected in ("primary button label", "selection label", "dark: placeholder"):
    check(expected in listed, f"verdict names {expected}")
check("disabled" not in listed, "disabled text never fails the gate")

# --- 5. the same tree passes when the bar is lowered -----------------------
loose = subprocess.run([sys.executable, str(HERE / "contrast.py"), "--min", "3.0"],
                       capture_output=True, text=True)
check(loose.returncode == 0, "exit 0 when every pair meets a 3.0 bar", f"got {loose.returncode}")

print(f"\n{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
