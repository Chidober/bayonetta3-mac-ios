// Switch 原版着色器（Maxwell 机器码）→ SPIR-V（Ryujinx.Graphics.Shader 翻译器），供 B 路线离线出 metallib。
// 与 Ryujinx.ShaderTools 的区别：翻译器的「查询」按真实情况作答，而不是一律缺省值——
//   贴图类型 / 图像格式 / 顶点属性类型 / 片元输出类型 / 计算线程组大小取自作业文件（由映射表配对的 PC 反射给出，同槽号 = 同资源）；
//   窗口原点左上、深度 0..1（游戏实测 nvnDeviceSetWindowOriginMode / SetDepthMode）。
// 用法：dmsh_xlate <作业.json> <输出目录>
//   作业：[{ "key", "stage": "vs|ps|cs", "code": 机器码文件（图形 = 0x50 字节 SPH + 指令，计算 = 指令），
//            "tex": { "槽": "2d|2darray|cube|cubearray|3d|buffer|1d|1darray|2dms|2dmsarray", … }, "img": { "槽": { "type", "fmt" } },
//            "attr": { "位置": "float|sint|uint" }, "fout": { "目标": "float|sint|uint" }, "local": [x, y, z],
//            "next": [配对片元着色器的机器码文件, …]（只给顶点着色器）}]
//   输出：<key>.spv 与 <key>.res.json（存储缓冲槽在 c0 的偏移等，贴图句柄 → 槽号的解码在出 metallib 的脚本里做）
// Original Switch shaders (Maxwell machine code) → SPIR-V (the Ryujinx.Graphics.Shader translator), for route B's offline metallib build.
// Difference from Ryujinx.ShaderTools: the translator's "queries" are answered truthfully rather than always with defaults -
//   texture type / image format / vertex attribute type / fragment output type / compute thread group size come from the job file (given by the PC reflection paired via the mapping table; same slot number = same resource);
//   window origin upper-left, depth 0..1 (measured from the game: nvnDeviceSetWindowOriginMode / SetDepthMode).
// Usage: dmsh_xlate <job.json> <output dir>
//   Job: [{ "key", "stage": "vs|ps|cs", "code": machine code file (graphics = 0x50-byte SPH + instructions, compute = instructions),
//           "tex": { "slot": "2d|2darray|cube|cubearray|3d|buffer|1d|1darray|2dms|2dmsarray", ... }, "img": { "slot": { "type", "fmt" } },
//           "attr": { "location": "float|sint|uint" }, "fout": { "target": "float|sint|uint" }, "local": [x, y, z],
//           "next": [machine code files of the paired fragment shaders, ...] (vertex shaders only) }]
//   Output: <key>.spv and <key>.res.json (offsets of storage buffer slots in c0, etc.; decoding texture handle → slot number is done in the script that builds the metallib)
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

