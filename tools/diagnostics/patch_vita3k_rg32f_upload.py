"""Generate the local Vita3K RG32F external-upload-format correction.

The reviewed input maps floating F32F32 to GL_RG_INTEGER while its internal
format is GL_RG32F and transfer type is GL_FLOAT. Only this mapping changes;
U32U32 remains integer. No third-party source or executable is distributed.
"""
import argparse
import hashlib
import json
from pathlib import Path

OLD = '''    case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16:
        return GL_RG;
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32F32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U32U32:
        return GL_RG_INTEGER;'''
NEW = '''    case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32F32:
        return GL_RG;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U32U32:
        return GL_RG_INTEGER;'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--expected-sha256', required=True)
    args = parser.parse_args()
    data = args.source.read_bytes()
    before = hashlib.sha256(data).hexdigest()
    if before != args.expected_sha256:
        raise ValueError('reviewed texture_formats.cpp identity mismatch')
    source = data.decode('utf-8').replace('\r\n', '\n')
    if source.count(OLD) != 1:
        raise ValueError('reviewed RG32F upload mapping anchor missing or ambiguous')
    result = source.replace(OLD, NEW).encode()
    with args.output.open('xb') as output:
        output.write(result)
    print(json.dumps({'source_sha256': before,
                      'output_sha256': hashlib.sha256(result).hexdigest(),
                      'change': 'F32F32 external format GL_RG_INTEGER -> GL_RG; U32U32 unchanged'}))


if __name__ == '__main__':
    main()
