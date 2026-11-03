"""Generate a local Vita3K GLSL vertex-dot patch without vendoring Vita3K.

The input is the reviewed B61 compile_program.cpp used by the compatible local
runtime. Build the generated file in a separate external build directory. This
does not patch an executable, install a runtime, or establish hardware behavior.
"""

import argparse
import hashlib
import json
from pathlib import Path


HELPER = r'''
// Keep vertex dot accumulation identical across separately compiled passes.
static std::string vp_order_vertex_dots(std::string source) {
    if (source.find("dot(") == std::string::npos) return source;
    std::size_t at = 0;
    while ((at = source.find("dot(", at)) != std::string::npos) {
        source.replace(at, 4, "vp_ordered_dot(");
        at += 15;
    }
    const std::string helper = R"glsl(
float vp_ordered_dot(vec2 a, vec2 b) {
    precise float p0=a.x*b.x; precise float p1=a.y*b.y;
    precise float sum=p0; sum=sum+p1; return sum;
}
float vp_ordered_dot(vec3 a, vec3 b) {
    precise float p0=a.x*b.x; precise float p1=a.y*b.y; precise float p2=a.z*b.z;
    precise float sum=p0; sum=sum+p1; sum=sum+p2; return sum;
}
float vp_ordered_dot(vec4 a, vec4 b) {
    precise float p0=a.x*b.x; precise float p1=a.y*b.y;
    precise float p2=a.z*b.z; precise float p3=a.w*b.w;
    precise float sum=p0; sum=sum+p1; sum=sum+p2; sum=sum+p3; return sum;
}
)glsl";
    at = source.find("void secondary_program");
    if (at == std::string::npos) at = source.find("void main");
    if (at == std::string::npos) throw std::runtime_error("Vertex dot patch has no function anchor");
    source.insert(at, helper);
    return source;
}
'''


def patch(source):
    if "vp_order_vertex_dots" in source:
        raise ValueError("Input already contains the vertex-dot patch")
    replacements = [
        ("#include <vector>", "#include <vector>\n#include <stdexcept>", 1),
        ("namespace renderer::gl {\n", "namespace renderer::gl {\n" + HELPER.lstrip("\n"), 1),
        ("static SharedGLObject compile_glsl(GLenum type, const std::string &source) {",
         "static SharedGLObject compile_glsl(GLenum type, const std::string &incoming) {\n"
         "    const std::string source = type == GL_VERTEX_SHADER ? "
         "vp_order_vertex_dots(incoming) : incoming;", 1),
        ('"-vpmad1"', '"-vpmad1-vpdot1"', 2),
    ]
    for old, new, count in replacements:
        if source.count(old) != count:
            raise ValueError(f"Reviewed source anchor changed: {old}")
        source = source.replace(old, new)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, help="New generated .cpp file in an external build directory")
    args = parser.parse_args()
    original = args.source.read_bytes()
    source = original.decode("utf-8-sig").replace("\r\n", "\n")
    result = patch(source).encode("utf-8")
    with args.output.open("xb") as handle:
        handle.write(result)
    print(json.dumps({
        "source": str(args.source.resolve()),
        "source_sha256": hashlib.sha256(original).hexdigest(),
        "output": str(args.output.resolve()),
        "output_sha256": hashlib.sha256(result).hexdigest(),
        "scope": "GLSL vertex compilation; cold and warm paths; fragment and SPIR-V unchanged",
        "cache_suffix": "-vpmad1-vpdot1",
    }, indent=2))


if __name__ == "__main__":
    main()