static class SwshXlate
{
    // B3 版：只改句柄基址。B3 用组合贴图句柄，翻出的着色器从“绑定句柄”字 8 + 2i 取槽 i（字节 0x20 + 8i，284 个样本），
    // 没有分离采样器句柄；作业里给 tex_base / samp_base 时用作业的值。通用层的基址 0x168 / 0x568 不适用。
    // B3 version: only the handle bases change. B3 uses combined texture handles; translated shaders take slot i from bound handle word 8 + 2i (byte 0x20 + 8i, 284 samples),
    // with no separate sampler handles; job fields tex_base / samp_base override them. the base runtime's bases 0x168 / 0x568 do not apply.
    static int TexBase = 0x20, SampBase = 0x420;

    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code; readonly JsonObject _job; int _binding = 1; public readonly List<string> Notes = new();
        readonly byte[] _c1;
        public Accessor(byte[] code, JsonObject job) { _code = code; _job = job; _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>(); }
        // c1 常数段（着色器包控制段前 0x100 字节）：间接跳转 BRX 的跳转表在这里（LDC c[1]）；不提供时翻译器解不出跳转目标、控制流出错（粒子着色器死循环）
        // c1 constant segment (the 0x100 bytes before the control segment in the shader package): the jump table of indirect jump BRX lives here (LDC c[1]); without it the translator cannot resolve jump targets and control flow goes wrong (particle shaders loop forever)
        public uint ConstantBuffer1Read(int offset) { if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset); Notes.Add($"c1 读越界 +{offset:X}（c1 长 {_c1.Length}）"); return 0; }
        // 全部资源放进第 0 组，各占不同绑定号（spirv-cross 出成一个参数缓冲）；常量段号 index 的缓冲由翻译器给 binding = index+1 的惯例不依赖
        // All resources go into set 0, each with a distinct binding number (spirv-cross emits them as one argument buffer); the translator's convention of giving the buffer of constant segment number index binding = index+1 is not relied on
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        public int QuerySamplerArrayLengthFromPool() => 32;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
        public int QueryTextureArrayLengthFromPool() => 32;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        int TexSlot(int handle, int cbufSlot)
        {
            (int off0, _, _) = TextureHandle.UnpackOffsets(handle);
            int bytes = off0 * 4;
            return (cbufSlot < 0 || (cbufSlot & 0xffff) == 2) && bytes >= TexBase && bytes < SampBase && (bytes - TexBase) % 8 == 0 ? (bytes - TexBase) / 8 : -1;
        }
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            string t = slot >= 0 ? Get("tex", slot) ?? Get("img", slot) : null;
            if (t == null) { Notes.Add($"贴图句柄 {handle:X}（常量段 {cbufSlot}，槽 {slot}）无 PC 类型，按 2D"); return SamplerType.Texture2D; }
            return t switch
            {
                "1d" => SamplerType.Texture1D, "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer, "2d" => SamplerType.Texture2D, "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample, "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D, "cube" => SamplerType.TextureCube, "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => SamplerType.Texture2D,
            };
        }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (f == null) { Notes.Add($"图像句柄 {handle:X} 槽 {slot} 无 PC 格式，按 R8G8B8A8Unorm"); return TextureFormat.R8G8B8A8Unorm; }
            return Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;   // 配对 PC 的 groupshared 声明之和 | sum of the paired PC shader's groupshared declarations
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            return new GpuGraphicsState(false, InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, false /* 深度 0..1 | depth 0..1 */, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                false /* 不做 Y 取反 | no Y flip */, true /* 原点左上 | origin upper-left */, false);
        }
        public bool QueryHostSupportsTextureShadowLod() => true;
        // 作业 reduced = true：放宽精度——翻译器只给强制精确的运算打 NoContraction（缺省全部打：保持 Maxwell 分开舍入的逐位结果，代价是 MSL 里大量禁优化的 spvFMul 等）
        // Job reduced = true: relaxed precision - the translator marks NoContraction only on operations that must be exact (the default marks all: keeps the bit-exact results of Maxwell's separate rounding, at the cost of many optimization-disabled spvFMul etc. in the MSL)
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
    }

    static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--source-sha")   // 嵌入的 Program.cs + csproj 的 sha256 前 16 位 | first 16 digits of the sha256 of the embedded Program.cs + csproj
        {
            var asm = typeof(SwshXlate).Assembly; var ms = new MemoryStream();
            foreach (var n in new[] { "src0", "src1" }) asm.GetManifestResourceStream(n).CopyTo(ms);
            Console.WriteLine(Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(ms.ToArray()))[..16].ToLowerInvariant());
            return 0;
        }
        var jobs = JsonNode.Parse(File.ReadAllText(args[0])).AsArray(); string outDir = args[1]; Directory.CreateDirectory(outDir);
        int ok = 0, bad = 0;
        // 全体片元输入的并集：先把所有像素着色器解码一遍，记下它们读的插值位置与分量；每个顶点着色器都声明并初始化这个并集，
        // 任意顶点 / 片元组合都能建管线，不再依赖运行时见过的配对（实测：只见过一部分配对时，人物材质的组合建管线失败，人物只剩深度预通道留下的黑影）。
        // 并集超过 30 个位置（Metal 顶点输出上限 31 个 float4，含位置）时不用并集，退回逐配对。
        // Union of all fragment inputs : decode every pixel shader first and record the interpolated locations and components it reads; every vertex shader declares and initializes that union,
        // so any vertex / fragment combination builds a pipeline without depending on pairs seen at runtime (measured: with only part of the pairs seen, the character material combinations failed to build and characters were left as the black shapes of the depth prepass).
        // If the union exceeds 30 locations (Metal's vertex output limit is 31 float4 including position), the union is not used and the per-pair path remains.
        int unionMask = 0; UInt128 unionComp = 0;
        var auProp = typeof(TranslatorContext).GetProperty("AttributeUsage", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic);
        foreach (JsonObject job in jobs)
        {
            if ((string)job["stage"] != "ps") continue;
            try
            {
                var c0 = Translator.CreateContext(0, new Accessor(File.ReadAllBytes((string)job["code"]), job), new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, 0));
                var au = auProp.GetValue(c0);
                unionMask |= (int)au.GetType().GetProperty("UsedInputAttributes").GetValue(au);
                unionComp |= (UInt128)au.GetType().GetProperty("ThisInputAttributesComponents").GetValue(au);
            }
            catch (Exception) { }
        }
        bool useUnion = System.Numerics.BitOperations.PopCount((uint)unionMask) <= 30;
        Console.WriteLine($"片元输入并集 {System.Numerics.BitOperations.PopCount((uint)unionMask)} 个位置（掩码 {unionMask:X8}），{(useUnion ? "顶点着色器全部按并集输出" : "超过上限，退回逐配对")}");
        foreach (JsonObject job in jobs)
        {
            string key = (string)job["key"];
            try
            {
                byte[] code = File.ReadAllBytes((string)job["code"]);
                TexBase = job["tex_base"] is JsonNode tb ? (int)tb : 0x20; SampBase = job["samp_base"] is JsonNode sbase ? (int)sbase : 0x420;
                var acc = new Accessor(code, job);
                // 作业 debug = false：关 DebugMode（DebugMode 会跳过 AST 优化器，产物满是临时变量；不改语义）
                // Job debug = false: turn off DebugMode (DebugMode skips the AST optimizer and the output is full of temporaries; semantics unchanged)
                var flags = (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) | ((string)job["stage"] == "cs" ? TranslationFlags.Compute : 0);
                var ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                // 作业 next（顶点着色器）：运行时与它配对过的像素着色器机器码。逐个 SetNextStage（Ryujinx 自己的阶段衔接），顶点着色器就声明每个配对片元着色器读的插值输出；
                // Metal 要求片元输入都由顶点输出提供，否则建管线失败（约 9000 次）。多个配对时输出取并集，只有最后一个的分量被初始化为 0，其余声明未写（与 Maxwell 上读到未定义值一致）。
                // Job next (vertex shaders): machine code of the pixel shaders it was paired with at runtime. SetNextStage on each (Ryujinx's own stage linking) makes the vertex shader declare every interpolated output its fragment partners read;
                // Metal requires every fragment input to come from a vertex output or pipeline creation fails (about 9000 times). With several partners the outputs are the union; only the last one's components are initialized to 0 and the rest are declared but unwritten (matching the undefined values Maxwell would read).
                int linked = 0;
                if (job["next"] is JsonArray next)
                    foreach (var n in next)
                    {
                        try
                        {
                            var nacc = new Accessor(File.ReadAllBytes((string)n), new JsonObject());
                            ctx.SetNextStage(Translator.CreateContext(0, nacc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, 0)));
                            linked++;
                        }
                        catch (Exception e) { acc.Notes.Add($"配对片元着色器解码失败 | paired fragment shader failed to decode: {Path.GetFileName((string)n)} {e.GetType().Name}"); }
                    }
                if (useUnion && unionMask != 0 && (string)job["stage"] == "vs")
                {
                    var au = auProp.GetValue(ctx); var t = au.GetType(); var bf = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
                    t.GetMethod("MergeOutputUserAttributes", bf).Invoke(au, new object[] { false, unionMask, Array.Empty<int>() });   // 声明并集的全部输出 | declare every output in the union
                    t.GetProperty("NextInputAttributesComponents").SetValue(au, unionComp);                                              // 并集的每个分量都初始化为 0 | initialize every component of the union to 0
                    linked = -1;
                }
                ShaderProgram p = ctx.Translate();
                File.WriteAllBytes(Path.Combine(outDir, key + ".spv"), p.BinaryCode);
                var res = new JsonObject
                {
                    ["key"] = key, ["stage"] = (string)job["stage"], ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
                    ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
                    ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap,
                    ["linked_next"] = linked,
                };
                foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add(new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot });
                foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add(new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });
                foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
                    foreach (var t in list) res[name].AsArray().Add(new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags });
                File.WriteAllText(Path.Combine(outDir, key + ".res.json"), res.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                ok++;
            }
            catch (Exception e) { bad++; Console.Error.WriteLine($"【红】{key}：{e.GetType().Name}：{e.Message.Split('\n')[0]}"); if (Environment.GetEnvironmentVariable("DMSH_STACK") != null) Console.Error.WriteLine(e.StackTrace); }
        }
        Console.WriteLine($"翻译 {ok} 成功，{bad} 失败");
        return bad == 0 ? 0 : 1;
    }
}
