"""VP shader IR v2: a typed GLSL -> Cg lowering bounded by the measured Cg profile.

v1 accepted straight-line float/vector code only. v2 adds the constructs the
remaining locked 1.4.0 pipelines need, and nothing more:

  * types int/uint/bool and their vectors
  * comparison, logical, bitwise, shift and ternary operators
  * nested if/else and early return
  * the intrinsics fma/abs/max/min/floor/log2/exp2/sqrt/inversesqrt/isnan and the texture family
  * samplerCube, gl_FrontFacing and gl_FragDepth

The bound is empirical, not stylistic: tools/shader/probe_cg_capability.py
compiles every construct against the locked Shacc service, and the profile
rejects unsigned types, fma/mad and any float->bits reinterpretation. Lowerings
that cannot be exact are recorded as notes and are never silent; see
docs/VP01_6_2_SHADER_COVERAGE.md.
"""
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import re

import shadow_helpers

class Unsupported(ValueError): pass

# GLSL type -> (kind, components). Kinds: float, int, uint, bool, sampler.
TYPES={'float':('float',1),'vec2':('float',2),'vec3':('float',3),'vec4':('float',4),
       'int':('int',1),'ivec2':('int',2),'ivec3':('int',3),'ivec4':('int',4),
       'uint':('uint',1),'uvec2':('uint',2),'uvec3':('uint',3),'uvec4':('uint',4),
       'bool':('bool',1),'bvec2':('bool',2),'bvec3':('bool',3),'bvec4':('bool',4),
       'sampler1D':('sampler',0),'sampler2D':('sampler',0),'samplerCube':('sampler',0),
       'sampler2DShadow':('sampler',0)}
SAMPLER_TYPES={'sampler1D','sampler2D','samplerCube','sampler2DShadow'}
# The shadow sampler is bound as an ordinary sampler2D over the real depth
# allocation; the emitted helper performs compare/filter itself.
SHADOW_SAMPLER_TYPES={'sampler2DShadow'}
UNSUPPORTED_SHADOW_TYPES={'samplerCubeShadow','sampler2DArrayShadow',
                          'sampler3DShadow','samplerCubeArrayShadow'}
# The Cg profile has no unsigned type, so uint is carried as a 32-bit signed
# pattern. Equality, &, |, ^, ~ and the bit pattern itself are unaffected; the
# operations that are not are rejected below.
CG={'float':'float','vec2':'float2','vec3':'float3','vec4':'float4',
    'int':'int','ivec2':'int2','ivec3':'int3','ivec4':'int4',
    'uint':'int','uvec2':'int2','uvec3':'int3','uvec4':'int4',
    'bool':'bool','sampler1D':'sampler1D','sampler2D':'sampler2D',
    'samplerCube':'samplerCUBE','sampler2DShadow':'sampler2D'}
SCALAR={'float':'float','int':'int','uint':'uint','bool':'bool'}

LEX=re.compile(r'\s+|//[^\n]*|/\*.*?\*/'
               r'|0[xX][0-9a-fA-F]+[uU]?|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[fFuU]?'
               r'|[A-Za-z_]\w*'
               r'|<<|>>|<=|>=|==|!=|&&|\|\|'
               r'|[{}()\[\];,.=+*/%&|^~!<>?:-]',re.S)

DIRECTIVES={'#version 460','#extension GL_ARB_separate_shader_objects : enable',
    '#define ftoi floatBitsToInt','#define ftou floatBitsToUint',
    '#define itof intBitsToFloat','#define utof uintBitsToFloat'}
# A call is legal only if it is a constructor, one of these, or a samplervfn.
BUILTINS={'fma':3,'abs':1,'max':2,'min':2,'floor':1,'ceil':1,'trunc':1,
          'cos':1,'sin':1,'log2':1,'exp2':1,
          'sqrt':1,'inversesqrt':1,'isnan':1,'frac':1}
TEXTURE={'texture':2,'textureLod':3,'textureLodOffset':4,'textureOffset':3}
# kind -> the Cg spelling of a bits-reinterpretation from that kind.
BITS_TO_FLOAT={'uint':'intBitsToFloat','int':'intBitsToFloat'}
BITS_FROM_FLOAT={}  # measured empty: the PSP2 Cg profile has no float->bits form
# Constructor kind changes the profile carries with the same meaning as GLSL. Each
# one was measured against the locked compiler; none is a reinterpretation of value.
# int->uint only relabels bits the signed carrier already holds, so it stays exact.
CONVERSIONS={
    ('float','int'):'int-to-float: the target must round like the source language',
    ('uint','int'):'int-to-uint: the signed carrier already holds the same 32 bits',
    ('int','uint'):'int-to-uint: GLSL int(uint) preserves the carrier bit pattern',
    ('bool','int'):'int-to-bool: the target must test nonzero like the source language',
}

def tokens(source):
    lines=[]
    for line in source.splitlines():
        if line.startswith('#'):
            if line not in DIRECTIVES: raise Unsupported('unknown GLSL directive: '+line)
        else: lines.append(line)
    text='\n'.join(lines); result=[]; pos=0
    for m in LEX.finditer(text):
        if m.start()!=pos: raise Unsupported('unrecognized GLSL syntax at '+text[pos:pos+32])
        pos=m.end(); t=m.group()
        if not t.isspace() and not t.startswith(('//','/*')): result.append(t)
    if pos!=len(text): raise Unsupported('unrecognized trailing syntax')
    return result+['<EOF>']

