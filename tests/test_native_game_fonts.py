import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools/assets"))
from build_pixel_fonts import Gfd
from native_font_strikes import FontStrike, NativeGlyph
from native_game_fonts import NativeGameFonts, MAGIC
from PIL import Image


class CatalogueTest(unittest.TestCase):
    def test_layout_is_original_and_strikes_are_original_bytes(self):
        # A synthetic GFD with an ink glyph and an empty control character.
        header=bytearray(65)
        header[:8]=b"GFD\0\x06\x0f\x01\0"
        struct.pack_into("<4I",header,20,24,1,2,0)
        struct.pack_into("<I",header,56,4)
        header[60:65]=b"test\0"
        original=[(65,0,9|15<<12,11|21<<12,0xffff0201),
                  (0xe810,0,0,0,0xffff0000)]
        gfd=Gfd(bytes(header),"test",1,original)
        small=NativeGlyph(65,3,2,4,-1,1,b"\xff\0\xff\0\xff\0")
        large=NativeGlyph(65,5,3,6,0,2,bytes([255])*15)
        fonts=[FontStrike(12,{65:small},"small",10),FontStrike(16,{65:large},"large",14)]
        with patch("native_game_fonts.load_font",side_effect=fonts):
            converter=NativeGameFonts(["small.bdf","large.hex"])
        source_header=type("Header",(),dict(width=32,height=32,payload_size=4096))()
        # Only the real TEX encoder is replaced: this test checks the GFD
        # catalogue and the exact masks supplied to that encoder.
        masks=[]
        def encode(template,mask):
            masks.append(mask.copy())
            return mask.tobytes()
        with patch("build_pixel_fonts.alpha_page",return_value=(Image.new("L",(32,32)),source_header)), \
             patch("build_pixel_fonts.make_tex",side_effect=encode):
            data,pages,summary,_=converter.convert(gfd,[b"RGBA-template"],None)
        parsed=Gfd.parse(data)
        for old,new in zip(original,parsed.glyphs):
            self.assertEqual(old[0],new[0])
            self.assertEqual(old[2:],new[2:])
        secondary=parsed.glyphs[2:]
        self.assertEqual(secondary[0],(MAGIC,1,2,2,0))
        for record,glyph,strike in zip(secondary[1:3],[small,large],fonts):
            page=record[1]&255;x=(record[1]>>8)&4095;y=record[1]>>20
            self.assertEqual(masks[page].crop((x,y,x+glyph.width,y+glyph.height)).tobytes(),glyph.pixels)
            self.assertEqual(record[4]>>16,strike.ascent)
        self.assertEqual(secondary[3][2],0)
        self.assertEqual(secondary[4][2],0)
        self.assertEqual(summary["native_strikes"],[12,16])


if __name__=="__main__":
    unittest.main()
