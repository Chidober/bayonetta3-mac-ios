#!/usr/bin/env python3
# B3 着色器离线出包：b3host 导出的着色器（B3_SHADER_DUMP 目录：<键>.code、<键>.ctl、shader_map.tsv）+ 绘制时事实（日志里的 B3-SHFACT 行）
# → B3 版翻译器（dm/b3/shader/xlate，Ryujinx 着色器翻译器）→ SPIR-V → spirv-cross MSL → metal → <键>_b.metallib + 回放反射 <键>_b.json，并写 B3 映射表。
# 产物格式与通用层的 dm/gfx/tools/dmsh_build.py 相同（运行时 nvn_mtl.m 读同样的字段），但键、映射表、事实、句柄基址都是 B3 自己的。产物只写到仓库外。
# 用法：build.py --dump DIR --facts LOG [LOG…] [--out DIR] [--map FILE] [--limit N] [--keys K…]；需要 dotnet（翻译器）、spirv-cross、Xcode 的 metal。
# B3 offline shader build : shaders dumped by b3host (B3_SHADER_DUMP dir: <key>.code, <key>.ctl, shader_map.tsv) + draw-time facts (B3-SHFACT lines in logs)
# → the B3 translator (dm/b3/shader/xlate, the Ryujinx shader translator) → SPIR-V → spirv-cross MSL → metal → <key>_b.metallib + replay reflection <key>_b.json, and writes the B3 shader map.
# Output format matches the base runtime's dm/gfx/tools/dmsh_build.py (nvn_mtl.m reads the same fields at runtime), but keys, map, facts and handle bases are B3's own . Output goes outside the repository only.
# Usage: build.py --dump DIR --facts LOG [LOG…] [--out DIR] [--map FILE] [--limit N] [--keys K…]; needs dotnet (translator), spirv-cross and Xcode's metal.
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
CHAIN = (HERE / 'CHAIN').read_text().split()[0]               # B3 翻译链版本号，与 Makefile 的 B3_SHADER_CHAIN 一致 | B3 translation chain tag, same as B3_SHADER_CHAIN in the Makefile
XC = ['--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-texture-buffer-native', '--msl-pad-fragment-output']   # 与 dmsh_build.py 相同 | same as dmsh_build.py
ZH_STAGE = {'顶点': 'vs', '像素': 'ps', '计算': 'cs', '外壳': 'hs', '域': 'ds'}
NVN_STAGE = {'vs': 0, 'ps': 1, 'hs': 3, 'ds': 4, 'cs': 5}       # NVNshaderStage
TEX_BASE, SAMP_BASE, IMG_BASE = 0x20, 0x420, 0x120                               # B3 绑定句柄：槽 i 在字节 0x20 + 8i（HYPOTHESIS 待更多样本）| B3 bound handles: slot i at byte 0x20 + 8i (HYPOTHESIS pending more samples)
# 图像句柄表 c[2] 0x120 + 8 r：NVN 驱动布局（通用层实测同值）。B3 唯一样本 fp_i_cb2_4A（字节 0x128）→ 槽 1；运行时 B3-SHFACT kind=img 记实际绑定的图像槽以核对（HYPOTHESIS）
# Image handle table c[2] 0x120 + 8 r: an NVN driver layout (the base runtime measured the same value). The only B3 sample fp_i_cb2_4A (byte 0x128) → slot 1; at runtime B3-SHFACT kind=img logs the image slots actually bound, to check (HYPOTHESIS)
TARGET = {0: '1d', 1: '2d', 2: '3d', 3: '1darray', 4: '2darray', 5: '2dms', 6: '2dmsarray', 7: '2d', 8: 'cube', 9: 'cubearray', 10: 'buffer'}   # NVNtextureTarget
SBUF_BASE = {'vs': 0x110, 'cs': 0x310, 'ps': 0x510}           # c0 存储缓冲地址表基址：沿用通用层实测（NVN 驱动布局，HYPOTHESIS：B3 相同）| c0 storage buffer table bases: the base runtime's measurement (NVN driver layout, HYPOTHESIS: same for B3)
DRV_FILL = {0: [(0x110, 0x210), (0x310, 0x410), (0x510, 0x610)], 2: [(0x168, 0x568), (0x568, 0x968)]}   # 回放层实际能填的驱动段区间（nvn_mtl.m）| driver segment ranges the replay layer can fill (nvn_mtl.m)


def nvn_formats():
    """NVNformat 值 → 分量类型（名字以 UI 结尾 = uint，以 I 结尾 = sint，其余 float）| NVNformat value → component type (name ending in UI = uint, in I = sint, else float)"""
    out = {}
    for m in re.finditer(r'NVN_FORMAT_(\w+)\s*=\s*(0x[0-9A-Fa-f]+)', (REPO / 'dm/gfx/nvn/include/nvn.h').read_text()):
        n = m.group(1); out[int(m.group(2), 16)] = 'uint' if n.endswith('UI') else 'sint' if n.endswith('I') else 'float'
    return out