class Parser:
    def __init__(self,source,stage):
        if stage not in ('vertex','fragment'): raise Unsupported('only vertex/fragment stages supported')
        self.ts=tokens(source);self.pos=0;self.stage=stage;self.symbols={};self.scopes=[[]]
        self.reads=set();self.writes=set();self.indices={};self.precise=[];self.notes=[]
        self.initialized={'gl_Position':0,'gl_FragCoord':15,'gl_FragDepth':0}
        self.symbols['gl_Position']={'type':'vec4','storage':'builtin_out'}
        self.symbols['gl_FragCoord']={'type':'vec4','storage':'builtin_in'}
        self.symbols['gl_FragDepth']={'type':'float','storage':'builtin_out'}
        self.symbols['gl_FrontFacing']={'type':'bool','storage':'builtin_in'}
        self.initialized['gl_FrontFacing']=1
    # --- token helpers ---
    def peek(self,n=0):return self.ts[self.pos+n]
    def take(self,want=None):
        t=self.peek()
        if want is not None and t!=want:raise Unsupported(f'expected {want}, found {t}')
        self.pos+=1;return t
    def ident(self):
        t=self.take()
        if not re.fullmatch(r'[A-Za-z_]\w*',t):raise Unsupported('identifier expected')
        return t
    def number(self):
        t=self.take()
        if not re.fullmatch(r'\d+',t): raise Unsupported('static integer index/location required')
        return int(t)
    def statements(self):return self.scopes[-1]
    # --- symbol table ---
    def add(self,name,ty,storage,**kwargs):
        if name in self.symbols:raise Unsupported('duplicate variable '+name)
        if ty not in TYPES:raise Unsupported('unsupported variable '+name)
        if 'location' in kwargs and any(s['storage']==storage and s.get('location')==kwargs['location']
                                        for s in self.symbols.values()):
            raise Unsupported('duplicate input/output location')
        self.symbols[name]={'type':ty,'storage':storage,**kwargs}
        self.initialized[name]=(1<<TYPES[ty][1])-1 if storage in ('in','cbuf','sampler') else 0
    def declare_global(self):
        self.take('layout');self.take('(');quals={}
        while self.peek()!=')':
            key=self.ident();value=True
            if self.peek()=='=':self.take();value=self.number()
            if key in quals:raise Unsupported('duplicate layout qualifier')
            quals[key]=value
            if self.peek()!=')':self.take(',')
        self.take(')');storage=self.take()
        if storage in ('smooth','flat','noperspective','centroid','sample'):
            # The locked PSP2 Cg profiles reject every interpolation modifier
            # measured here; dropping one would silently change interpolation.
            raise Unsupported('interpolation qualifier '+storage+' is not in the measured PSP2 Cg profile')
        if storage not in ('in','out','uniform'):raise Unsupported('unsupported global storage')
        ty=self.ident()
        if storage=='uniform' and self.peek()=='{':
            if set(quals)!={'std140','binding'}:raise Unsupported('unsupported uniform layout')
            self.take('{');self.take('vec4');name=self.ident();self.take('[');size=self.number()
            self.take(']');self.take(';');self.take('}');self.take(';')
            if not re.fullmatch(r'(?:vs|fs)_cbuf\d+',name) or not 1<=size<=4096:raise Unsupported('unsupported cbuf')
            self.add(name,'vec4','cbuf',size=size,binding=quals['binding']);return
        name=self.ident()
        if storage=='uniform':
            if ty in UNSUPPORTED_SHADOW_TYPES:
                raise Unsupported('only sampler2DShadow shadow sampling is measured; got '+ty)
            if ty in SAMPLER_TYPES:
                self.take(';')
                if set(quals)!={'binding'}:raise Unsupported('only bound sampler uniforms supported')
                self.add(name,ty,'sampler',binding=quals['binding'])
                return
            # A flat non-sampler uniform is how the Maxwell frontend exposes the
            # individual pieces of driver state a shader reads directly (a viewport
            # rectangle, an inverse size, and so on). Cg has no scalar-uniform form
            # distinct from the constant register file, so it becomes a cbuf: one
            # vector, or N when the declaration carries an explicit [N].
            if ty!='vec4':raise Unsupported('only vec4 and sampler uniforms supported')
            size=1
            if self.peek()=='[':
                self.take('[');size=self.number();self.take(']')
                if not 1<=size<=4096:raise Unsupported('uniform array size out of range')
            self.take(';')
            self.add(name,'vec4','cbuf',size=size,required_vectors=size)
            return
        self.take(';')
        if ty!='vec4' or set(quals)!={'location'} or not 0<=quals['location']<16:
            raise Unsupported('unsupported varying declaration')
        self.add(name,ty,storage,location=quals['location'])
    def parse(self):
        while self.peek()!='void':
            if self.peek()=='out':
                for t in ('out','gl_PerVertex','{','vec4','gl_Position',';','}',';'):self.take(t)
                if self.stage!='vertex':raise Unsupported('vertex builtin in fragment stage')
                continue
            self.declare_global()
        for t in ('void','main','(',')','{'):self.take(t)
        self.block(open_brace=False)
        self.take('}');self.take('<EOF>')
        if self.stage=='vertex' and 'gl_Position' not in self.writes:raise Unsupported('vertex has no position output')
        if self.stage=='fragment':
            written=[n for n in self.writes if self.symbols[n]['storage']=='out']
            if not written or any(self.symbols[n]['location']!=0 for n in written):
                raise Unsupported('only COLOR0 fragment output supported')
        for n in self.writes:
            if self.symbols[n]['storage'] in ('in','sampler','cbuf','builtin_in'):raise Unsupported('write to readonly resource')
            if self.symbols[n]['storage'] in ('out','builtin_out') and self.initialized[n]!=(1<<TYPES[self.symbols[n]['type']][1])-1:
                raise Unsupported('partially initialized output '+n)
        if self.stage=='vertex' and 'gl_FragCoord' in self.reads:raise Unsupported('fragment builtin in vertex stage')
        if self.stage=='vertex' and 'gl_FrontFacing' in self.reads:raise Unsupported('fragment builtin in vertex stage')
        if self.stage=='vertex' and 'gl_FragDepth' in self.writes:raise Unsupported('fragment builtin in vertex stage')
        if self.stage=='fragment' and 'gl_Position' in self.reads|self.writes:raise Unsupported('vertex builtin in fragment stage')
        for name,used in self.indices.items():
            self.symbols[name]['required_vectors']=max(used)+1
        return {'schema':'vp-shader-ir-2','stage':self.stage,'symbols':self.symbols,'statements':self.statements(),
                'reads':sorted(self.reads),'writes':sorted(self.writes),'precise_variables':self.precise,
                'lowering_notes':sorted(set(self.notes)),
                'source_glsl_sha256':hashlib.sha256(self.source.encode()).hexdigest()}
    def block(self,open_brace=True):
        """Parse statements until the closing brace; returns the nested statement list."""
        if open_brace:self.take('{')
        here=self.scopes[-1]
        while self.peek()!='}':
            if self.peek()=='return':
                self.take();self.take(';');here.append({'op':'return'});continue
            if self.peek()=='break':
                self.take();self.take(';');here.append({'op':'break'});continue
            if self.peek()=='continue':
                self.take();self.take(';');here.append({'op':'continue'});continue
            if self.peek()=='discard':
                if self.stage!='fragment':raise Unsupported('discard outside fragment stage')
                self.take();self.take(';');here.append({'op':'discard'});continue
            if self.peek()=='for':
                here.append(self.for_statement());continue
            if self.peek()=='if':
                here.append(self.if_statement());continue
            precise=False
            if self.peek()=='precise':self.take();precise=True
            if self.peek() in TYPES and self.peek(1)!='.':
                ty=self.take();name=self.ident()
                if ty in SAMPLER_TYPES:raise Unsupported('local sampler unsupported')
                self.add(name,ty,'local');self.take('=');expr=self.expr();self.take(';')
                here.append({'op':'declare','name':name,'type':ty,'precise':precise,'value':self.compat(ty,expr)})
                self.initialized[name]=(1<<TYPES[ty][1])-1
                if precise:self.precise.append(name)
            else:
                if precise:raise Unsupported('precise must qualify a declaration')
                target=self.reference(write=True);self.take('=');value=self.expr();self.take(';')
                here.append({'op':'assign','target':target,'value':self.compat(target['type'],value)})
                base=target['base'] if target['op']=='swizzle' else target
                if base['op']!='ref':raise Unsupported('write through array/unsupported lvalue')
                mask=sum(1<<'xyzw'.index(c) for c in target['fields']) if target['op']=='swizzle' else (1<<TYPES[target['type']][1])-1
                self.initialized[base['name']]|=mask
        if open_brace:self.take('}')
        return here
    def for_statement(self):
        """Parse `for(init; cond; step) { body }` and keep it as a loop node.

        The Maxwell frontend emits `for(;;)` with an internal `break`, so empty
        clauses are the common case. The initializer is hoisted out of the loop by
        the Cg emitter, which keeps its declaration in the enclosing scope; that is
        why only declarations and plain assignments are accepted there.

        Statements after a loop see only what is initialized on EVERY path, since a
        for may run zero times: the merged mask is the intersection of the state
        before the loop and the state after its body.
        """
        self.take('for');self.take('(')
        before=dict(self.initialized)
        init=[]
        if self.peek()!=';':
            if self.peek() in TYPES and self.peek(1)!='.':
                ty=self.take();name=self.ident()
                if ty in SAMPLER_TYPES:raise Unsupported('local sampler unsupported')
                self.add(name,ty,'local');self.take('=');value=self.expr()
                init.append({'op':'declare','name':name,'type':ty,'precise':False,
                             'value':self.compat(ty,value)})
                self.initialized[name]=(1<<TYPES[ty][1])-1
            else:
                target=self.reference(write=True);self.take('=');value=self.expr()
                init.append({'op':'assign','target':target,
                             'value':self.compat(target['type'],value)})
        self.take(';')
        cond=None
        if self.peek()!=';':
            cond=self.expr()
            if cond['type']!='bool':raise Unsupported('for condition must be a scalar bool')
        self.take(';')
        step=[]
        if self.peek()!=')':
            target=self.reference(write=True);self.take('=');value=self.expr()
            step.append({'op':'assign','target':target,
                         'value':self.compat(target['type'],value)})
        self.take(')')
        self.scopes.append([])
        body=self.block()
        after=dict(self.initialized)
        self.scopes.pop()
        self.initialized={n:before.get(n,0)&after.get(n,0) for n in after}
        return {'op':'for','init':init,'condition':cond,'step':step,'body':body}
    def if_statement(self):
        self.take('if');self.take('(');cond=self.expr();self.take(')')
        if cond['type']!='bool':raise Unsupported('if condition must be a scalar bool')
        before=dict(self.initialized);self.scopes.append([])
        taken=self.block();scopes_then=dict(self.initialized)
        self.scopes.pop()
        node={'op':'if','condition':cond,'then':taken}
        if self.peek()=='else':
            self.take('else');after_then=dict(scopes_then)
            self.scopes.append([])
            other=self.block(open_brace=self.peek()=='{') if self.peek()!='if' else [self.if_statement()]
            scopes_else=dict(self.initialized)
            self.scopes.pop()
            node['else']=other
            after_else=scopes_else
        else:
            after_else=before
        merged={}
        for name in self.initialized:merged[name]=scopes_then.get(name,0)&after_else.get(name,0)
        self.initialized=merged
        return node
    def compat(self,ty,expr):
        """Return the expression to store, or reject the coercion.

        GLSL converts a signed integer expression implicitly where a float is
        expected; Cg performs the identical numeric conversion, so it is kept as
        an explicit node rather than dropped.
        """
        if ty==expr['type']:return expr
        ka,na=TYPES[ty];kb,nb=TYPES[expr['type']]
        if (ka,kb) in CONVERSIONS and na==nb:
            self.notes.append('implicit '+CONVERSIONS[ka,kb])
            return {'op':'convert','arg':expr,'type':ty}
        raise Unsupported(f'type mismatch {ty} <- {expr["type"]}')
    def reference(self,write=False):
        name=self.ident()
        if name not in self.symbols:raise Unsupported('unknown variable '+name)
        s=self.symbols[name];node={'op':'ref','name':name,'type':s['type']}
        (self.writes if write else self.reads).add(name)
        if s['storage']=='cbuf':
            # A flat uniform declaration names a single vector, so the frontend
            # writes it bare while the lowered symbol is a one-vector cbuf. Only an
            # explicit `[n]` is treated as indexing; otherwise the index is 0.
            index=0
            if self.peek()=='[':
                self.take('[');index=self.number();self.take(']')
            if index>=s['size']:raise Unsupported('constant-buffer index out of range')
            self.indices.setdefault(name,set()).add(index);node={'op':'index','base':node,'index':index,'type':'vec4'}
        if self.peek()=='.':
            self.take();swizzle=self.ident()
            if not re.fullmatch('[xyzw]{1,4}',swizzle) or TYPES[node['type']][1]<max('xyzw'.index(c)+1 for c in swizzle):
                raise Unsupported('invalid swizzle')
            if write and len(set(swizzle))!=len(swizzle):raise Unsupported('duplicate write swizzle')
            node={'op':'swizzle','base':node,'fields':swizzle,
                  'type':swizzle_kind(node['type'],swizzle)}
        if not write and s['storage'] not in ('cbuf','sampler'):
            mask=sum(1<<'xyzw'.index(c) for c in node['fields']) if node['op']=='swizzle' else (1<<TYPES[node['type']][1])-1
            if mask & self.initialized[name]!=mask:raise Unsupported('read before initialization '+name)
        return node
    # --- expressions ---
    UNARY={'!','~','-'}
    # Lowest precedence first; index 0 is the loosest binding level.
    LEVELS=[('||',),('&&',),('|',),('^',),('&',),('==','!='),('<','>','<=','>='),
            ('<<','>>'),('+','-'),('*','/','%')]
    def expr(self):return self.ternary()
    def ternary(self):
        cond=self.binary(0)
        if self.peek()!='?':return cond
        if cond['type']!='bool':raise Unsupported('ternary condition must be a scalar bool')
        self.take('?');a=self.ternary();self.take(':');b=self.ternary()
        if a['type']!=b['type']:raise Unsupported(f'ternary branch mismatch {a["type"]} : {b["type"]}')
        return {'op':'select','condition':cond,'then':a,'other':b,'type':a['type']}
    def binary(self,level):
        if level==len(self.LEVELS):return self.unary()
        node=self.binary(level+1)
        while self.peek() in self.LEVELS[level]:
            op=self.take();rhs=self.binary(level+1)
            node={'op':'binary','operator':op,'left':node,'right':rhs,'type':self.binary_type(op,node,rhs)}
        return node
    def binary_type(self,op,a,b):
        ka,na=TYPES[a['type']];kb,nb=TYPES[b['type']]
        if op in ('&&','||'):
            if a['type']!='bool' or b['type']!='bool':raise Unsupported(f'logical {op} needs scalar bool operands')
            return 'bool'
        if op in ('<','>','<=','>='):
            if ka=='bool' or kb=='bool':raise Unsupported(f'relational {op} on bool')
            if ka!=kb:raise Unsupported(f'relational {op} type mismatch {a["type"]} {b["type"]}')
            if ka=='uint' and a['type']=='uint':
                raise Unsupported('relational comparison of uint is not order-preserving in the signed Cg carrying type')
            if na!=nb:raise Unsupported(f'relational {op} needs matching widths')
            return 'bool' if na==1 else 'bvec%d'%na
        if op in ('==','!='):
            if ka!=kb:
                if {ka,kb}=={'uint','int'} and (a['op']=='literal' or b['op']=='literal'):
                    # GLSL accepts an integer literal where the other operand is
                    # uint; the literal is a bit value, resolved during lowering.
                    width=max(na,nb)
                    return 'bool' if width==1 else 'bvec%d'%width
                raise Unsupported(f'equality {op} type mismatch {a["type"]} {b["type"]}')
            return 'bool' if na==1 else 'bvec%d'%na
        if op in ('&','^','|'):
            if ka not in ('int','uint') or kb!=ka:raise Unsupported(f'bitwise {op} needs matching integer operands')
            if na!=nb and 1 not in (na,nb):raise Unsupported(f'bitwise {op} width mismatch')
            return a['type'] if na>1 else b['type']
        if op in ('<<','>>'):
            if ka not in ('int','uint') or kb not in ('int','uint'):raise Unsupported(f'shift {op} needs integer operands')
            if nb!=1:raise Unsupported(f'shift {op} amount must be scalar')
            if ka=='uint' and na!=1:raise Unsupported('vector uint shifts are not measured')
            if b['op']=='literal' and not 0<=int(b['value'].rstrip('uU'),0)<32:
                raise Unsupported('shift count outside defined GLSL range 0..31')
            return a['type']
        if op=='%':
            raise Unsupported('integer remainder: the sce_fp_psp2 profile rejects arbitrary integer divide')
        if ka in ('bool','sampler') or kb in ('bool','sampler'):raise Unsupported(f'arithmetic {op} on {a["type"]} {b["type"]}')
        if ka!=kb:raise Unsupported(f'arithmetic {op} cannot mix {a["type"]} and {b["type"]}')
        # GLSL broadcasts a scalar over the other operand's components.
        if na!=nb and na!=1 and nb!=1:raise Unsupported(f'arithmetic {op} needs matching widths: {a["type"]} {b["type"]}')
        if op=='/' and ka in ('int','uint'):
            raise Unsupported('integer divide: the sce_fp_psp2 profile rejects arbitrary integer divide')
        return a['type'] if na>1 else b['type']
    def unary(self):
        t=self.peek()
        if t in self.UNARY:
            self.take();arg=self.unary();k=TYPES[arg['type']][0]
            if t=='!' and arg['type']!='bool':raise Unsupported('logical not needs a bool operand')
            if t=='~' and k not in ('int','uint'):raise Unsupported('bitwise not needs an integer operand')
            if t=='-' and k in ('bool','sampler'):raise Unsupported('negate needs a numeric operand')
            return {'op':{'!':'logic_not','~':'bit_not','-':'negate'}[t],'arg':arg,'type':arg['type']}
        return self.postfix()
    def postfix(self):
        node=self.primary()
        if self.peek()=='(':
            raise Unsupported('calling a non-function value is unsupported')
        return node
    def primary(self):
        t=self.peek()
        if t=='(':
            self.take();node=self.expr();self.take(')');return node
        # Boolean literals appear in generated code as the loop guard's `b_0=true;`.
        if t in ('true','false'):
            self.take();return {'op':'literal','type':'bool','value':(t=='true')}
        if re.fullmatch(r'0[xX][0-9a-fA-F]+[uU]?|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[fFuU]?',t):
            self.take();return literal_node(t)
        if self.peek(1)=='(':
            return self.call()
        return self.reference()
    def call(self):
        func=self.ident();self.take('(');args=[]
        while self.peek()!=')':
            args.append(self.expr())
            if self.peek()!=')':self.take(',')
        self.take(')')
        return self.make_call(func,args)
    def make_call(self,func,args):
        if func in ('bitfieldExtract','bitfieldInsert'):
            expected=3 if func=='bitfieldExtract' else 4
            if len(args)!=expected or args[0]['type'] not in ('int','uint'):
                raise Unsupported(func+' requires a scalar 32-bit integer')
            if func=='bitfieldInsert' and args[1]['type']!=args[0]['type']:
                raise Unsupported('bitfieldInsert requires matching base/insert types')
            def constant(node):
                while node['op']=='call' and node['function'] in ('int','uint') and len(node['args'])==1:
                    node=node['args'][0]
                if node['op']!='literal' or node['type'] not in ('int','uint'):
                    raise Unsupported(func+' requires constant offset/count')
                return int(node['value'].rstrip('uU'),0)
            offset,bits=map(constant,args[-2:])
            if offset<0 or bits<0 or offset+bits>32:
                raise Unsupported(func+' has undefined offset/count')
            if bits==0 and func=='bitfieldExtract':return literal_node('0u' if args[0]['type']=='uint' else '0')
            name=('bitfield_extract_' + ('u' if args[0]['type']=='uint' else 's')
                  if func=='bitfieldExtract' else 'bitfield_insert')
            self.notes.append(func+' uses integer shifts and masks; float-pattern carriers remain rejected')
            return {'op':'call','function':name,'args':args,'type':args[0]['type']}
        if func in TYPES and TYPES[func][0]!='sampler':
            if not args or any(TYPES[a['type']][0]=='sampler' for a in args):raise Unsupported('invalid constructor')
            kind=TYPES[func][0]
            # The measured target conversion rounds toward negative infinity, while
            # GLSL truncates. trunc() first makes int() see an integral value, so the
            # two agree for every value in range; a bare int(x) would differ on every
            # negative fraction. Only the scalar form occurs in the locked library.
            if len(args)==1 and kind=='int' and TYPES[args[0]['type']][0]=='float':
                if TYPES[func][1]!=1:raise Unsupported('vector float-to-int conversion is not measured')
                self.notes.append('float-to-int conversion lowered as int(trunc(x)); the target int() rounds toward negative infinity')
                return {'op':'convert','arg':{'op':'call','function':'trunc','args':[args[0]],'type':'float'},'type':'int'}
            # uint has no Cg type. The truncating value in [0, 2^32) is split at 2^31
            # so the signed carrier can hold the result without overflowing int.
            if len(args)==1 and kind=='uint' and TYPES[args[0]['type']][0]=='float':
                if TYPES[func][1]!=1:raise Unsupported('vector float-to-uint conversion is not measured')
                self.notes.append('float-to-uint conversion lowered as trunc then a 2^31 sign split; exact on [0, 2^32)')
                return {'op':'call','function':'uint_of_float','args':[args[0]],'type':'uint'}
            for arg in args:
                arg_kind=TYPES[arg['type']][0]
                # Only the int-source conversions Cg carries with the same meaning are
                # allowed; every other kind change is a reinterpretation or has no PSP2
                # form at all, so it is refused instead of assumed.
                if arg_kind==kind:
                    continue
                if kind=='float' and arg_kind=='uint':
                    self.notes.append('uint-to-float uses a signed-range correction so all 32-bit values round as unsigned')
                    continue
                if (kind,arg_kind) not in CONVERSIONS:
                    raise Unsupported(f'{func}({arg["type"]}): the PSP2 Cg profile has no '
                                      f'{arg_kind}-to-{kind} conversion with these semantics')
                self.notes.append(CONVERSIONS[kind,arg_kind])
            width=sum(TYPES[a['type']][1] for a in args)
            if not (width==TYPES[func][1] or len(args)==1):
                raise Unsupported('constructor component mismatch')
            return {'op':'call','function':func,'args':args,'type':func}
        if func in TEXTURE:
            types=[a['type'] for a in args]
            if types and types[0] in SHADOW_SAMPLER_TYPES:
                # The observed shadow form is only textureLod(shadow, vec3(uv,ref), 0.0).
                # Implicit texture(), offsets, non-zero and dynamic LOD are not
                # measured; treating them as a 2D LOD0 fetch would be a guess.
                if func!='textureLod':
                    raise Unsupported('sampler2DShadow only supports textureLod(...,0.0); '
                                      'got '+func)
                if len(args)!=3 or types[1]!='vec3':
                    raise Unsupported('sampler2DShadow textureLod requires vec3(uv,ref)')
                lod=args[2]
                zero=False
                if lod.get('op')=='literal' and lod.get('literal')=='float':
                    try: zero=float(str(lod['value']).rstrip('fF'))==0.0
                    except ValueError: zero=False
                if not zero:
                    raise Unsupported('sampler2DShadow requires a literal LOD of 0.0; '
                                      'implicit, non-zero and dynamic LOD are not measured')
                if args[0]['op']!='ref':
                    raise Unsupported('sampler2DShadow needs a named uniform sampler')
                sampler=args[0]['name']
                symbol=self.symbols[sampler]
                symbol['shadow']=True
                symbol['shadow_abi']=shadow_helpers.abi_for_sampler(sampler)
                note=(f'{sampler} sampler2DShadow LOD0 lowered by the statement-inline '
                      f'emitter (strategy {shadow_helpers.STRATEGY}, policy '
                      f'{shadow_helpers.POLICY}): point/linear compare and Maxwell '
                      f'wrap execute in scoped statements, native sampler '
                      f'point/clamp/no-mip/no-compare')
                if note not in self.notes:self.notes.append(note)
                return {'op':'call','function':'textureLod','args':args,'type':'float',
                        'shadow':True}
            if len(args)!=TEXTURE[func] or types[0] not in ('sampler1D','sampler2D','samplerCube'):
                raise Unsupported('invalid texture call '+func)
            if func in ('textureLodOffset','textureOffset') and types[0]!='sampler2D':
                raise Unsupported(func+' is only measured for sampler2D')
            coord_type={'sampler1D':'float','sampler2D':'vec2','samplerCube':'vec3'}[types[0]]
            if types[1]!=coord_type:raise Unsupported('texture coordinates must be '+coord_type)
            if func in ('textureLod','textureLodOffset') and types[2]!='float':
                raise Unsupported(func+' needs a scalar lod')
            if func in ('textureLodOffset','textureOffset') and types[-1] not in ('ivec2','uvec2'):
                raise Unsupported(func+' needs an integer texel offset')
            if types[0]=='samplerCube' and func in ('textureOffset','textureLodOffset'):
                raise Unsupported(func+' is not supported for samplerCube')
            if func in ('textureLodOffset','textureOffset'):
                if args[0]['op']!='ref':raise Unsupported(func+' needs a named sampler')
                sampler=args[0]['name']
                self.symbols[sampler]['inv_size']=True
                self.notes.append(f'{func} on {sampler} lowered to the equivalent UV offset using a '
                                  f'resource-supplied inverse extent uniform {sampler}_inv_size')
            return {'op':'call','function':func,'args':args,'type':'vec4'}
        if func in ('uintBitsToFloat','intBitsToFloat','utof','itof'):
            if len(args)!=1 or TYPES[args[0]['type']][0] not in ('uint','int'):raise Unsupported(func+' needs one integer argument')
            width=TYPES[args[0]['type']][1]
            return {'op':'call','function':'bits_to_float','args':args,
                    'type':'float' if width==1 else 'vec%d'%width}
        if func in ('floatBitsToUint','floatBitsToInt','ftou','ftoi'):
            if len(args)!=1 or TYPES[args[0]['type']][0]!='float':
                raise Unsupported(func+' needs one float argument')
            target='uint' if func in ('floatBitsToUint','ftou') else 'int'
            return {'op':'call','function':'bits_from_float','args':args,'type':target}
        if func in BUILTINS:
            if len(args)!=BUILTINS[func]:raise Unsupported(f'{func} takes {BUILTINS[func]} arguments')
            kinds=[TYPES[a['type']][0] for a in args]
            if func=='fma':
                if kinds!=['float']*3 or len({a['type'] for a in args})!=1:raise Unsupported('fma needs three operands of one float type')
                self.notes.append('fma lowered to (a*b+c): fused rounding is a target property, verified separately')
                return {'op':'call','function':'fma','args':args,'type':args[0]['type']}
            if func=='isnan':
                if kinds!=['float']:raise Unsupported('isnan needs one float operand')
                return {'op':'call','function':'isnan','args':args,'type':'bool'}
            if func in ('max','min'):
                if kinds[0]!=kinds[1] or kinds[0] not in ('float','int'):raise Unsupported(f'{func} needs matching numeric operands')
                if TYPES[args[0]['type']][1]!=TYPES[args[1]['type']][1]:raise Unsupported(f'{func} width mismatch')
                return {'op':'call','function':func,'args':args,'type':args[0]['type']}
            if kinds!=['float']:raise Unsupported(f'{func} needs one float operand')
            return {'op':'call','function':func,'args':args,'type':args[0]['type']}
        raise Unsupported('unsupported operation '+func)

