"""The lossless layout must retain offsets, ordinary sampling and exact sources."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/shader'))
from vertex_rg32f import lower_cg
from shader_ir import lower, emit_cg


class VertexRg32fTests(unittest.TestCase):
    def test_preserve_existing_cg_math_and_offset(self):
        source = ('void main(uniform sampler2D tex0, uniform float2 tex0_inv_size) {\n'
                  'float4 a=tex2Dlod(tex0,float4(uv+float2(int2(2,0))*'
                  'tex0_inv_size,0.0,0.));\n outp=dot(a,b);}')
        result, samplers = lower_cg(source)
        self.assertEqual(samplers, ['tex0'])
        self.assertIn('float4(uv+float2(int2(2,0))*tex0_inv_size,0.0,0.)', result)
        self.assertIn('outp=dot(a,b);', result)
        self.assertEqual(result.count('uniform float2 tex0_inv_size'), 1)
        self.assertIn('uniform float tex0_rgba32f_packed', result)
        with self.assertRaises(ValueError):
            lower_cg(result)

    def test_vertex_only_and_plain_lod_needs_logical_extent(self):
        source = ('#version 460\nlayout(binding=0) uniform sampler2D tex0;\n'
                  'void main(){gl_Position=textureLod(tex0,vec2(0.5),0.0);}')
        cg, resources = emit_cg(lower(source, 'vertex'))
        self.assertIn('uniform float2 tex0_inv_size', cg)
        self.assertEqual(resources[0]['rgba32f_packed'], 1)
        fragment = source.replace('gl_Position', 'color').replace(
            'void main()', 'layout(location=0) out vec4 color; void main()')
        cg, _ = emit_cg(lower(fragment, 'fragment'))
        self.assertNotIn('vp_rg32f_', cg)
        self.assertNotIn('_rgba32f_packed', cg)

    def test_unmeasured_sample_is_refused(self):
        with self.assertRaises(ValueError):
            lower_cg('void main(uniform sampler2D tex0) {a=tex2D(tex0,uv);}')
        original = 'void main(uniform samplerCube tex0) {a=texCUBElod(tex0,c);}'
        self.assertEqual(lower_cg(original), (original, []))


if __name__ == '__main__':
    unittest.main()