def read_map(dump):
    """导出目录的 shader_map.tsv：键 → 阶段，程序序号 → 键列表 | the dump's shader_map.tsv: key → stage, program serial → keys"""
    stage, by_prog = {}, {}
    for ln in (dump / 'shader_map.tsv').read_text().splitlines():
        p = ln.split('\t')
        if len(p) < 4 or p[2] not in ZH_STAGE: continue
        stage[p[3]] = ZH_STAGE[p[2]]; by_prog.setdefault(p[1], set()).add(p[3])
    return stage, by_prog


def read_facts(logs, stage, by_prog, fmt):
    """B3-SHFACT 行 → 每个键的 tex / attr / fout 事实；同一槽见到不同值时记冲突，按先见到的作答 | B3-SHFACT lines → tex / attr / fout facts per key; conflicting values on one slot are recorded and the first one wins"""
    facts, conflicts = {}, 0
    # 程序序号只在一次运行里有意义：先用本日志自己的 B3-SHFACT-PROG 行，没有时才退回导出映射表（导出可能来自别的运行）
    # Program serials only mean something within one run: use the log's own B3-SHFACT-PROG lines first  and fall back to the dump map only when absent (the dump may come from another run)
    rx = re.compile(r'B3-SHFACT prog=(\d+) kind=(\w+)(?: stage=(\d+))? (?:slot|loc)=(\d+)(?: target=(\d+))? format=(0x[0-9a-f]+|0)')
    rp = re.compile(r'B3-SHFACT-PROG prog=(\d+) keys=([0-9a-f,]*)')
    for log in logs:
        own = {}
        with open(log, encoding='utf-8', errors='replace') as f:
            for ln in f:
                m = rp.search(ln)
                if m: own[m.group(1)] = {k for k in m.group(2).split(',') if k in stage}
        with open(log, encoding='utf-8', errors='replace') as f:
            for ln in f:
                m = rx.search(ln)
                if not m: continue
                prog, kind, st, slot, tg, fm = m.groups(); fm = int(fm, 16)
                for key in own.get(prog, by_prog.get(prog, ())):
                    s = stage[key]
                    if kind == 'tex' and NVN_STAGE.get(s) == int(st): table, val = 'tex', TARGET.get(int(tg), '2d')
                    elif kind == 'attr' and s == 'vs': table, val = 'attr', fmt.get(fm, 'float')
                    elif kind == 'fout' and s == 'ps': table, val = 'fout', fmt.get(fm, 'float')
                    else: continue
                    t = facts.setdefault(key, {}).setdefault(table, {})
                    if slot in t and t[slot] != val: conflicts += 1
                    t.setdefault(slot, val)
    return facts, conflicts


def read_links(logs, stage):
    """顶点着色器 → 运行时与它一起绘制过的像素着色器，只取日志的 B3-SHFACT-PROG 行（每行是一次运行里一个程序的各阶段键；导出映射表的序号跨运行会撞，不用）
    vertex shader → pixel shaders it was drawn with at runtime, from the logs' B3-SHFACT-PROG lines only (each line is one program's stage keys within one run; the dump map's serials collide across runs and are not used)"""
    rp = re.compile(r'B3-SHFACT-PROG prog=(\d+) keys=([0-9a-f,]*)')
    groups = []
    for log in logs:
        with open(log, encoding='utf-8', errors='replace') as f:
            for ln in f:
                m = rp.search(ln)
                if m: groups.append(set(m.group(2).split(',')))
    links = {}
    for g in groups:
        vs = [k for k in g if stage.get(k) == 'vs']; ps = [k for k in g if stage.get(k) == 'ps']
        for v in vs: links.setdefault(v, set()).update(ps)
    return {v: sorted(p) for v, p in links.items() if p}


def decode_tex(name):
    """fp_t_tcb_<句柄> / fp_t_cb<槽>_<句柄> → (贴图槽, 采样器槽, 句柄类型)；B3 只认绑定句柄表（c[2] 起 0x20）| → (texture slot, sampler slot, handle type); B3 only knows the bound handle table (c[2] from 0x20)"""
    m = re.match(r'[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m: return None
    slots = int(m.group(1)) if m.group(1) else -1; h = int(m.group(2), 16)
    tb = (slots & 0xffff) if slots >= 0 else 2
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    def slot(word, base): return (word * 4 - base) // 8 if tb == 2 and word * 4 >= base and (word * 4 - base) % 8 == 0 else None
    words = [o0] + ([o1] if ty else [])
    t = next((slot(x, TEX_BASE) for x in words if x * 4 < SAMP_BASE and slot(x, TEX_BASE) is not None), None)
    s = next((slot(x, SAMP_BASE) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE) is not None), None)
    return t, s, ty


def decode_img(name):
    """fp_i_cb2_<句柄> → 图像槽：(句柄字 × 4 − IMG_BASE) / 8，只认 c[2] 且落在贴图表之前 | fp_i_cb2_<handle> → image slot: (handle word × 4 − IMG_BASE) / 8, only for c[2] and before the texture table"""
    m = re.match(r'[a-z]+_i\w*?_cb(\d+)_([0-9A-F]+)', name)
    if not m or (int(m.group(1)) & 0xffff) != 2: return None
    b = (int(m.group(2), 16) & 0x3fff) * 4
    return (b - IMG_BASE) // 8 if IMG_BASE <= b < IMG_BASE + 8 * 8 and (b - IMG_BASE) % 8 == 0 else None


