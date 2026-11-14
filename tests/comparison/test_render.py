import copy
import json
import re
import shutil
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "docs" / "comparison"))

import render  # noqa: E402

FIXTURE = Path(__file__).with_name("fixture.json")
SVG_NS = "{http://www.w3.org/2000/svg}"


def fixture_dict():
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


class LoadDataTests(unittest.TestCase):
    def load_mutated(self, mutate):
        data = fixture_dict()
        mutate(data)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "data.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            return render.load_data(path)

    def test_loads_fixture(self):
        data = render.load_data(FIXTURE)
        self.assertEqual(len(data["repos"]), 3)

    def test_rejects_unknown_verdict(self):
        def mutate(d):
            d["repos"][0]["cells"]["errors"]["v"] = "maybe"
        with self.assertRaisesRegex(ValueError, "a/b.*errors"):
            self.load_mutated(mutate)

    def test_rejects_missing_cell(self):
        def mutate(d):
            del d["repos"][0]["cells"]["screenshot"]
        with self.assertRaisesRegex(ValueError, "a/b.*screenshot"):
            self.load_mutated(mutate)

    def test_rejects_yes_without_evidence(self):
        def mutate(d):
            del d["repos"][1]["cells"]["errors"]["evidence"]
        with self.assertRaisesRegex(ValueError, "Pollux12/vscode-gmod-glua-ls.*errors"):
            self.load_mutated(mutate)

    def test_rejects_star_key(self):
        def mutate(d):
            d["repos"][0]["cells"]["errors"]["star"] = True
        with self.assertRaisesRegex(ValueError, "a/b.*errors.*star"):
            self.load_mutated(mutate)

    def test_rejects_star_key_in_realms(self):
        def mutate(d):
            d["repos"][0]["cells"]["realms"]["S"]["star"] = True
        with self.assertRaisesRegex(ValueError, "a/b.*realms.S.*star"):
            self.load_mutated(mutate)

    def test_rejects_short_sha(self):
        def mutate(d):
            d["repos"][1]["sha"] = "2222222"
        with self.assertRaisesRegex(ValueError, "Pollux12/vscode-gmod-glua-ls.*sha"):
            self.load_mutated(mutate)

    def test_rejects_missing_sha(self):
        def mutate(d):
            del d["repos"][1]["sha"]
        with self.assertRaisesRegex(ValueError, "Pollux12/vscode-gmod-glua-ls.*sha"):
            self.load_mutated(mutate)

    def test_rejects_evidence_in_another_repo(self):
        def mutate(d):
            d["repos"][1]["cells"]["license"]["evidence"] = (
                "https://github.com/Pollux12/gmod-glua-ls/blob/" + "2" * 40 + "/LICENSE")
        with self.assertRaisesRegex(ValueError, "Pollux12/vscode-gmod-glua-ls.*license"):
            self.load_mutated(mutate)

    def test_rejects_evidence_at_another_sha(self):
        def mutate(d):
            d["repos"][1]["cells"]["license"]["evidence"] = (
                "https://github.com/Pollux12/vscode-gmod-glua-ls/blob/" + "4" * 40 + "/LICENSE")
        with self.assertRaisesRegex(ValueError, "Pollux12/vscode-gmod-glua-ls.*license"):
            self.load_mutated(mutate)

    def test_rejects_evidence_on_a_branch(self):
        def mutate(d):
            d["repos"][0]["cells"]["realms"]["C"]["evidence"] = "https://github.com/a/b/blob/main/realms.lua#L2"
        with self.assertRaisesRegex(ValueError, "a/b.*realms.C"):
            self.load_mutated(mutate)

    def test_accepts_tree_and_release_evidence(self):
        def mutate(d):
            d["repos"][1]["cells"]["purelua"]["evidence"] = (
                "https://github.com/Pollux12/vscode-gmod-glua-ls/tree/" + "2" * 40)
            d["repos"][1]["cells"]["license"]["evidence"] = (
                "https://github.com/Pollux12/vscode-gmod-glua-ls/releases/download/v1/x.vsix")
        self.load_mutated(mutate)

    def test_rejects_bad_realms_subcell(self):
        def mutate(d):
            d["repos"][0]["cells"]["realms"]["M"]["v"] = "nope"
        with self.assertRaisesRegex(ValueError, "a/b.*realms"):
            self.load_mutated(mutate)

    def test_rejects_unknown_pick_repo(self):
        def mutate(d):
            d["picks"][0]["repo"] = "nobody/nothing"
        with self.assertRaisesRegex(ValueError, "nobody/nothing"):
            self.load_mutated(mutate)