def swizzle_kind(base,swizzle):
    kind=TYPES[base][0]
    width=len(swizzle)
    if kind=='float':return 'float' if width==1 else 'vec%d'%width
    if kind=='int':return 'int' if width==1 else 'ivec%d'%width
    if kind=='uint':return 'uint' if width==1 else 'uvec%d'%width
    if kind=='bool':return 'bool' if width==1 else 'bvec%d'%width
    raise Unsupported('cannot swizzle a sampler')

def literal_node(text):
    if re.fullmatch(r'0[xX][0-9a-fA-F]+[uU]?',text):
        return {'op':'literal','value':text,'literal':'int','type':'uint' if text[-1] in 'uU' else 'int'}
    if text[-1] in 'uU':return {'op':'literal','value':text,'literal':'uint','type':'uint'}
    if '.' in text or 'e' in text.lower() or text[-1] in 'fF':
        return {'op':'literal','value':text,'literal':'float','type':'float'}
    return {'op':'literal','value':text,'literal':'int','type':'int'}

def specialize_unsigned_attributes(ir, locations):
    """Preserve Maxwell integer vertex fetches across the float carrier GLSL.

    The offline frontend declares generic inputs as vec4. For a captured UINT
    attribute, its bits are an integer value, not the IEEE encoding of the
    numerically converted value. Use an integer GXP input and expose its bit
    carrier to the existing IR; carrier resolution cancels subsequent ftou.
    Runtime binding must check the descriptor against the GXP input type.
    """
    if not locations: return
    if ir['stage']!='vertex': raise Unsupported('integer attributes require vertex stage')
    selected={}
    for location in locations:
        names=[n for n,s in ir['symbols'].items()
               if s['storage']=='in' and s.get('location')==location]
        if len(names)!=1: raise Unsupported(f'unsigned attribute {location} is not declared')
        name=names[0];s=ir['symbols'][name]
        if s['type']!='vec4': raise Unsupported('unsigned input specialization requires vec4')
        selected[name]=name+'_uint';s['type']='uvec4'
        s['unsigned_attribute']=True
        ir['symbols'][selected[name]]=ir['symbols'].pop(name)
    ir['reads']=[selected.get(name,name) for name in ir['reads']]
    def rewrite(node):
        if isinstance(node,list): return [rewrite(n) for n in node]
        if not isinstance(node,dict): return node
        if node.get('op')=='ref' and node.get('name') in selected:
            return {'op':'call','function':'bits_to_float','type':'vec4',
                    'args':[{'op':'ref','name':selected[node['name']],'type':'uvec4'}]}
        result={k:rewrite(v) for k,v in node.items()}
        if result.get('op')=='swizzle' and result['base'].get('function')=='bits_to_float':
            carrier=result['base']['args'][0]
            width=TYPES[result['type']][1]
            return {'op':'call','function':'bits_to_float','type':result['type'],
                    'args':[{'op':'swizzle','base':carrier,'fields':result['fields'],
                             'type':'uint' if width==1 else 'uvec%d'%width}]}
        return result
    ir['statements']=rewrite(ir['statements'])
    ir['lowering_notes'].append('UINT vertex attributes specialized from captured descriptors: '+
                               ','.join(str(x) for x in sorted(locations)))

