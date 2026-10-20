"""Source guards for password keyboard configuration (not device/IME tests).

Run: python -m unittest discover -s android/tests -v
"""
from pathlib import Path
import re
import unittest


APP = Path(__file__).resolve().parents[1] / "app/src/main/java/org/megavideoprotect/app"


class PasswordKeyboardGuards(unittest.TestCase):
    def test_every_masked_input_uses_password_keyboard_without_autocorrect(self):
        masked_inputs = 0
        for path in APP.glob("*.kt"):
            code = path.read_text(encoding="utf-8")
            if "PasswordVisualTransformation()" not in code:
                continue
            masked_inputs += 1
            with self.subTest(file=path.name):
                mask = re.search(
                    r"visualTransformation\s*=\s*if\s*\((\w+)\)\s*PasswordVisualTransformation\(\)",
                    code,
                )
                if mask is None:
                    # Dedicated password fields need no conditional wrapper, but
                    # must configure the same field's IME rather than just mask it.
                    self.assertRegex(
                        code,
                        r"visualTransformation\s*=\s*PasswordVisualTransformation\(\),\s*"
                        r"keyboardOptions\s*=\s*KeyboardOptions\(\s*"
                        r"keyboardType\s*=\s*KeyboardType\.Password,\s*"
                        r"autoCorrectEnabled\s*=\s*false,?\s*\)",
                        "Dedicated password inputs must suppress keyboard suggestions too",
                    )
                else:
                    flag = mask.group(1)
                    self.assertRegex(
                        code,
                        rf"keyboardOptions\s*=\s*if\s*\({flag}\)\s*KeyboardOptions\("
                        r"\s*keyboardType\s*=\s*KeyboardType\.Password,"
                        r"\s*autoCorrectEnabled\s*=\s*false,?\s*\)\s*else\s*KeyboardOptions\.Default",
                        "Masking alone does not identify a password field to the keyboard",
                    )
                self.assertIn("import androidx.compose.foundation.text.KeyboardOptions", code)
                self.assertIn("import androidx.compose.ui.text.input.KeyboardType", code)
        self.assertGreater(masked_inputs, 0, "No password fields checked")


if __name__ == "__main__":
    unittest.main()