class StarTests(unittest.TestCase):
    def setUp(self):
        self.data = render.load_data(FIXTURE)

    def test_sole_yes_gets_star(self):
        self.assertIn(("a/b", "errors"), self.data["stars"])
        self.assertIn(("x/a&b", "screenshot"), self.data["stars"])

    def test_tie_gets_no_star(self):
        # purelua and license: two repos say yes
        for col in ("purelua", "license"):
            self.assertFalse([s for s in self.data["stars"] if s[1] == col], col)
        self.assertEqual(len(self.data["stars"]), 2)

    def test_realms_never_starred(self):
        self.assertFalse([s for s in self.data["stars"] if s[1] == "realms"])

    def test_table_marks_only_starred_cells(self):
        table = render.render_table(self.data)
        self.assertEqual(table.count("★"), 2)


class TableTests(unittest.TestCase):
    def setUp(self):
        self.data = render.load_data(FIXTURE)
        self.table = render.render_table(self.data)

    def test_table_links_repo_and_evidence(self):
        self.assertIn("[a/b](https://github.com/a/b)", self.table)
        self.assertIn(f"[✅](https://github.com/a/b/blob/{'1' * 40}/errors.lua#L1) ★", self.table)

    def test_table_escapes_pipe_in_label(self):
        self.assertIn("a\\|b", self.table)

    def test_table_realms_cell(self):
        self.assertIn(
            f"S[✅](https://github.com/a/b/blob/{'1' * 40}/realms.lua#L1) "
            f"C[🟡](https://github.com/a/b/blob/{'1' * 40}/realms.lua#L2) "
            f"M[❌](https://github.com/a/b/blob/{'1' * 40}/realms.lua#L3)",
            self.table,
        )

    def test_table_unverified_is_plain_glyph(self):
        self.assertIn("| ❔ |", self.table)

    def test_table_one_row_per_repo_in_order(self):
        lines = self.table.strip().splitlines()
        self.assertEqual(len(lines), 2 + 3)
        self.assertTrue(lines[2].startswith("| [a/b]"))
        self.assertTrue(lines[3].startswith("| [Pollux12/"))