def lower(source,stage,unsigned_attributes=()):
    parser=Parser(source,stage);parser.source=source;ir=parser.parse()
    specialize_unsigned_attributes(ir,unsigned_attributes)
    resolve_uint_carriers(ir)
    return ir


# --- uint carrier resolution -------------------------------------------------
# The PSP2 Cg profile has no unsigned type and no float->bits reinterpretation,
# but intBitsToFloat exists and utof(ftou(x)) is a bit-exact identity. Every
# uint carrier therefore keeps two Cg variables: the integer, which bitwise and
# equality operations read, and a float mirror built with intBitsToFloat. That
# mirror is exact for any integer value and becomes the original float whenever
# the carrier holds a float's bit pattern. int_valid records whether the integer
# variable still holds the true pattern: a ftou assignment clears it, and only a
# later integer assignment restores it. A read that needs bits the integer
# variable no longer has is rejected, never approximated.

def uint_refs(expr):
    found=[]
    def walk(node):
        if isinstance(node,dict):
            if node.get('op')=='ref' and TYPES[node['type']][0]=='uint':found.append(node['name'])
            for value in node.values():walk(value)
        elif isinstance(node,list):
            for value in node:walk(value)
    walk(expr);return found

def float_type_of(glsl_type):
    width=TYPES[glsl_type][1]
    return 'float' if width==1 else 'vec%d'%width

def bits_to_float_call(arg):
    return {'op':'call','function':'bits_to_float','args':[arg],'type':float_type_of(arg['type'])}

def shadow_ref(name,glsl_type):
    return {'op':'ref','name':name+'__f','type':float_type_of(glsl_type)}

def carrier_ref(name,glsl_type):
    return {'op':'ref','name':name,'type':glsl_type}

def require_integer(expr,valid,what):
    missing=sorted({n for n in uint_refs(expr) if not valid.get(n,True)})
    if missing:
        raise Unsupported(f'{what} needs the bit pattern of uint carrier {",".join(missing)}, which the '
                          f'current path may hold as a float pattern; no exact PSP2 Cg form exists for that')

def record_lowering_note(shadow,note):
    shadow.setdefault('__lowering_notes',[]).append(note)

def zero_integer(expr):
    if expr['op']=='literal' and TYPES[expr['type']][0] in ('int','uint'):
        return int(expr['value'].rstrip('uU'),0)==0
    return (expr['op']=='call' and expr['function']=='int' and len(expr['args'])==1 and
            expr['args'][0]['op']=='literal' and
            TYPES[expr['args'][0]['type']][0] in ('int','uint') and
            int(expr['args'][0]['value'].rstrip('uU'),0)==0)