def reflect(stage, msl, res):
    """MSL 参数缓冲成员 → 回放反射绑定（种类同 nvn_refl.h；与 dmsh_build.py 的 reflect 同口径，句柄基址换成 B3 的）| MSL argument buffer members → replay reflection bindings (kinds as in nvn_refl.h; same convention as dmsh_build.py's reflect, with B3's handle bases)"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', msl, re.S)
    binds = []
    for m in re.finditer(r'^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)\]\];', body.group(1) if body else '', re.M):
        ty, nm, aid = m.group(1).strip(), m.group(2), int(m.group(3))
        e = {'msl_name': nm, 'msl_type': ty, 'arg_id': aid}
        if nm == 'support_buffer': e['kind'] = 'support'
        elif re.fullmatch(r'[a-z]+_c(\d+)', nm):
            n = int(re.fullmatch(r'[a-z]+_c(\d+)', nm).group(1))
            e.update(kind='cbuf', reg=n - 3) if 3 <= n <= 16 else e.update(kind='c1' if n == 1 else 'drvcb', bank=n)
        elif re.fullmatch(r'[a-z]+_s(\d+)', nm): e.update(kind='sbuf', index=int(re.fullmatch(r'[a-z]+_s(\d+)', nm).group(1)))
        elif nm.endswith('Smplr'):
            d = decode_tex(nm[:-5]) or (None, None, 0)
            e.update(kind='samp', reg=d[1]) if d[2] else e.update(kind='samp_tex', reg=d[0])
        else:
            d = decode_tex(nm)
            if re.match(r'[a-z]+_i', nm): e.update(kind='img', reg=decode_img(nm))
            elif not d: e.update(kind='?')
            else: e.update(kind='tbuf' if 'texture_buffer' in ty else 'tex', reg=d[0])
        binds.append(e)
    base = SBUF_BASE.get(stage)
    sb = sorted(res.get('sbufs', []), key=lambda b: b['binding'])
    for e in binds:
        if e['kind'] != 'sbuf': continue
        b = sb[e['index']] if e['index'] < len(sb) else None
        off = b['cb_offset'] * 4 if b and b['cb_slot'] == 0 else None
        e['reg'] = (off - base) // 16 if off is not None and base is not None and 0 <= off - base < 0x100 and (off - base) % 16 == 0 else None
        e['rw'] = bool(b and b['flags'] & 1)   # Ryujinx BufferUsageFlags.Write = 1 << 0（原先误用位 1）| Ryujinx BufferUsageFlags.Write = 1 << 0 (bit 1 was used by mistake)
    return binds


def drv_reads(src):
    out = {}
    for bank in (0, 2):
        offs, dyn = set(), False
        for m in re.finditer(r'fp_c%d\)\._m0\[([^\]]+)\]' % bank, src):
            idx = m.group(1).rstrip('u')
            if idx.isdigit(): offs.add(int(idx) * 16)
            else: dyn = True
        if offs or dyn: out[str(bank)] = {'vec4_offsets': sorted(offs), 'dynamic': dyn}
    return out


def unfillable(dr):
    bad = []
    for bank, d in dr.items():
        if d['dynamic']: bad.append(f'c{bank} dynamic')
        bad += [f'c{bank}+{o:#x}' for o in d['vec4_offsets'] if not any(a <= o < b for a, b in DRV_FILL[int(bank)])]
    return bad


AT_DECL = """
// B3 alpha 测试：由 dm/b3/shader/build.py 加入。<entry>_b3at 与 <entry> 相同，只在返回前按函数常量 200（NVNalphaFunc）/ 201（参考值）丢弃片元
// B3 alpha test: added by dm/b3/shader/build.py. <entry>_b3at equals <entry> except that it discards fragments before returning, per function constants 200 (NVNalphaFunc) / 201 (reference)
constant int b3_at_func [[function_constant(200)]];
constant float b3_at_ref_c [[function_constant(201)]];
constant int b3_at_f = is_function_constant_defined(b3_at_func) ? b3_at_func : 8;
constant float b3_at_ref = is_function_constant_defined(b3_at_ref_c) ? b3_at_ref_c : 0.0;
static inline bool b3_at_pass(float a)
{
    switch (b3_at_f) { case 1: return false; case 2: return a < b3_at_ref; case 3: return a == b3_at_ref; case 4: return a <= b3_at_ref;
                       case 5: return a > b3_at_ref; case 6: return a != b3_at_ref; case 7: return a >= b3_at_ref; default: return true; }
}
"""


def add_alpha_test(src, entry):
    """片元着色器多出一个带 alpha 测试的入口 <entry>_b3at（入口函数在文件末尾，spirv-cross 的输出结构 main0_out 的 [[color(0)]] 成员是颜色 0）；没有颜色 0 时原样返回
    Give a fragment shader a second entry <entry>_b3at with the alpha test (the entry function is at the end of the file; the [[color(0)]] member of spirv-cross's output struct is colour 0); returned unchanged without colour 0"""
    m = re.search(r'struct\s+\w+_out\s*\{[^}]*?\b(\w+)\s*\[\[color\(0\)\]\]', src)
    f = re.search(r'^fragment\s+\S+\s+' + re.escape(entry) + r'\s*\(', src, re.M)
    if not m or not f: return src, False
    body = src[f.start():].replace(' ' + entry + '(', ' ' + entry + '_b3at(', 1)
    body = body.replace('return out;', 'if (!b3_at_pass(out.%s.w)) discard_fragment();\n    return out;' % m.group(1))
    return src.rstrip() + '\n' + AT_DECL + '\n' + body, True


def compute_dims(ctl_path):
    """计算着色器的线程组大小与共享内存：控制段 +0x748 起三个 u32 是 (x, y, z)（66 个计算着色器样本全部是合理值，图形着色器此处全 0）；
    +0x754 / +0x758 非 0 时认为用了共享内存，给 32 KiB（Metal 的上限；HYPOTHESIS：这两字的单位未核，多给只影响占用率）
    Threadgroup size and shared memory of a compute shader: three u32 at control +0x748 are (x, y, z) (sensible values in all 66 compute samples, zeros there in graphics shaders);
    when +0x754 / +0x758 is non-zero the shader is taken to use shared memory and gets 32 KiB (Metal's limit; HYPOTHESIS: the unit of these words is unverified, over-allocating only affects occupancy)"""
    try: b = ctl_path.read_bytes()
    except OSError: return None, 0
    if len(b) < 0x75c: return None, 0
    x, y, z = struct.unpack_from('<3I', b, 0x748); s1, s2 = struct.unpack_from('<2I', b, 0x754)
    if not (1 <= x <= 1024 and 1 <= y <= 1024 and 1 <= z <= 64 and x * y * z <= 1024): return None, 0
    return [x, y, z], 32768 if (s1 or s2) else 0


SHUF_OLD = '    return simd_shuffle(value, lane);'
SHUF_NEW = ('    T b3r = simd_shuffle(value, lane);\n'
            '    return ((((ulong)(simd_vote::vote_t)simd_active_threads_mask()) >> lane) & 1ul) ? b3r : value;')


def fix_shuffle(src):
    """子组交换只取当前有效线程：游戏的光源合并按 NVIDIA 的线程 ↔ 像素排布从固定线程取值；Apple 的线程组排布不同，源线程可能没有像素，读到的是垃圾值（角色菜单衣服配色错、鞋子闪烁消失）。源线程无效时用自己的值
    Subgroup shuffles only take active threads: the game's light merging reads fixed lanes assuming NVIDIA's lane <-> pixel layout; Apple's layout differs and the source lane may have no pixel, giving garbage (wrong outfit colours, flickering / vanishing shoes in the character menu). With an inactive source lane, use our own value"""
    return src.replace(SHUF_OLD, SHUF_NEW, 1) if SHUF_OLD in src else src


SDK = 'macosx'   # --sdk iphoneos：给 iPhone 编（产物目录 b-<链>-ios）| --sdk iphoneos: build for iPhone (output dir b-<chain>-ios)
AIR_CACHE = Path(os.environ.get('B3_AIR_CACHE', str(Path.home() / 'b3-work/b3/air_cache')))
_METAL_VER = None
def compile_air(msl, air):
    """MSL → AIR，按"MSL 内容 + 编译选项 + 编译器版本"的哈希复用上次的产物（增量重建：只编译翻译结果真的变了的着色器）；返回 subprocess 结果或 None（命中缓存）
    MSL → AIR, reusing the previous output by a hash of "MSL text + compile flags + compiler version" (incremental rebuild: only shaders whose translation really changed are compiled); returns the subprocess result, or None on a cache hit"""
    global _METAL_VER
    if _METAL_VER is None: _METAL_VER = subprocess.run(['xcrun', '-sdk', SDK, 'metal', '--version'], capture_output=True, text=True).stdout
    flags = ['-fpreserve-invariance']   # 没有 invariant 输出时不起作用；保留以免编译缓存全部失效 | no effect without invariant outputs; kept so the compile cache stays valid
    h = hashlib.sha256(((SDK if SDK != 'macosx' else '') + _METAL_VER + ' '.join(flags) + '\0' + msl.read_text()).encode()).hexdigest()
    hit = AIR_CACHE / f'{h}.air'
    if hit.exists(): shutil.copyfile(hit, air); return None
    r = subprocess.run(['xcrun', '-sdk', SDK, 'metal', *flags, '-c', str(msl), '-o', str(air)], capture_output=True, text=True)
    if not r.returncode:
        AIR_CACHE.mkdir(parents=True, exist_ok=True); tmp = AIR_CACHE / f'{h}.{os.getpid()}.tmp'; shutil.copyfile(air, tmp); os.replace(tmp, hit)
    return r


EQ_TOL = 'b3_eq_tol'   # 函数常量 202，运行时给（缺省值见 add_depth_eq）| function constant 202, set at run time (default in add_depth_eq)
def add_depth_eq(src, entry):
    """深度"相等"容差版本 <entry>_b3eq：运行时对多次间接绘制的"相等"改用"小于等于"并绑定深度副本到片元纹理 0，这里补上近侧界限——片元比已写深度近超过 EQ_TOL 就丢弃。
    合起来相当于"相等、容忍几个最小单位"：Apple GPU 两次光栅化差 1 个单位（章节地图竖线）能通过，按形状写深度的剪纸卡片（过场云）形状外的部分仍被丢弃。没有入口时返回 None
    Depth-EQUAL tolerance version <entry>_b3eq: at run time multi-indirect EQUAL draws use LEQUAL and bind a depth copy at fragment texture 0; this adds the near bound - a fragment nearer than the written depth by more than EQ_TOL is discarded.
    Together this is "EQUAL within a few units": the one-unit rasterization difference on Apple GPUs (chapter-map lines) passes, while cut-out cards that write depth by shape (cutscene clouds) still drop everything outside the shape. Returns None without an entry"""
    f = re.search(r'^fragment\s+\S+\s+' + re.escape(entry) + r'\s*\((.*?)\)\s*\{', src, re.M | re.S)
    if not f: return None
    params = f.group(1); add = [] if '[[position]]' in params else ['float4 gl_FragCoord [[position]]']
    pm = re.search(r'float4\s+(\w+)\s*\[\[position\]\]', params); pos = pm.group(1) if pm else 'gl_FragCoord'
    add.append('depth2d<float> b3_zc [[texture(0)]]')
    head = src[:f.start()] + src[f.start():f.end()].replace(' ' + entry + '(', ' ' + entry + '_b3eq(', 1)
    head = head[:head.rindex(')')] + ', ' + ', '.join(add) + ')\n{'
    consts = 'constant float b3_eq_tol_c [[function_constant(202)]];\nconstant float b3_eq_tol = is_function_constant_defined(b3_eq_tol_c) ? b3_eq_tol_c : 2.4e-7f;\n'
    out = head + f'\n    if ({pos}.z < b3_zc.read(uint2({pos}.xy)) - {EQ_TOL}) discard_fragment();' + src[f.end():]
    k = out.find('using namespace metal;\n')
    return out[:k + 23] + consts + out[k + 23:] if k >= 0 else consts + out


def infer_int_attrs(src):
    """没有运行时格式事实时推断整数顶点属性：一个 float4 输入的每次使用都是 as_type<int/uint>(in.X.c)（按原始位当整数用，如骨骼编号），它就是整数格式。
    改声明为 uint4、去掉位转换，Metal 才会把整数格式按整数交给着色器（否则转成浮点值，位解读就错：魔女时间残影成斜块）
    Infer integer vertex attributes when runtime format facts are missing: a float4 input whose every use is as_type<int/uint>(in.X.c) (raw bits used as an integer, e.g. bone indices) has an integer format.
    Declare it uint4 and drop the bitcasts so Metal hands integer formats to the shader as integers (otherwise they are converted to float values and the bit reading goes wrong: witch-time afterimages as slabs)"""
    m = re.search(r'struct\s+(\w+_in)\s*\{(.*?)\};', src, re.S)
    f = re.search(r'\b\w+_in\s+(\w+)\s*\[\[stage_in\]\]', src)
    if not m or not f: return src
    inn = f.group(1); body = m.group(2); out = src
    for name, n in re.findall(r'float4\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]', body):
        uses = re.findall(r'(?:as_type<(u?int)>\()?\b' + inn + r'\.' + name + r'\b(\.[xyzw])?', out)
        if not uses or any(not t or not c for t, c in uses): continue
        out = re.sub(r'as_type<int>\(' + inn + r'\.' + name + r'(\.[xyzw])\)', lambda q: 'int(' + inn + '.' + name + q.group(1) + ')', out)
        out = re.sub(r'as_type<uint>\(' + inn + r'\.' + name + r'(\.[xyzw])\)', lambda q: inn + '.' + name + q.group(1), out)
        out = re.sub(r'float4(\s+' + name + r'\s*\[\[attribute\(' + n + r'\)\]\])', r'uint4\1', out, count=1)
    return out


def add_vfetch_bounds(src):
    """顶点读取越界按 0：NVN 读超出顶点缓冲大小的顶点返回 0，Metal 读到相邻内存（第 5 章特效间接绘制读 504 个顶点、缓冲只有 4 个 → 大斜板 / 紫色射线）。
    入口加 [[vertex_id]] 与每属性元素上限表 b3_vlim（buffer(1)，运行时按绑定的缓冲大小给），超出就把该属性置 0
    Out-of-range vertex fetch reads 0: NVN returns 0 for vertices past the vertex buffer size, Metal reads adjacent memory (chapter 5 effect indirect draws fetch 504 vertices from a 4-vertex buffer → huge slabs / purple beams).
    The entry gets [[vertex_id]] and a per-attribute element limit table b3_vlim (buffer(1), filled at run time from the bound buffer sizes); an attribute past its limit is zeroed"""
    m = re.search(r'struct\s+(\w+_in)\s*\{(.*?)\};', src, re.S)
    f = re.search(r'^vertex\s+\S+\s+\w+\s*\((.*?)\)\s*\{', src, re.M | re.S)
    if f and '[[stage_in]]' not in f.group(1): f = None
    if not m or not f: return src
    attrs = re.findall(r'(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]', m.group(2))
    if not attrs: return src
    inn = re.search(r'\b\w+_in\s+(\w+)\s*\[\[stage_in\]\]', f.group(1)).group(1)
    vm = re.search(r'uint\s+(\w+)\s*\[\[vertex_id\]\]', f.group(1)); vid = vm.group(1) if vm else 'b3_vid'
    params = f.group(1) + ('' if vm else ', uint b3_vid [[vertex_id]]') + ', constant uint *b3_vlim [[buffer(1)]]'
    body = ''.join(f'\n    if ({vid} >= b3_vlim[{n}]) {inn}.{name} = {t}(0);' for t, name, n in attrs)
    return src[:f.start(1)] + params + src[f.end(1):f.end()] + body + src[f.end():]


def finish(key, stage, work, out, tg_size=None, c1=None):
    """SPIR-V → MSL → metallib + 反射 JSON；返回 (状态, 说明) | SPIR-V → MSL → metallib + reflection JSON; returns (status, detail)"""
    spv, msl, air = work / f'{key}.spv', work / f'{key}.metal', work / f'{key}.air'
    if not spv.exists(): return 'xlate', 'no spv'
    r = subprocess.run(['spirv-cross', *XC, str(spv), '--output', str(msl)], capture_output=True, text=True)
    if r.returncode: return 'msl', r.stderr.strip()[:160]
    msl.write_text(msl.read_text() if os.environ.get('B3_AB_NO_SHUFFLE') else fix_shuffle(msl.read_text()))   # B3_AB_NO_SHUFFLE：对照构建，不改子组交换| A/B build without the shuffle change 
    eq_src = None
    if stage == 'ps':   # 深度相等容差版本，单独一个文件，主文件的编译缓存不受影响 | depth-EQUAL tolerance version , in its own file so the main file's compile cache is unaffected
        e0 = re.search(r'fragment\s+\S+\s+(\w+)\s*\(', msl.read_text())
        if e0: eq_src = add_depth_eq(msl.read_text(), e0.group(1)); eq_entry = e0.group(1) + '_b3eq'
    at = False
    if stage == 'ps':
        entry0 = re.search(r'(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(', msl.read_text())
        if entry0:
            src2, at = add_alpha_test(msl.read_text(), entry0.group(2))
            if at: msl.write_text(src2)
    int_src, int_locs = None, []
    if stage == 'vs':   # 顶点读取越界按 0；推断出的整数属性另编一个 <entry>_b3int 版本，运行时按实际顶点格式选（浮点格式也会被按位读，不能一刀切）| out-of-range vertex fetch reads 0 ; inferred integer attributes get a separate <entry>_b3int version chosen at run time by the actual vertex format (float formats are bit-read too, so no blanket change)
        base = msl.read_text(); isrc = infer_int_attrs(base)
        if isrc != base:
            e0 = re.search(r'vertex\s+\S+\s+(\w+)\s*\(', isrc)
            fl = dict((n, t) for t, n in re.findall(r'(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]', base)); il = dict((n, t) for t, n in re.findall(r'(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]', isrc))
            int_locs = sorted(int(n) for n in il if il[n] != fl.get(n))
            if e0: int_src = re.sub(r'\b' + re.escape(e0.group(1)) + r'\s*\(', e0.group(1) + '_b3int(', add_vfetch_bounds(isrc), count=1); int_entry = e0.group(1) + '_b3int'
        msl.write_text(add_vfetch_bounds(base))
    # 位置量化（b3x13）已撤（b3x15）：x / y / z / w 各自抹 7 位尾数，远处深度 z/w 误差可达约 1e-5，远山等远处物体互相深度冲突（视角移动时抖动、白色噪点）；深度相等失败的真正原因是丢绘制
    # Position quantization (b3x13) removed (b3x15): dropping 7 mantissa bits of x / y / z / w separately gives far-depth z/w errors up to about 1e-5, so distant geometry such as far mountains depth-fights (jitter and white speckles while the camera moves); the real cause of the depth-EQUAL failures was dropped draws 
    # 顶点位置 invariant（b3x10–b3x16）已撤（b3x17）：对照构建量过，标题怪兽与章节地图的深度误差分布有无它都一样（只差 1 个单位，由深度相等容差处理，/ ）
    # Vertex position invariance (b3x10-b3x16) removed (b3x17): measured with an A/B build, the depth-mismatch distribution on the title monster and the chapter map is the same with or without it (one unit only, handled by the depth-EQUAL tolerance, / )
    r = compile_air(msl, air)
    if r and r.returncode: return 'metal', r.stderr.strip()[:160]
    airs, alt_entry, alt_at = [str(air)], None, None
    aspv = work / f'{key}~alt.spv'
    if stage == 'ps' and aspv.exists():   # 输出类型相反的版本，入口改名 <entry>_alt，编进同一个库| opposite-output-type version with the entry renamed to <entry>_alt, compiled into the same library 
        amsl, aair = work / f'{key}~alt.metal', work / f'{key}~alt.air'
        r = subprocess.run(['spirv-cross', *XC, str(aspv), '--output', str(amsl)], capture_output=True, text=True)
        e0 = re.search(r'(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(', amsl.read_text()) if not r.returncode else None
        if e0:
            src = re.sub(r'\b' + re.escape(e0.group(2)) + r'\s*\(', e0.group(2) + '_alt(', (amsl.read_text() if os.environ.get('B3_AB_NO_SHUFFLE') else fix_shuffle(amsl.read_text())), count=1)
            src2, aat = add_alpha_test(src, e0.group(2) + '_alt')
            amsl.write_text(src2)
            r = compile_air(amsl, aair)
            if not (r and r.returncode): airs.append(str(aair)); alt_entry = e0.group(2) + '_alt'; alt_at = (alt_entry + '_b3at') if aat else None
    int_ok = None
    if int_src:
        imsl, iair = work / f'{key}~int.metal', work / f'{key}~int.air'
        imsl.write_text(int_src); r = compile_air(imsl, iair)
        if not (r and r.returncode): airs.append(str(iair)); int_ok = int_entry
    eq_ok = None
    if eq_src:
        emsl, eair = work / f'{key}~eq.metal', work / f'{key}~eq.air'
        emsl.write_text(eq_src); r = compile_air(emsl, eair)
        if not (r and r.returncode): airs.append(str(eair)); eq_ok = eq_entry
    lib = out / f'{key}_b.metallib'
    r = subprocess.run(['xcrun', '-sdk', SDK, 'metallib', *airs, '-o', str(lib) + '.tmp'], capture_output=True, text=True)
    if r.returncode: return 'metallib', r.stderr.strip()[:160]
    os.replace(str(lib) + '.tmp', lib)
    src = msl.read_text(); res = json.loads((work / f'{key}.res.json').read_text())
    entry = re.search(r'(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(', src); dr = drv_reads(src)
    refl = {'key': key + '_b', 'route': 'B', 'chain': CHAIN, 'dm_ctl': key, 'source': 'b3-dump', 'stage': stage,
            'entry': entry.group(2) if entry else 'main0', 'msl_entry_kind': entry.group(1) if entry else None,
            'alpha_test_entry': (entry.group(2) + '_b3at') if at and entry else None,
            'fout0': (lambda m: {'float4': 'float', 'int4': 'sint', 'uint4': 'uint'}.get(m.group(1), 'float') if m else None)(re.search(r'(\w+)\s+\w+\s*\[\[color\(0\)\]\]', src)) if stage == 'ps' else None,
            'alt_entry': alt_entry, 'alt_alpha_test_entry': alt_at, 'eq_entry': eq_ok, 'int_entry': int_ok, 'int_locs': int_locs if int_ok else [],
            'threadgroup_size': tg_size, 'c1': c1 or '', 'notes': res.get('notes', []), 'drv_reads': dr, 'drv_unfillable': unfillable(dr),
            'bindings': reflect(stage, src, res),
            'vertex_inputs': [{'location': int(m.group(2)), 'msl_type': m.group(1)} for m in re.finditer(r'(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]', src)]}
    tmp = out / f'{key}_b.json.tmp'; tmp.write_text(json.dumps(refl, ensure_ascii=False, sort_keys=True)); os.replace(tmp, out / f'{key}_b.json')
    return 'ok', ''


def ensure_xlate(path):
    """B3 翻译器：把仓内 Ryujinx 源码（dm/third_party/ryujinx，不改）复制到仓库外、打 B3 补丁（xlate/ryujinx-b3.patch）后编译；版本戳记在输出目录，CHAIN 变了就重编
    B3 translator: copy the in-repo Ryujinx source (dm/third_party/ryujinx, unmodified) outside the repository, apply the B3 patch (xlate/ryujinx-b3.patch) and build; a stamp in the output dir forces a rebuild when CHAIN changes"""
    stamp = path.parent / 'CHAIN.built'
    if path.exists() and stamp.exists() and stamp.read_text() == CHAIN: return path
    src = path.parent.parent / 'ryujinx-src'
    shutil.rmtree(src, True); shutil.copytree(REPO / 'dm/third_party/ryujinx', src, ignore=shutil.ignore_patterns('obj', 'bin'))
    r = subprocess.run(['patch', '-p1', '-i', str(HERE / 'xlate/ryujinx-b3.patch')], cwd=src, capture_output=True, text=True)
    if r.returncode: sys.exit('B3 补丁打不上 | B3 patch failed:\n' + r.stdout + r.stderr)
    env = dict(os.environ); env.setdefault('DOTNET_ROOT', '/opt/homebrew/opt/dotnet/libexec')
    r = subprocess.run(['dotnet', 'build', str(HERE / 'xlate/b3_xlate.csproj'), '-c', 'Release', f'-p:RyuSrc={src}', '-o', str(path.parent)], capture_output=True, text=True, env=env)
    shutil.rmtree(HERE / 'xlate/obj', True)   # dotnet 的中间目录不留在仓库 | do not leave dotnet's intermediate dir in the repository
    if r.returncode or not path.exists(): sys.exit('B3 翻译器编译失败 | B3 translator build failed:\n' + r.stdout[-2000:] + r.stderr[-2000:])
    stamp.write_text(CHAIN)
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description='B3 offline shader build.')
    ap.add_argument('--dump', required=True, type=Path); ap.add_argument('--facts', nargs='*', default=[])
    ap.add_argument('--out', type=Path, default=Path.home() / 'b3-work/b3/shader_cache'); ap.add_argument('--map', type=Path, default=Path.home() / 'b3-work/b3/shader_map.tsv')
    ap.add_argument('--xlate', type=Path, default=Path.home() / 'b3-work/build/b3/xlate/b3_xlate')
    ap.add_argument('--limit', type=int, default=0); ap.add_argument('--keys', nargs='*', default=[]); ap.add_argument('-j', type=int, default=8); ap.add_argument('--sdk', default='macosx', choices=('macosx', 'iphoneos'))
    a = ap.parse_args(argv)
    for p in (a.out, a.map):
        if REPO in p.resolve().parents: sys.exit('输出不能在仓库里 | output must be outside the repository')
    stage, by_prog = read_map(a.dump)
    facts, conflicts = read_facts(a.facts, stage, by_prog, nvn_formats())
    keys = [k for k in (a.keys or sorted(stage)) if (a.dump / f'{k}.code').exists() and stage.get(k) in ('vs', 'ps', 'cs')]
    links = read_links(a.facts, stage)
    if a.limit: keys = keys[:a.limit]
    global SDK; SDK = a.sdk
    out = a.out / (f'b-{CHAIN}' + ('-ios' if SDK == 'iphoneos' else '')); out.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='b3sh_'))
    jobs, tg, c1s = [], {}, {}
    def bin_of(k):   # 去掉 0x40 余量与头：图形 0x30、计算 0x100| drop the 0x40 margin and the header: 0x30 for graphics, 0x100 for compute 
        p = work / f'{k}.bin'
        if not p.exists(): p.write_bytes((a.dump / f'{k}.code').read_bytes()[:-0x40][0x100 if stage[k] == 'cs' else 0x30:])
        return p
    for k in keys:
        bin_of(k)
        j = {'key': k, 'code': str(work / f'{k}.bin'), 'stage': 'cs' if stage[k] == 'cs' else stage[k], 'debug': False}
        c1f = a.dump / f'{k}.c1'
        if c1f.exists(): j['c1'] = c1f.read_bytes().hex(); c1s[k] = j['c1']   # 编译器常量 c[1]| compiler constants c[1] 
        if stage[k] == 'cs':
            local, shared = compute_dims(a.dump / f'{k}.ctl')
            if local: j['local'] = local; j['shared'] = shared; tg[k] = local
        if stage[k] == 'vs':   # 配对的片元着色器| paired fragment shaders 
            nxt = [str(bin_of(p)) for p in links.get(k, []) if (a.dump / f'{p}.code').exists()]
            if nxt: j['next'] = nxt
        j.update(facts.get(k, {})); jobs.append(j)
        if stage[k] == 'ps':   # 输出类型相反的版本：同一片元着色器有时写整数目标、有时写浮点目标 | opposite-output-type version : one fragment shader may write integer targets at one time and float targets at another
            base = (j.get('fout') or {}).get('0', 'float')
            alt = dict(j); alt['key'] = k + '~alt'; alt['fout'] = {str(i): ('float' if base in ('sint', 'uint') else 'sint') for i in range(8)}
            jobs.append(alt)
    (work / 'jobs.json').write_text(json.dumps(jobs))
    env = dict(os.environ); env.setdefault('DOTNET_ROOT', '/opt/homebrew/opt/dotnet/libexec')
    x = subprocess.run([str(ensure_xlate(a.xlate)), str(work / 'jobs.json'), str(work)], capture_output=True, text=True, env=env)
    fo = [k for k in os.environ.get('B3_FINISH_ONLY', '').split(',') if k]   # 诊断：全体翻译（并集照常），只编这几个键 | diagnostic: translate everything (union as usual) but compile only these keys
    if fo: keys = [k for k in keys if k in fo]
    with ThreadPoolExecutor(a.j) as ex: results = list(ex.map(lambda k: (k, finish(k, stage[k], work, out, tg.get(k), c1s.get(k))), keys))
    stats = {}
    for k, (st, _) in results: stats[st] = stats.get(st, 0) + 1
    zh = {v: k for k, v in ZH_STAGE.items()}
    a.map.parent.mkdir(parents=True, exist_ok=True)
    a.map.write_text('# kind: b3  B3 着色器映射表（dm/b3/shader/build.py）| B3 shader map\n' + ''.join(f'b3\t0\t{zh[stage[k]]}\t{k}\n' for k in sorted(stage)))
    print(json.dumps({'chain': CHAIN, 'keys': len(keys), 'with_facts': sum(1 for k in keys if k in facts), 'fact_conflicts': conflicts,
                      'vs_linked': sum(1 for k in keys if stage[k] == 'vs' and links.get(k)), 'results': stats,
                      'xlate_tail': x.stdout.strip().splitlines()[-2:] if x.stdout else [], 'out': str(out), 'map': str(a.map)}, ensure_ascii=False))
    if os.environ.get('B3_KEEP_WORK'): print('work', work)   # 诊断：留下中间文件（spv / metal）| diagnostic: keep the intermediate files (spv / metal)
    else: shutil.rmtree(work, True)
    return 0 if stats.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main())
