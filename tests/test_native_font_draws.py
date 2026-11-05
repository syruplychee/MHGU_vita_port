import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools/assets"))
from check_native_font_draws import check, copy_ratio, FONT_VERTEX_SHA


class DrawProofTest(unittest.TestCase):
    def test_1080_buffer_with_720_viewport_loses_one_third_of_pixels(self):
        q=struct.pack("<16f",-1,1,0,0,-1,-1,0,1,1,1,1,0,1,-1,1,1)
        draw=dict(stride=16,count=4,indexed=0,attributes=[0x38200000],
                  target=[1920,1080],texture_size=[1280,720],viewport=[0,0,1280,720])
        old=copy_ratio(draw,q,540)
        self.assertAlmostEqual(old[0],2/3)
        self.assertAlmostEqual(old[1],2/3)
        draw["target"]=[1280,720]
        self.assertEqual(copy_ratio(draw,q,540),(1.,1.))

    def fixture(self, root, *, uv_span=16, shift=0.0):
        index = root/"index.json"
        index.write_text(json.dumps(dict(programs=[dict(token=1,vertex_stage=0)],
                                         stages=[dict(stage_sha256=FONT_VERTEX_SHA)])))
        world=[1.,0,0,0,0,1.,0,0,0,0,1.,0,31.,610.,0,1.]
        vertices=[]
        for x,y,u,v in [(100,200,50,80),(116,200,50+uv_span,80),
                        (100,216,50,80+uv_span),(116,216,50+uv_span,80+uv_span)]:
            vertices.append(struct.pack("<3fI2f",(x+shift)/0.75-31,y/0.75-610,0,0xffffffff,u,v))
        (root/"font-probe-000-vertices.bin").write_bytes(b"".join(vertices))
        (root/"font-probe-000-vs-cbuf3.bin").write_bytes(struct.pack("<16f",*world))
        (root/"font-probe-000-vs-cbuf4.bin").write_bytes(struct.pack("<4f",2/1280,-2/720,-1,1))
        (root/"font-probe-000.json").write_text(json.dumps(dict(vs=1,stride=24,count=4,
            attributes=[0x38400000,0x11400600,0x38800800],target=[1280,720],viewport=[0,0,1280,720])))
        return index

    def test_native_quad_is_one_screen_pixel_per_texel(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); index=self.fixture(root)
            result=check(root,index)
            self.assertTrue(result["passed"])
            self.assertEqual(result["quads"],1)
            self.assertEqual(result["texel_screen_ratios"],[dict(x=1.,y=1.,quads=1)])

    def test_legacy_half_texel_inset_is_detected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); index=self.fixture(root,uv_span=15)
            result=check(root,index)
            self.assertFalse(result["passed"])
            self.assertGreater(result["failures"][0]["texel_screen_ratio"][0],1.)

    def test_fractional_screen_anchor_is_detected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); index=self.fixture(root,shift=0.25)
            self.assertFalse(check(root,index)["passed"])

    def test_truncated_capture_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); index=self.fixture(root)
            (root/"font-probe-000-vertices.bin").write_bytes(bytes(24))
            with self.assertRaisesRegex(ValueError,"length mismatch"):
                check(root,index)


if __name__=="__main__":
    unittest.main()