def to_float_expr(expr,valid,shadow,what):
    """Rewrite a uint-valued expression into an exactly equal float expression."""
    op=expr['op']
    if op=='call' and expr['function']=='bits_from_float':return expr['args'][0]
    if op=='negate' and expr['arg']['op']=='literal' and TYPES[expr['type']][0] in ('int','uint'):
        literal=expr['arg']['value'].rstrip('uU')
        folded=-int(literal,0)
        value={'op':'literal','value':str(folded),'literal':'int','type':expr['type']}
        return bits_to_float_call(value)
    if op=='literal':return bits_to_float_call(expr)
    if op=='swizzle':
        require_integer(expr,valid,what)
        return bits_to_float_call(expr)
    if op=='ref':
        name=expr['name'];shadow[name]=True
        if not valid.get(name,True):return shadow_ref(name,expr['type'])
        return bits_to_float_call(carrier_ref(name,expr['type']))
    if op=='select':
        return {'op':'select','condition':expr['condition'],
                'then':to_float_expr(expr['then'],valid,shadow,what),
                'other':to_float_expr(expr['other'],valid,shadow,what),
                'type':float_type_of(expr['type'])}
    raise Unsupported(f'{what} carries a float pattern through {op}; no exact PSP2 Cg form exists for that')

def uint_numeric_expr(expr,valid,shadow,what):
    """Return a float expression holding the unsigned value, exactly.

    For a carrier that still holds its bit pattern the existing unsigned
    conversion is exact. For one that holds a float pattern, the mirror is
    decoded as a binary32 bit field: the integer is recovered without asking
    Cg for a float-to-bits operation. Every non-NaN pattern decodes exactly;
    a NaN payload cannot be read by this profile, which is recorded as a note.
    """
    if expr['op']=='ref':
        name=expr['name'];shadow[name]=True
        if not valid.get(name,True):
            record_lowering_note(shadow,'integer equality on a float-pattern uint carrier compares the decoded binary32 value; exact for every non-NaN pattern, NaN payloads remain unreadable on this profile')
            return {'op':'call','function':'float_bits_to_uint_numeric',
                    'args':[shadow_ref(name,expr['type'])],'type':float_type_of(expr['type'])}
        return {'op':'call','function':'uint_to_float','args':[expr],
                'type':float_type_of(expr['type'])}
    if expr['op']=='literal':
        return {'op':'literal','value':str(literal_value(expr))+'.0','literal':'float','type':'float'}
    raise Unsupported(f'{what} carries a uint value through {expr["op"]}; no exact comparison form is defined for that')

def literal_value(expr):
    return int(str(expr['value']).rstrip('uU'),0)

def lower_uint_equality(expr,valid,shadow):
    """==/!= between a uint carrier and a small integer literal.

    Equality of the 32-bit patterns is the same as equality of their unsigned
    values. The literal must be exactly representable as binary32, so the
    decoded float cannot collide with a different pattern by rounding; in the
    locked library every such literal is 0..10.
    """
    sides=(expr['left'],expr['right'])
    uint_side=None
    literal=None
    for side in sides:
        if side['op']=='literal' and TYPES[side['type']][0] in ('int','uint'):
            literal=side
        elif TYPES[side['type']][0]=='uint':uint_side=side
    if uint_side is None or literal is None or uint_side['op']=='literal':return None
    value=literal_value(literal)
    if not 0<=value<1<<24:
        raise Unsupported('uint equality against '+str(value)+' is not exact: above 2^24 a different '
                          'pattern can round to the same binary32 value')
    return {'op':'binary','operator':expr['operator'],
            'left':uint_numeric_expr(uint_side,valid,shadow,'the equality'),
            'right':{'op':'literal','value':str(value)+'.0','literal':'float','type':'float'},
            'type':'bool'}

def signed_numeric_from_carrier(expr,valid,shadow,what):
    """Return float(int(bits(x))) without inventing a float-to-bits operation.

    The integer variable holds the signed numeric value when it is valid. When
    it is not, the float mirror is decoded field by field; this is exact for
    zero, subnormals, normals and infinities. The profile cannot read a NaN
    payload, so a NaN pattern yields NaN instead of its finite signed value.
    """
    if expr['op']=='ref':
        name=expr['name'];shadow[name]=True
        if not valid.get(name,True):
            record_lowering_note(shadow,'float(int(floatBitsToUint(x))) decodes the signed binary32 pattern numerically; exact for zero, subnormals, normals and infinities, NaN payloads remain unreadable on this profile')
            return {'op':'call','function':'float_bits_to_int_numeric',
                    'args':[shadow_ref(name,expr['type'])],'type':float_type_of(expr['type'])}
        return {'op':'call','function':'carrier_signed_to_float','args':[expr],
                'type':float_type_of(expr['type'])}
    if expr['op']=='literal':
        value=literal_value(expr)
        if value>0xFFFFFFFF:raise Unsupported('uint literal out of range')
        if value>=1<<31:value-=1<<32
        return {'op':'literal','value':str(value)+'.0','literal':'float','type':'float'}
    raise Unsupported(f'{what} carries a uint value through {expr["op"]}; no exact signed conversion is defined for that')

def resolve_expression(expr,valid,shadow):
    """Resolve uint reads inside an expression that needs integer values."""
    op=expr['op']
    if op=='literal':return expr
    if op=='ref':
        if TYPES[expr['type']][0]!='uint':return expr
        require_integer(expr,valid,'the use of '+expr['name'])
        return expr
    if op=='index':expr['base']=resolve_expression(expr['base'],valid,shadow);return expr
    if op=='swizzle':expr['base']=resolve_expression(expr['base'],valid,shadow);return expr
    if op=='convert':expr['arg']=resolve_expression(expr['arg'],valid,shadow);return expr
    if op in ('negate','logic_not','bit_not'):
        expr['arg']=resolve_expression(expr['arg'],valid,shadow);return expr
    if op=='binary':
        if expr['operator']=='<=' and expr['left']['op']=='call' and expr['left']['function']=='int':
            args=expr['left']['args']
            if (len(args)==1 and TYPES[args[0]['type']][0]=='uint' and args[0]['op']=='ref' and
                    not valid.get(args[0]['name'],True) and zero_integer(expr['right'])):
                source=shadow_ref(args[0]['name'],args[0]['type'])
                zero=literal_node('0.0')
                record_lowering_note(shadow,'int(floatBitsToUint(x)) <= 0 lowers to x<0 || x==0; equivalent for finite values and signed zero, signed NaN bit patterns remain unsupported')
                return {'op':'binary','operator':'||',
                        'left':{'op':'binary','operator':'<','left':source,'right':zero,'type':'bool'},
                        'right':{'op':'binary','operator':'==','left':deepcopy(source),'right':zero,'type':'bool'},
                        'type':'bool'}
        has_uint=any(TYPES[expr[side]['type']][0]=='uint' for side in ('left','right'))
        if has_uint and expr['operator'] in ('==','!='):
            lowered=lower_uint_equality(expr,valid,shadow)
            if lowered is not None:return lowered
        if has_uint and expr['operator'] in ('&','|','^','<<','>>','==','!=','<','>','<=','>='):
            require_integer(expr,valid,'the '+expr['operator']+' comparison')
        elif has_uint:
            raise Unsupported('arithmetic on uint carriers is not order-preserving in the signed Cg carrying type')
        expr['left']=resolve_expression(expr['left'],valid,shadow)
        expr['right']=resolve_expression(expr['right'],valid,shadow)
        return expr
    if op=='select':
        expr['condition']=resolve_expression(expr['condition'],valid,shadow)
        for side in ('then','other'):
            if TYPES[expr[side]['type']][0]=='uint':expr[side]=resolve_expression(expr[side],valid,shadow)
        return expr
    if op=='call':
        function=expr['function']
        if (function=='float' and len(expr['args'])==1 and
                expr['args'][0]['op']=='call' and expr['args'][0]['function']=='int' and
                len(expr['args'][0]['args'])==1 and
                TYPES[expr['args'][0]['args'][0]['type']][0]=='uint'):
            # float(int(x)) is the numeric value of x's signed 32-bit pattern,
            # not of the unsigned carrier used elsewhere. Keep that distinction.
            return signed_numeric_from_carrier(expr['args'][0]['args'][0],valid,shadow,
                                               'the float(int(...)) conversion')
        if function=='bits_from_float':
            raise Unsupported('float to bits reinterpretation is only lowered for a direct assignment to a '
                              'uint carrier; this use is not one')
        if function=='bits_to_float':
            return to_float_expr(expr['args'][0],valid,shadow,'intBitsToFloat argument')
        if function in TYPES:
            target_kind=TYPES[function][0]
            for index,arg in enumerate(expr['args']):
                if (target_kind=='float' and TYPES[arg['type']][0]=='uint' and arg['op']=='ref' and
                        not valid.get(arg['name'],True)):
                    source=shadow_ref(arg['name'],arg['type'])
                    expr['args'][index]={'op':'call','function':'float_bits_to_uint_numeric','args':[source],
                                         'type':float_type_of(arg['type'])}
                    record_lowering_note(shadow,'float(uint(floatBitsToUint(x))) uses a binary32 bit-field decoder for finite values and infinities; NaN payload/sign conversion remains unsupported')
                    continue
                arg=resolve_expression(arg,valid,shadow)
                if target_kind=='float' and TYPES[arg['type']][0]=='uint':
                    require_integer(arg,valid,'the '+function+' constructor')
                    expr['args'][index]={'op':'call','function':'uint_to_float','args':[arg],
                                         'type':float_type_of(arg['type'])}
                else:
                    expr['args'][index]=arg
            if (target_kind=='float' and len(expr['args'])==1 and
                    expr['args'][0]['op']=='call' and
                    expr['args'][0]['function']=='uint_to_float' and
                    expr['args'][0]['type']==function):
                return expr['args'][0]
            return expr
        expr['args']=[resolve_expression(arg,valid,shadow) for arg in expr['args']]
        return expr
    raise Unsupported('unknown IR expression')

def remember_float_bits(name,glsl_type,value,float_bits):
    """Track simple float temporaries that are exact intBitsToFloat results.

    This lets a later floatBitsToUint cancel the round trip without asking the
    PSP2 Cg profile for a float-to-bits operation it does not have.
    """
    if glsl_type!='float':
        float_bits.pop(name,None)
    elif value['op']=='call' and value['function']=='bits_to_float':
        float_bits[name]=deepcopy(value['args'][0])
    elif value['op']=='ref' and value['name'] in float_bits:
        float_bits[name]=deepcopy(float_bits[value['name']])
    else:
        float_bits.pop(name,None)