class SvgTests(unittest.TestCase):
    def setUp(self):
        self.data = render.load_data(FIXTURE)

    def svg(self, theme="light", limit=None):
        return render.render_svg(self.data, theme, limit)

    def test_svg_is_valid_xml_with_ampersand_name(self):
        text = self.svg()
        ET.fromstring(text)
        self.assertIn("a&amp;b", text)

    def test_svg_limit(self):
        root = ET.fromstring(self.svg(limit=2))
        labels = [e for e in root.iter(SVG_NS + "text") if e.get("class") == "repo"]
        self.assertEqual(len(labels), 2)

    def test_svg_label_column_fits_longest_name(self):
        root = ET.fromstring(self.svg())
        cells = [e for e in root.iter(SVG_NS + "rect") if "cell" in (e.get("class") or "").split()]
        first_x = float(cells[0].get("x"))
        self.assertGreaterEqual(first_x, len("Pollux12/vscode-gmod-glua-ls") * 6.2 + 8)

    def test_dark_and_light_differ_only_in_colors(self):
        light, dark = self.svg("light"), self.svg("dark")
        self.assertNotEqual(light, dark)
        count = lambda t: len(list(ET.fromstring(t).iter()))
        self.assertEqual(count(light), count(dark))
        self.assertIn("#24292f", light)
        self.assertIn("#e6edf3", dark)

    def test_unknown_theme_rejected(self):
        with self.assertRaises(ValueError):
            self.svg("sepia")

    def test_svg_has_cells_stars_and_hatch(self):
        root = ET.fromstring(self.svg())
        rects = [e for e in root.iter(SVG_NS + "rect") if "cell" in (e.get("class") or "").split()]
        # 3 repos x 5 columns
        self.assertEqual(len(rects), 15)
        stars = [e for e in root.iter(SVG_NS + "text") if e.text == "★" and e.get("class") == "star"]
        self.assertEqual(len(stars), 2)
        self.assertIn("url(#hatch)", self.svg())

    def test_svg_includes_footer_and_picks(self):
        text = self.svg()
        self.assertIn("as of 2026-11-14", text)
        self.assertNotIn("· as of", text)
        self.assertIn("code read, nothing executed", text)
        self.assertIn("Most capabilities", text)


    def test_svg_explains_star(self):
        n = len(self.data["repos"])
        sentence = f'★ = the only server with a verified "yes" in this column (out of {n} compared)'
        # limit must not change N: it counts every repo in the data, not the drawn ones
        for limit in (None, 2):
            root = ET.fromstring(self.svg(limit=limit))
            texts = ["".join(e.itertext()) for e in root.iter(SVG_NS + "text")]
            self.assertIn(sentence, texts)

    def test_svg_star_sentence_fits_canvas(self):
        root = ET.fromstring(self.svg(limit=2))
        width = float(root.get("width"))
        sentence = [e for e in root.iter(SVG_NS + "text") if "the only server" in "".join(e.itertext())][0]
        self.assertLessEqual(float(sentence.get("x")) + len("".join(sentence.itertext())) * render.FOOT_CHAR_W, width)


class MarkerTests(unittest.TestCase):
    def test_replace_between_markers_requires_markers(self):
        with self.assertRaises(ValueError):
            render.replace_between_markers("no markers here", "x")
        with self.assertRaises(ValueError):
            render.replace_between_markers("<!-- comparison:start -->\nonly start", "x")
        with self.assertRaises(ValueError):
            render.replace_between_markers("only end\n<!-- comparison:end -->", "x")

    def test_replace_between_markers_replaces_block(self):
        text = "head\n<!-- comparison:start -->\nold\n<!-- comparison:end -->\ntail\n"
        out = render.replace_between_markers(text, "NEW")
        self.assertEqual(out, "head\n<!-- comparison:start -->\nNEW\n<!-- comparison:end -->\ntail\n")


class MainTests(unittest.TestCase):
    def test_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            data = tmp / "data.json"
            shutil.copy(FIXTURE, data)
            md = tmp / "COMPARISON.md"
            md.write_text("# T\n\n<!-- comparison:start -->\n<!-- comparison:end -->\n", encoding="utf-8")
            out = tmp / "out"
            out.mkdir()
            argv = ["--data", str(data), "--out-dir", str(out), "--comparison-md", str(md)]
            self.assertEqual(render.main(argv), 0)
            names = ["comparison-light.svg", "comparison-dark.svg"]
            first = [(out / n).read_bytes() for n in names] + [md.read_bytes()]
            self.assertIn(b"[a/b](https://github.com/a/b)", first[2])
            self.assertEqual(render.main(argv), 0)
            second = [(out / n).read_bytes() for n in names] + [md.read_bytes()]
            self.assertEqual(first, second)
            self.assertNotIn(b"\r", b"".join(second))

    def test_default_paths_do_not_depend_on_cwd(self):
        self.assertEqual(render.ROOT, ROOT)


if __name__ == "__main__":
    unittest.main()