def resolve_statement(node,valid,shadow,float_bits):
    """Resolve one statement; returns the statements that replace it (one or two)."""
    op=node['op']
    if op=='declare':
        name=node['name']
        if TYPES[node['type']][0]!='uint':
            original=deepcopy(node['value'])
            node['value']=resolve_expression(node['value'],valid,shadow)
            remember_float_bits(name,node['type'],original,float_bits)
            return [node]
        value=node['value'];shadow[name]=True
        if value['op']=='call' and value['function']=='bits_from_float':
            source=value['args'][0]
            if source['op']=='ref' and source['name'] in float_bits:
                value=deepcopy(float_bits[source['name']])
                node['value']=value
        if value['op']=='call' and value['function']=='bits_from_float':
            valid[name]=False
            return [{'op':'declare','name':name+'__f','type':float_type_of(node['type']),'precise':False,
                     'value':value['args'][0]}]
        require_integer(value,valid,'the initializer of '+name)
        valid[name]=True
        mirror={'op':'declare','name':name+'__f','type':float_type_of(node['type']),'precise':False,
                'value':bits_to_float_call(carrier_ref(name,node['type']))}
        return [node,mirror]
    if op=='assign':
        target=node['target'];value=node['value']
        if TYPES[target['type']][0]!='uint':
            original=deepcopy(value)
            node['value']=resolve_expression(value,valid,shadow)
            if target['op']=='ref':remember_float_bits(target['name'],target['type'],original,float_bits)
            return [node]
        if target['op']!='ref':raise Unsupported('write through a uint swizzle/array is unsupported')
        name=target['name'];shadow[name]=True
        if value['op']=='call' and value['function']=='bits_from_float':
            source=value['args'][0]
            if source['op']=='ref' and source['name'] in float_bits:
                value=deepcopy(float_bits[source['name']])
                node['value']=value
        if value['op']=='call' and value['function']=='bits_from_float':
            valid[name]=False
            return [{'op':'assign','target':shadow_ref(name,target['type']),'value':value['args'][0]}]
        if any(not valid.get(carrier,True) for carrier in uint_refs(value)):
            # The right-hand side is a float pattern, so the integer variable cannot follow it.
            valid[name]=False
            return [{'op':'assign','target':shadow_ref(name,target['type']),
                     'value':to_float_expr(value,valid,shadow,'the assignment to '+name)}]
        require_integer(value,valid,'the assignment to '+name)
        valid[name]=True
        mirror={'op':'assign','target':shadow_ref(name,target['type']),
                'value':bits_to_float_call(carrier_ref(name,target['type']))}
        return [node,mirror]
    if op=='return':return [node]
    if op in ('break','continue','discard'):return [node]
    if op=='for':
        node['init']=[s for inner in node['init'] for s in resolve_statement(inner,valid,shadow,float_bits)]
        if node['condition'] is not None:
            node['condition']=resolve_expression(node['condition'],valid,shadow)
        before_bits=deepcopy(float_bits)
        loop_bits=deepcopy(float_bits)
        node['step']=[s for inner in node['step'] for s in resolve_statement(inner,valid,shadow,loop_bits)]
        body_valid=dict(valid)
        node['body']=[s for inner in node['body'] for s in resolve_statement(inner,body_valid,shadow,loop_bits)]
        float_bits.clear()
        float_bits.update({name:value for name,value in before_bits.items() if loop_bits.get(name)==value})
        # A loop may run zero times, so only what both sides agree on stays valid.
        for name in body_valid:
            valid[name]=valid.get(name,True) and body_valid.get(name,True)
        return [node]
    if op=='if':
        node['condition']=resolve_expression(node['condition'],valid,shadow)
        then_valid=dict(valid)
        then_bits=deepcopy(float_bits)
        node['then']=[s for inner in node['then'] for s in resolve_statement(inner,then_valid,shadow,then_bits)]
        else_valid=dict(valid)
        else_bits=deepcopy(float_bits)
        if 'else' in node:
            node['else']=[s for inner in node['else'] for s in resolve_statement(inner,else_valid,shadow,else_bits)]
        float_bits.clear()
        float_bits.update({name:value for name,value in then_bits.items() if else_bits.get(name)==value})
        for name in set(then_valid)|set(else_valid):
            valid[name]=then_valid.get(name,True) and else_valid.get(name,True)
        return [node]
    raise Unsupported('unsupported IR statement')

def resolve_uint_carriers(ir):
    shadow={}
    float_bits={}
    valid={name:True for name,s in ir['symbols'].items() if TYPES[s['type']][0]=='uint'}
    ir['statements']=[s for node in ir['statements'] for s in resolve_statement(node,valid,shadow,float_bits)]
    notes=shadow.pop('__lowering_notes',[])
    for name in sorted(n for n in shadow if not n.startswith('__')):
        if name not in ir['symbols']:raise Unsupported('carrier mirror for an unknown symbol')
        ir['symbols'][name+'__f']={'type':float_type_of(ir['symbols'][name]['type']),'storage':'local'}
    if shadow:
        ir['lowering_notes'].append('uint carriers keep an intBitsToFloat mirror; utof(ftou(x)) and utof(integer) '
                                    'are both lowered exactly after flow analysis')
    ir['lowering_notes'].extend(notes)


def cg_literal(e):
    # Boolean literals carry a Python bool, not a source token: the frontend writes
    # the loop guard's `b_0=true;`, so `true`/`false` reach the emitter directly.
    if isinstance(e['value'],bool):
        return 'true' if e['value'] else 'false'
    v=e['value'].rstrip('uU')
    if e['literal']=='float':
        v=v.rstrip('fF')
        return v+('.0' if '.' not in v and 'e' not in v.lower() else '')
    number=int(v,0)
    if e['literal']=='uint' and number>0x7FFFFFFF:number-=1<<32
    return str(number)

def cg_expr(e):
    op=e['op']
    if op=='literal':return cg_literal(e)
    if op=='ref':return {'gl_Position':'vp_position','gl_FragCoord':'vp_fragcoord',
                         'gl_FragDepth':'vp_depth','gl_FrontFacing':'vp_front_facing'}.get(e['name'],e['name'])
    if op=='index':return f'{cg_expr(e["base"])}[{e["index"]}]'
    if op=='swizzle':return cg_expr(e['base'])+'.'+e['fields']
    if op=='convert':return '('+CG[e['type']]+')('+cg_expr(e['arg'])+')'
    if op=='negate':return '(-'+cg_expr(e['arg'])+')'
    if op=='logic_not':return '(!'+cg_expr(e['arg'])+')'
    if op=='bit_not':return '(~'+cg_expr(e['arg'])+')'
    if op=='binary':
        if e['operator']=='>>' and e['left']['type']=='uint':
            return 'vp_ushr('+cg_expr(e['left'])+','+cg_expr(e['right'])+')'
        return '('+cg_expr(e['left'])+e['operator']+cg_expr(e['right'])+')'
    if op=='select':return '('+cg_expr(e['condition'])+'?'+cg_expr(e['then'])+':'+cg_expr(e['other'])+')'
    if op=='call':
        fn=e['function']
        if fn.startswith('bitfield_extract_') or fn=='bitfield_insert':
            return 'vp_'+fn+'('+','.join(cg_expr(a) for a in e['args'])+')'
        if fn=='fma':return '('+cg_expr(e['args'][0])+'*'+cg_expr(e['args'][1])+'+'+cg_expr(e['args'][2])+')'
        if fn=='bits_to_float':return BITS_TO_FLOAT[TYPES[e['args'][0]['type']][0]]+'('+cg_expr(e['args'][0])+')'
        if fn=='uint_to_float':
            arg=e['args'][0]
            value=cg_expr(arg)
            width=TYPES[arg['type']][1]
            # A 16-bit split keeps the unsigned magnitude exact before the single
            # final rounding; the old (float)(low31)+2^31 form could round twice.
            if width==1:return f'vp_uint_to_float({value})'
            components='xyzw'[:width]
            converted=','.join(f'vp_uint_to_float({value}.{component})' for component in components)
            return f'{CG[e["type"]]}({converted})'
        if fn=='carrier_signed_to_float':
            return '((float)('+cg_expr(e['args'][0])+'))'
        if fn=='uint_of_float':
            value=cg_expr(e['args'][0])
            return (f'(({value}<2147483648.0)?((int)(trunc({value}))):'
                    f'(((int)(trunc({value}-2147483648.0)))|(-2147483648)))')
        if fn=='float_bits_to_uint_numeric':
            arg=e['args'][0]
            value=cg_expr(arg)
            width=TYPES[arg['type']][1]
            if width==1:return f'vp_float_bits_to_uint_numeric({value})'
            components='xyzw'[:width]
            converted=','.join(f'vp_float_bits_to_uint_numeric({value}.{component})'
                               for component in components)
            return f'{CG[e["type"]]}({converted})'
        if fn=='float_bits_to_int_numeric':
            arg=e['args'][0]
            value=cg_expr(arg)
            width=TYPES[arg['type']][1]
            if width!=1:raise Unsupported('signed numeric bit decode is only defined for scalar carriers')
            return f'vp_float_bits_to_int_numeric({value})'
        if fn=='texture':
            if e['args'][0]['type'] in SHADOW_SAMPLER_TYPES:
                raise Unsupported('shadow sampler only emits explicit textureLod at LOD0')
            if e['args'][0]['type']=='sampler1D':
                return 'tex1D('+','.join(cg_expr(a) for a in e['args'])+')'
            builtin='texCUBE' if e['args'][0]['type']=='samplerCube' else 'tex2D'
            return builtin+'('+','.join(cg_expr(a) for a in e['args'])+')'
        if fn=='textureLod' and e['args'][0]['type']=='sampler2DShadow':
            raise Unsupported('sampler2DShadow call must be the whole value of a '
                              'standalone declaration or assignment; nested shadow '
                              'expressions are not transformed')
        if fn=='textureLod' and e['args'][0]['type']=='sampler1D':
            s,coord,lod=e['args']
            return f'tex1Dlod({cg_expr(s)},float4({cg_expr(coord)},0.0,0.0,{cg_expr(lod)}))'
        if fn=='textureLod' and e['args'][0]['type']=='samplerCube':
            s,coord,lod=e['args']
            return f'texCUBElod({cg_expr(s)},float4({cg_expr(coord)},{cg_expr(lod)}))'
        if fn=='textureLod':
            s,uv,lod=e['args'];return f'tex2Dlod({cg_expr(s)},float4({cg_expr(uv)},0.0,{cg_expr(lod)}))'
        if fn=='textureLodOffset':
            s,uv,lod,off=e['args'];sampler=cg_expr(s)
            return (f'tex2Dlod({sampler},float4({cg_expr(uv)}+float2({cg_expr(off)})*{sampler}_inv_size,'
                    f'0.0,{cg_expr(lod)}))')
        if fn=='textureOffset':
            s,uv,off=e['args'];sampler=cg_expr(s)
            return f'tex2D({sampler},{cg_expr(uv)}+float2({cg_expr(off)})*{sampler}_inv_size)'
        if fn=='inversesqrt':fn='rsqrt'
        return CG.get(fn,fn)+'('+','.join(cg_expr(a) for a in e['args'])+')'
    raise Unsupported('unknown IR expression')

def cg_type(ty):return CG[ty]

def emit_cg(ir):
    syms=ir['symbols'];used=set(ir['reads'])|set(ir['writes']);stage=ir['stage']
    params=[];resources=[]
    for name,s in syms.items():
        if name not in used or s['storage']=='local':continue
        ty=cg_type(s['type']);storage=s['storage']
        if name=='gl_Position':params.append('out float4 vp_position : POSITION')
        elif name=='gl_FragCoord':params.append('float4 vp_fragcoord : WPOS')
        elif name=='gl_FragDepth':params.append('out float vp_depth : DEPTH')
        elif name=='gl_FrontFacing':params.append('uniform float4 vp_face_state')
        elif storage in ('in','out'):
            location=s['location']
            if stage=='vertex' and storage=='in':
                semantic='ATTR'+str(location)
            elif stage=='fragment' and storage=='out':
                semantic='COLOR0'
            elif location<10:
                semantic='TEXCOORD'+str(location)
            elif location==10:
                semantic='COLOR0'
                note=('varying location 10 is emitted as COLOR0: the PSP2 profiles accept ten TEXCOORD '
                      'slots, and the extended slot is measured to compile on both stages')
                if note not in ir['lowering_notes']:ir['lowering_notes'].append(note)
            elif location==11:
                semantic='COLOR1'
                note=('varying location 11 is emitted as COLOR1: the PSP2 profiles accept ten TEXCOORD '
                      'slots, and the extended slot is measured to compile on both stages')
                if note not in ir['lowering_notes']:ir['lowering_notes'].append(note)
            else:
                raise Unsupported('varying location '+str(location)+' exceeds the measured COLOR1 slot')
            params.append(('out ' if storage=='out' else '')+f'{ty} {name} : {semantic}')
        elif storage=='cbuf':
            size=s.get('required_vectors')
            if not size or size>128:raise Unsupported('cbuf exceeds initial GXM 128-vector budget')
            params.append(f'uniform float4 {name}[{size}]');resources.append({'name':name,**s})
        elif storage=='sampler':
            params.append(f'uniform {ty} {name}')
            if s.get('inv_size'):params.append(f'uniform float2 {name}_inv_size')
            if s.get('shadow'):
                # Per-sampler runtime state from the host-pure TSC planner. The
                # sampler itself stays an ordinary sampler2D over real depth.
                params.append(f'uniform float4 {name}_shadow_extent')
                params.append(f'uniform float4 {name}_shadow_state')
                params.append(f'uniform float4 {name}_shadow_border')
                s['shadow_uniform_vectors']=3
                s['shadow_abi']=shadow_helpers.abi_for_sampler(name)
            resources.append({'name':name,**s})
        else:raise Unsupported('unsupported symbol storage')
    calls=set()
    def scan(value):
        if isinstance(value,dict):
            if value.get('op')=='call':calls.add(value.get('function'))
            if (value.get('op')=='binary' and value.get('operator')=='>>'
                    and value['left']['type']=='uint'):calls.add('ushr')
            for child in value.values():scan(child)
        elif isinstance(value,list):
            for child in value:scan(child)
    scan(ir['statements'])
    lines=['// Generated from typed vp-shader-ir-2. No fastmath/fastprecision enabled.']
    if calls & {'ushr','bitfield_extract_u','bitfield_extract_s','bitfield_insert'}:
        lines.extend([
            'int vp_ushr(int value, int count) {',
            '  if (count == 0) return value;',
            '  return (value >> count) & (2147483647 >> (count - 1));',
            '}',
        ])
    if 'bitfield_extract_u' in calls:
        lines.extend([
            'int vp_bitfield_extract_u(int value, int offset, int bits) {',
            '  if (bits == 0) return 0;',
            '  if (bits == 32) return value;',
            '  return vp_ushr(value, offset) & vp_ushr(-1, 32 - bits);',
            '}',
        ])
    if 'bitfield_extract_s' in calls:
        lines.extend([
            'int vp_bitfield_extract_s(int value, int offset, int bits) {',
            '  if (bits == 0) return 0;',
            '  if (bits == 32) return value;',
            '  return (value << (32 - offset - bits)) >> (32 - bits);',
            '}',
        ])
    if 'bitfield_insert' in calls:
        lines.extend([
            'int vp_bitfield_insert(int base, int insert, int offset, int bits) {',
            '  if (bits == 0) return base;',
            '  if (bits == 32) return insert;',
            '  int mask = vp_ushr(-1, 32 - bits) << offset;',
            '  return (base & ~mask) | ((insert << offset) & mask);',
            '}',
        ])
    if 'uint_to_float' in calls:
        lines.extend([
            'float vp_uint_to_float(int u) {',
            '  // Both masked halves are nonnegative and exactly representable.',
            '  // Keep this branch-free: the locked OpenGL conversion chain',
            '  // corrupts the signed branch form for real bone indices.',
            '  return (float)((u >> 16) & 65535) * 65536.0 + (float)(u & 65535);',
            '}',
        ])
    if 'float_bits_to_uint_numeric' in calls:
        lines.extend([
            '// The locked Shacc implements log2 as ln and exp2 as exp; normalize',
            '// with exact powers of two instead of relying on those intrinsics.',
            'float vp_float_bits_to_uint_numeric(float x) {',
            '  float ax = abs(x);',
            '  float e = 0.0;',
            '  float m;',
            '  float hi;',
            '  float lo;',
            '  float negative = (x < 0.0 || (x == 0.0 && (1.0 / x) < 0.0)) ? 1.0 : 0.0;',
            '  if (ax == 0.0) return negative * 2147483648.0;',
            '  if (ax != ax) return x;',
            '  if (ax > 3.402823466e38) { e = 255.0; m = 0.0; }',
            '  else if (ax < 1.175494351e-38) {',
            '    e = 0.0;',
            '    m = floor(ax * 8.507059173023462e37 * 8388608.0);',
            '  } else {',
            '    if (ax < 1.0) { ax = ax * 8.507059173023462e37; e = -126.0; }',
            '    if (ax >= 1.8446744073709552e19) { ax = ax * 5.421010862427522e-20; e = e + 64.0; }',
            '    if (ax >= 4.294967296e9) { ax = ax * 2.3283064365386963e-10; e = e + 32.0; }',
            '    if (ax >= 65536.0) { ax = ax * 1.52587890625e-5; e = e + 16.0; }',
            '    if (ax >= 256.0) { ax = ax * 0.00390625; e = e + 8.0; }',
            '    if (ax >= 16.0) { ax = ax * 0.0625; e = e + 4.0; }',
            '    if (ax >= 4.0) { ax = ax * 0.25; e = e + 2.0; }',
            '    if (ax >= 2.0) { ax = ax * 0.5; e = e + 1.0; }',
            '    m = floor((ax - 1.0) * 8388608.0);',
            '    e = e + 127.0;',
            '  }',
            '  hi = e * 8388608.0 + floor(m / 256.0) * 256.0;',
            '  lo = m - floor(m / 256.0) * 256.0;',
            '  if (negative != 0.0) return (2147483648.0 + hi) + lo;',
            '  return hi + lo;',
            '}',
        ])
    if 'float_bits_to_int_numeric' in calls:
        lines.extend([
            '// The locked Shacc implements log2 as ln and exp2 as exp; normalize',
            '// with exact powers of two instead of relying on those intrinsics.',
            'float vp_float_bits_to_int_numeric(float x) {',
            '  float ax = abs(x);',
            '  float e = 0.0;',
            '  float m;',
            '  float hi;',
            '  float lo;',
            '  float negative = (x < 0.0 || (x == 0.0 && (1.0 / x) < 0.0)) ? 1.0 : 0.0;',
            '  if (ax == 0.0) return (negative != 0.0) ? -2147483648.0 : 0.0;',
            '  if (ax != ax) return x;',
            '  if (ax > 3.402823466e38) { e = 255.0; m = 0.0; }',
            '  else if (ax < 1.175494351e-38) {',
            '    e = 0.0;',
            '    m = floor(ax * 8.507059173023462e37 * 8388608.0);',
            '  } else {',
            '    if (ax < 1.0) { ax = ax * 8.507059173023462e37; e = -126.0; }',
            '    if (ax >= 1.8446744073709552e19) { ax = ax * 5.421010862427522e-20; e = e + 64.0; }',
            '    if (ax >= 4.294967296e9) { ax = ax * 2.3283064365386963e-10; e = e + 32.0; }',
            '    if (ax >= 65536.0) { ax = ax * 1.52587890625e-5; e = e + 16.0; }',
            '    if (ax >= 256.0) { ax = ax * 0.00390625; e = e + 8.0; }',
            '    if (ax >= 16.0) { ax = ax * 0.0625; e = e + 4.0; }',
            '    if (ax >= 4.0) { ax = ax * 0.25; e = e + 2.0; }',
            '    if (ax >= 2.0) { ax = ax * 0.5; e = e + 1.0; }',
            '    m = floor((ax - 1.0) * 8388608.0);',
            '    e = e + 127.0;',
            '  }',
            '  hi = e * 8388608.0 + floor(m / 256.0) * 256.0;',
            '  lo = m - floor(m / 256.0) * 256.0;',
            '  if (negative != 0.0) return (hi - 2147483648.0) + lo;',
            '  return hi + lo;',
            '}',
        ])
    color_outputs=[name for name,s in syms.items()
                   if stage=='fragment' and s['storage']=='out' and s.get('location')==0]
    alpha_epilogue=[]
    varying_outputs=[name for name,s in syms.items()
                     if stage=='vertex' and s['storage']=='out' and name!='gl_Position']
    varying_one=bool(varying_outputs)
    if varying_one:
        params.append('uniform float4 vp_varying_one')
    if color_outputs:
        # Maxwell alpha test happens after the shader computes color, before
        # color/depth writes. GXM has no matching fixed-function test: kill
        # failed fragments in the program, including every early return.
        params.append('uniform float4 vp_alpha_test')
        color=color_outputs[0]
        alpha_epilogue=[
            'if (vp_alpha_test.x != 0.0) {',
            f'  float vp_a = {color}.w;',
            '  float vp_ref = vp_alpha_test.z;',
            '  float vp_func = vp_alpha_test.y;',
            '  bool vp_pass = (vp_func == 2.0 && vp_a < vp_ref) ||',
            '    (vp_func == 3.0 && vp_a == vp_ref) ||',
            '    (vp_func == 4.0 && vp_a <= vp_ref) ||',
            '    (vp_func == 5.0 && vp_a > vp_ref) ||',
            '    (vp_func == 6.0 && vp_a != vp_ref) ||',
            '    (vp_func == 7.0 && vp_a >= vp_ref) || vp_func == 8.0;',
            '  if (!vp_pass) discard;',
            '}',
        ]
    shadow_samplers=[name for name,s in syms.items()
                     if name in used and s.get('shadow')]
    if shadow_samplers:
        ir['shadow_contract']={
            'strategy':shadow_helpers.STRATEGY,
            'policy':shadow_helpers.POLICY,
            'policy_sha256':shadow_helpers.policy_sha256(),
            'emitter_sha256':shadow_helpers.emitter_source_sha256(),
            'helper_sha256':shadow_helpers.helper_source_sha256(),
            'samplers':{name:shadow_helpers.abi_for_sampler(name)
                        for name in shadow_samplers},
        }
    lines.append('void main('+',\n          '.join(params)+') {')
    has_face='gl_FrontFacing' in used
    if has_face:
        # The measured PSP2 FACE path becomes a constant predicate in the
        # current Vita3K USSE translator. For a culled draw the surviving
        # guest face is known exactly; the runtime binds that state and
        # rejects an unculled use rather than guessing per-fragment facing.
        lines.append('  bool vp_front_facing = vp_face_state.x != 0.0;')
    shadow_emitter=shadow_helpers.InlineEmitter()
    for s in ir['statements']:
        for line in cg_statement(s,'  ',shadow_emitter):
            if varying_one:
                for output in varying_outputs:
                    # Preserve a literal homogeneous varying W as an actual
                    # register write. The measured GXP->GLSL path loses the
                    # compiler's constant-one varying and exposes W=0 to FS.
                    pattern=r'(^\s*'+re.escape(output)+r'\.w\s*=\s*)1(?:\.0*|\.)(;)'
                    line=re.sub(pattern,r'\1vp_varying_one.x\2',line)
                    if re.match(r'^\s*'+re.escape(output)+r'\s*=',line):
                        line=line.replace('float4(0,0,0,1)','float4(0,0,0,vp_varying_one.x)')
            if alpha_epilogue and line.strip()=='return;':
                indent=line[:len(line)-len(line.lstrip())]
                lines.extend(indent+ep for ep in alpha_epilogue)
            lines.append(line)
    if not ir['statements'] or ir['statements'][-1]['op']!='return':
        lines.extend('  '+ep for ep in alpha_epilogue)
    lines+=['}','']
    cbuf_vectors=sum(r.get('required_vectors',0) for r in resources)
    aux_vectors=bool(alpha_epilogue)+has_face+varying_one
    shadow_vectors=3*len(shadow_samplers)
    if shadow_vectors:
        ir['uniform_budget']={
            'cbuf_vectors':cbuf_vectors,
            'aux_vectors':aux_vectors,
            'shadow_vectors':shadow_vectors,
            'total':cbuf_vectors+aux_vectors+shadow_vectors,
            'limit':128,
        }
    if cbuf_vectors+aux_vectors+shadow_vectors>128:
        raise Unsupported('combined cbufs exceed initial GXM 128-vector budget')
    return '\n'.join(lines),resources

def cg_shadow_statement(s,indent,emitter):
    """Lower one standalone shadow call to scoped inline statements."""
    call=s['value']
    if not shadow_helpers.is_shadow_call(call):
        raise Unsupported('sampler2DShadow call must be the whole value of a '
                          'standalone declaration or assignment')
    sampler=call['args'][0]
    coord=call['args'][1]
    if sampler['op']!='ref':
        raise Unsupported('shadow sampler must be a named uniform')
    if shadow_helpers.contains_shadow_call(coord):
        raise Unsupported('nested sampler2DShadow calls are not transformed')
    if s['op']=='declare' and s['type']!='float':
        raise Unsupported('sampler2DShadow returns float; declaration type is '
                          +s['type'])
    lines,result=emitter.emit_sample(sampler['name'],cg_expr(coord),indent,
                                     'dispatch')
    if s['op']=='declare':
        lines.append(f'{indent}{cg_type(s["type"])} {s["name"]} = {result};')
    else:
        if shadow_helpers.contains_shadow_call(s['target']):
            raise Unsupported('shadow sampler call cannot be used in an lvalue')
        lines.append(f'{indent}{cg_expr(s["target"])} = {result};')
    return lines

def cg_statement(s,indent,emitter=None):
    if emitter is None:emitter=shadow_helpers.InlineEmitter()
    op=s['op']
    if op in ('declare','assign'):
        if shadow_helpers.contains_shadow_call(s['value']):
            return cg_shadow_statement(s,indent,emitter)
        if op=='assign' and shadow_helpers.contains_shadow_call(s['target']):
            raise Unsupported('shadow sampler call cannot be used in an lvalue')
        if op=='declare':return [f'{indent}{cg_type(s["type"])} {s["name"]} = {cg_expr(s["value"])};']
        return [f'{indent}{cg_expr(s["target"])} = {cg_expr(s["value"])};']
    if op=='return':return [indent+'return;']
    if op=='break':return [indent+'break;']
    if op=='continue':return [indent+'continue;']
    if op=='discard':return [indent+'discard;']
    if op=='for':
        # Hoist the initializer so its declaration stays in the enclosing Cg scope,
        # and keep only the condition in the loop header.
        out=[]
        if s['condition'] and shadow_helpers.contains_shadow_call(s['condition']):
            raise Unsupported('sampler2DShadow call cannot be lowered inside a '
                              'for condition; use a standalone statement')
        for inner in s['init']:out.extend(cg_statement(inner,indent,emitter))
        head='for (; %s ;)' % cg_expr(s['condition']) if s['condition'] else 'for (;;)'
        out.append(f'{indent}{head} {{')
        for inner in s['body']:out.extend(cg_statement(inner,indent+'  ',emitter))
        for inner in s['step']:out.extend(cg_statement(inner,indent+'  ',emitter))
        out.append(indent+'}')
        return out
    if op=='if':
        if shadow_helpers.contains_shadow_call(s['condition']):
            raise Unsupported('sampler2DShadow call cannot be lowered inside an '
                              'if condition; use a standalone statement')
        out=[f'{indent}if ({cg_expr(s["condition"])}) {{']
        for inner in s['then']:out.extend(cg_statement(inner,indent+'  ',emitter))
        out.append(indent+'}')
        if 'else' in s:
            out[-1]=indent+'} else {'
            for inner in s['else']:out.extend(cg_statement(inner,indent+'  ',emitter))
            out.append(indent+'}')
        return out
    raise Unsupported('unsupported IR statement')

def link_pair(vertex,fragment):
    if vertex['stage']!='vertex' or fragment['stage']!='fragment':raise Unsupported('stage pair reversed')
    stores={s['location'] for n,s in vertex['symbols'].items() if s['storage']=='out' and n in vertex['writes']}
    loads={s['location'] for n,s in fragment['symbols'].items() if s['storage']=='in' and n in fragment['reads']}
    if not loads<=stores:raise Unsupported('fragment varying missing from vertex outputs')
    return sorted(loads)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--glsl',type=Path,required=True)
    p.add_argument('--stage',choices=['vertex','fragment'],required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--unsigned-attributes',default='',help='comma-separated captured UINT input locations')
    a=p.parse_args();locations=[int(x) for x in a.unsigned_attributes.split(',') if x]
    ir=lower(a.glsl.read_text(encoding='utf-8'),a.stage,locations);cg,res=emit_cg(ir)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.with_suffix('.ir.json').write_text(json.dumps(ir,indent=2)+'\n',encoding='utf-8',newline='\n')
    a.output.with_suffix('.cg').write_text(cg,encoding='utf-8',newline='\n')
    print(json.dumps({'stage':a.stage,'statements':len(ir['statements']),'resources':res,
                      'lowering_notes':ir['lowering_notes']}))

if __name__=='__main__':main()
