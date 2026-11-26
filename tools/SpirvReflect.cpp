// ---------------------------------------------------------------------------
// SpirvReflect —— 把 .spv 里的描述符绑定与块布局导出成 C++ 头文件（TM-12）。
//
// 为什么要有它：GLSL 里的 layout(set = , binding = ) 与 C++ 里的常量原本是两份
// 手写的数字，改一边漏一边，要等到运行期验证层报「descriptor 类型不匹配」才发现。
// 把 .spv（编译器已经认可的那份事实）在构建期反射出来，C++ 直接引用生成的常量，
// 再用 static_assert 校验自己那份结构体的成员偏移与块大小 —— 两边不一致就构建失败。
//
// 这不是运行期反射：只读 SPIR-V 字节码，不碰 Vulkan，产物是可 diff 的文本。
//
// 用法：
//   SpirvReflect <input.spv> <output.reflect.h> <namespace>
// 例：
//   SpirvReflect build/shaders/ssao.frag.spv build/shaders/ssao.frag.spv.reflect.h ssao_frag
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#if defined(_WIN32)
#include <crtdbg.h>
#endif
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;

enum class Kind
{
    Unknown,
    Bool,
    Float,
    Int,
    Vector,
    Matrix,
    Array,
    RuntimeArray,
    Struct,
    Image,
    Sampler,
    SampledImage,
    Pointer,
};

struct TypeInfo
{
    Kind kind = Kind::Unknown;
    uint32_t a = 0;  // 向量 / 矩阵 / 数组：元素类型；指针：指向的类型
    uint32_t b = 0;  // 向量：分量数；矩阵：列数；标量：位宽
    std::vector<uint32_t> members;
    uint32_t lengthId = 0;  // 数组：长度常量的 id
    uint32_t storageClass = 0;
};

struct Variable
{
    uint32_t id = 0;
    uint32_t typeId = 0;      // 指向的结构类型
    uint32_t storageClass = 0;
    std::string name;
};

struct Module
{
    std::vector<uint32_t> words;
    std::map<uint32_t, std::string> names;
    std::map<std::pair<uint32_t, uint32_t>, std::string> memberNames;
    std::map<uint32_t, TypeInfo> types;
    std::map<uint32_t, uint64_t> constants;
    std::map<uint32_t, uint32_t> setOf;
    std::map<uint32_t, uint32_t> bindingOf;
    std::map<uint32_t, uint32_t> arrayStride;
    std::map<uint32_t, uint32_t> matrixStride;
    std::map<uint32_t, uint32_t> memberOffset;  // key: (struct << 16) | index
    std::map<uint32_t, bool> isUniformBlock;
    std::map<uint32_t, bool> isStorageBlock;
    std::vector<Variable> variables;
    uint32_t executionModel = 0xFFFFFFFFu;
    std::string entryPoint = "main";
};

// SPIR-V 规定字节序是小端；按字取字节而不是 reinterpret_cast 后按字节走，
// 是因为后者一旦遇到没有终止符的串就会读到缓冲区末尾之外。
std::string readString(const uint32_t* words, uint32_t wordCount)
{
    std::string s;
    for (uint32_t w = 0; w < wordCount; ++w)
    {
        const uint32_t word = words[w];
        for (int b = 0; b < 4; ++b)
        {
            const char c = static_cast<char>((word >> (b * 8)) & 0xFFu);
            if (c == '\0')
                return s;
            s.push_back(c);
        }
    }
    return s;
}

bool parse(Module& m, const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0 || (size % 4) != 0)
        return false;
    m.words.resize(static_cast<size_t>(size) / 4u);
    in.read(reinterpret_cast<char*>(m.words.data()), size);
    if (!in)
        return false;
    if (m.words.size() < 5u || m.words[0] != kSpirvMagic)
        return false;


    size_t i = 5;
    while (i < m.words.size())
    {
        const uint32_t word0 = m.words[i];
        const uint32_t wc = word0 >> 16;
        const uint32_t op = word0 & 0xFFFFu;
        if (wc == 0 || i + wc > m.words.size())
            return false;

        // 用 data() + 偏移取指针：&m.words[i + 1] 在最后一条指令（i + 1 == size）
        // 时就是一次越界的 operator[]，Debug 下会直接断言退出。
        const uint32_t* p = m.words.data() + i + 1;
        const uint32_t n = wc - 1;

        switch (op)
        {
        case 15:  // OpEntryPoint
            if (n >= 3)
            {
                m.executionModel = p[0];
                m.entryPoint = readString(p + 2, n - 2);
            }
            break;
        case 5:  // OpName
            if (n >= 1)
                m.names[p[0]] = readString(p + 1, n - 1);
            break;
        case 6:  // OpMemberName
            if (n >= 3)
                m.memberNames[std::make_pair(p[0], p[1])] = readString(p + 2, n - 2);
            break;
        case 71:  // OpDecorate
            if (n >= 2)
            {
                const uint32_t target = p[0];
                const uint32_t decoration = p[1];
                if (decoration == 33 && n >= 3)  // Binding
                    m.bindingOf[target] = p[2];
                else if (decoration == 34 && n >= 3)  // DescriptorSet
                    m.setOf[target] = p[2];
                else if (decoration == 6 && n >= 3)  // ArrayStride
                    m.arrayStride[target] = p[2];
                else if (decoration == 7 && n >= 3)  // MatrixStride
                    m.matrixStride[target] = p[2];
                else if (decoration == 2)  // Block
                    m.isUniformBlock[target] = true;
                else if (decoration == 3)  // BufferBlock
                    m.isStorageBlock[target] = true;
            }
            break;
        case 72:  // OpMemberDecorate
            if (n >= 3)
            {
                const uint32_t type = p[0];
                const uint32_t index = p[1];
                const uint32_t decoration = p[2];
                if (decoration == 35 && n >= 4)  // Offset
                    m.memberOffset[(type << 16) | (index & 0xFFFFu)] = p[3];
            }
            break;
        case 20:  // OpTypeBool
            m.types[p[0]].kind = Kind::Bool;
            break;
        case 21:  // OpTypeInt
            if (n >= 3)
            {
                m.types[p[0]].kind = Kind::Int;
                m.types[p[0]].b = p[1];
            }
            break;
        case 22:  // OpTypeFloat
            if (n >= 2)
            {
                m.types[p[0]].kind = Kind::Float;
                m.types[p[0]].b = p[1];
            }
            break;
        case 23:  // OpTypeVector
            if (n >= 3)
            {
                m.types[p[0]].kind = Kind::Vector;
                m.types[p[0]].a = p[1];
                m.types[p[0]].b = p[2];
            }
            break;
        case 24:  // OpTypeMatrix
            if (n >= 3)
            {
                m.types[p[0]].kind = Kind::Matrix;
                m.types[p[0]].a = p[1];
                m.types[p[0]].b = p[2];
            }
            break;
        case 28:  // OpTypeArray
            if (n >= 3)
            {
                m.types[p[0]].kind = Kind::Array;
                m.types[p[0]].a = p[1];
                m.types[p[0]].lengthId = p[2];
            }
            break;
        case 29:  // OpTypeRuntimeArray
            if (n >= 2)
            {
                m.types[p[0]].kind = Kind::RuntimeArray;
                m.types[p[0]].a = p[1];
            }
            break;
        case 30:  // OpTypeStruct
        {
            if (n < 1)
                break;
            TypeInfo& t = m.types[p[0]];
            t.kind = Kind::Struct;
            for (uint32_t k = 1; k < n; ++k)
                t.members.push_back(p[k]);
            break;
        }
        case 32:  // OpTypePointer
            if (n >= 3)
            {
                m.types[p[0]].kind = Kind::Pointer;
                m.types[p[0]].storageClass = p[1];
                m.types[p[0]].a = p[2];
            }
            break;
        case 25:  // OpTypeImage
            m.types[p[0]].kind = Kind::Image;
            break;
        case 26:  // OpTypeSampler
            m.types[p[0]].kind = Kind::Sampler;
            break;
        case 27:  // OpTypeSampledImage
            if (n >= 2)
            {
                m.types[p[0]].kind = Kind::SampledImage;
                m.types[p[0]].a = p[1];
            }
            break;
        case 43:  // OpConstant
            // 注意操作数顺序：OpConstant 是「结果类型、结果 id、字面量」，
            // 与其它类型指令（只有结果 id）不同 —— 按 p[0] 存会存成类型 id 的键，
            // 于是数组长度查不着，最终在生成的头文件里显示成 [ ? ]。
            if (n >= 3)
                m.constants[p[1]] = p[2];
            break;
        case 59:  // OpVariable
            if (n >= 3)
            {
                Variable v;
                v.typeId = p[0];
                v.id = p[1];
                v.storageClass = p[2];
                m.variables.push_back(v);
            }
            break;
        default:
            break;
        }

        i += wc;
    }


    for (Variable& v : m.variables)
    {
        auto it = m.names.find(v.id);
        if (it != m.names.end())
            v.name = it->second;
        auto t = m.types.find(v.typeId);
        if (t != m.types.end() && t->second.kind == Kind::Pointer)
            v.typeId = t->second.a;
    }
    return true;
}

uint32_t roundUp(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
        return value;
    return (value + alignment - 1u) / alignment * alignment;
}

uint32_t typeSize(const Module& m, uint32_t id);
uint32_t typeAlign(const Module& m, uint32_t id);

uint32_t typeSize(const Module& m, uint32_t id)
{
    auto it = m.types.find(id);
    if (it == m.types.end())
        return 0;
    const TypeInfo& t = it->second;
    switch (t.kind)
    {
    case Kind::Bool:
        return 4;
    case Kind::Int:
    case Kind::Float:
        return t.b / 8u;
    case Kind::Vector:
        return t.b * typeSize(m, t.a);
    case Kind::Matrix:
    {
        const uint32_t column = typeSize(m, t.a);
        auto s = m.matrixStride.find(id);
        if (s != m.matrixStride.end() && t.b > 0)
            return s->second * (t.b - 1u) + column;
        return t.b * column;
    }
    case Kind::Array:
    {
        const uint32_t element = typeSize(m, t.a);
        auto c = m.constants.find(t.lengthId);
        const uint32_t count = (c == m.constants.end()) ? 0u : static_cast<uint32_t>(c->second);
        auto s = m.arrayStride.find(id);
        if (s != m.arrayStride.end() && count > 0)
            return s->second * (count - 1u) + element;
        return count * element;
    }
    case Kind::RuntimeArray:
        return 0;  // 长度未知：调用方负责标记
    case Kind::Struct:
    {
        if (t.members.empty())
            return 0;
        const uint32_t lastIndex = static_cast<uint32_t>(t.members.size()) - 1u;
        uint32_t lastOffset = 0;
        auto off = m.memberOffset.find((id << 16) | lastIndex);
        if (off != m.memberOffset.end())
            lastOffset = off->second;
        return roundUp(lastOffset + typeSize(m, t.members[lastIndex]), typeAlign(m, id));
    }
    default:
        return 0;
    }
}

uint32_t typeAlign(const Module& m, uint32_t id)
{
    auto it = m.types.find(id);
    if (it == m.types.end())
        return 1;
    const TypeInfo& t = it->second;
    switch (t.kind)
    {
    case Kind::Bool:
        return 4;
    case Kind::Int:
    case Kind::Float:
        return t.b / 8u;
    case Kind::Vector:
    {
        const uint32_t component = typeAlign(m, t.a);
        return (t.b == 2u) ? component * 2u : component * 4u;
    }
    case Kind::Matrix:
        return typeAlign(m, t.a);
    case Kind::Array:
    case Kind::RuntimeArray:
    {
        const uint32_t element = typeAlign(m, t.a);
        return element > 16u ? element : 16u;
    }
    case Kind::Struct:
    {
        uint32_t align = 1;
        for (uint32_t member : t.members)
        {
            const uint32_t a = typeAlign(m, member);
            if (a > align)
                align = a;
        }
        return align;
    }
    default:
        return 1;
    }
}

bool hasRuntimeArray(const Module& m, uint32_t id)
{
    auto it = m.types.find(id);
    if (it == m.types.end())
        return false;
    const TypeInfo& t = it->second;
    if (t.kind == Kind::RuntimeArray)
        return true;
    if (t.kind == Kind::Array)
        return hasRuntimeArray(m, t.a);
    if (t.kind == Kind::Struct)
    {
        for (uint32_t member : t.members)
            if (hasRuntimeArray(m, member))
                return true;
    }
    return false;
}

std::string sanitize(const std::string& name)
{
    std::string out;
    for (char c : name)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out.push_back(ok ? c : '_');
    }
    if (out.empty())
        out = "unnamed";
    if (out[0] >= '0' && out[0] <= '9')
        out.insert(out.begin(), '_');
    return out;
}

// kernelSize -> KernelSize；tex_position -> Tex_Position
std::string pascal(const std::string& name)
{
    std::string out;
    bool upper = true;
    for (char c : name)
    {
        if (c == '_')
        {
            upper = true;
            continue;
        }
        if (upper && c >= 'a' && c <= 'z')
            out.push_back(static_cast<char>(c - 'a' + 'A'));
        else
            out.push_back(c);
        upper = false;
    }
    if (out.empty())
        out = "X";
    if (out[0] >= '0' && out[0] <= '9')
        out.insert(out.begin(), '_');
    return out;
}

struct Descriptor
{
    uint32_t set = 0;
    uint32_t binding = 0;
    uint32_t count = 1;
    std::string name;
    std::string kind;
    uint32_t kindCode = 0;
    uint32_t typeId = 0;
};

// 描述符类型的稳定编号：写进 reflect.h 的 k<Name>Kind，C++ 侧用
// src/RHI/DescriptorReflect.h 的 descriptorTypeOf() 换成 VkDescriptorType。
// 两边的编号必须同步 —— 改这里就改那里。
uint32_t descriptorKindCode(const std::string& kind)
{
    if (kind == "sampler") return 1;
    if (kind == "combinedImageSampler") return 2;
    if (kind == "storageOrSampledImage") return 3;
    if (kind == "uniformBuffer") return 4;
    if (kind == "storageBuffer") return 5;
    if (kind == "other" || kind == "buffer" || kind == "unknown") return 0;
    return 6;
}

uint32_t descriptorCount(const Module& m, uint32_t typeId)
{
    auto it = m.types.find(typeId);
    if (it == m.types.end())
        return 1;
    if (it->second.kind == Kind::Array)
    {
        auto c = m.constants.find(it->second.lengthId);
        return (c == m.constants.end()) ? 1u : static_cast<uint32_t>(c->second);
    }
    if (it->second.kind == Kind::RuntimeArray)
        return 0;  // 变长
    return 1;
}

const char* descriptorKind(const Module& m, uint32_t typeId)
{
    auto it = m.types.find(typeId);
    if (it == m.types.end())
        return "unknown";
    Kind k = it->second.kind;
    if (k == Kind::Array || k == Kind::RuntimeArray)
    {
        auto elem = m.types.find(it->second.a);
        if (elem != m.types.end())
            k = elem->second.kind;
    }
    if (k == Kind::SampledImage)
        return "combinedImageSampler";
    if (k == Kind::Image)
        return "storageOrSampledImage";
    if (k == Kind::Sampler)
        return "sampler";
    if (k == Kind::Struct)
        return "buffer";
    return "other";
}

uint32_t stageFlag(uint32_t executionModel)
{
    switch (executionModel)
    {
    case 0:  // Vertex
        return 0x00000001u;
    case 1:  // TessellationControl
        return 0x00000002u;
    case 2:  // TessellationEvaluation
        return 0x00000004u;
    case 3:  // Geometry
        return 0x00000008u;
    case 4:  // Fragment
        return 0x00000010u;
    case 5:  // GLCompute
        return 0x00000020u;
    default:
        return 0;
    }
}

std::string typeName(const Module& m, uint32_t id)
{
    auto it = m.types.find(id);
    if (it == m.types.end())
        return "?";
    const TypeInfo& t = it->second;
    switch (t.kind)
    {
    case Kind::Bool:
        return "bool";
    case Kind::Int:
        return t.b == 64u ? "int64_t" : (t.b == 16u ? "int16_t" : "int32_t");
    case Kind::Float:
        return t.b == 64u ? "double" : (t.b == 16u ? "half" : "float");
    case Kind::Vector:
        return typeName(m, t.a) + std::to_string(t.b);
    case Kind::Matrix:
        return typeName(m, t.a) + "x" + std::to_string(t.b);
    case Kind::Array:
    {
        auto c = m.constants.find(t.lengthId);
        const std::string count =
            (c == m.constants.end()) ? std::string("?") : std::to_string(static_cast<uint32_t>(c->second));
        return typeName(m, t.a) + "[" + count + "]";
    }
    case Kind::RuntimeArray:
        return typeName(m, t.a) + "[]";
    case Kind::Struct:
    {
        auto n = m.names.find(id);
        return (n == m.names.end() || n->second.empty()) ? std::string("struct") : n->second;
    }
    case Kind::SampledImage:
        return "sampledImage";
    case Kind::Image:
        return "image";
    case Kind::Sampler:
        return "sampler";
    default:
        return "?";
    }
}

void emitBlock(std::ostream& out,
               const Module& m,
               uint32_t set,
               uint32_t binding,
               const std::string& id,
               uint32_t structId,
               const char* kindText)
{
    auto typeIt = m.types.find(structId);
    if (typeIt == m.types.end() || typeIt->second.kind != Kind::Struct)
        return;

    const bool runtime = hasRuntimeArray(m, structId);
    const uint32_t size = runtime ? 0u : typeSize(m, structId);
    auto structName = m.names.find(structId);

    out << "\n// ---- ";
    if (binding != 0xFFFFFFFFu)
        out << "set = " << set << " / binding = " << binding << " / ";
    out << kindText;
    if (structName != m.names.end() && !structName->second.empty())
        out << "（" << structName->second << "）";
    out << " ----\n";
    out << "// set / binding 见上面的 " << id << " 描述符常量，这里只给布局。\n";
    if (runtime)
    {
        out << "inline constexpr uint32_t k" << id << "BlockSize = 0;  // 含运行期数组，大小由使用方决定\n";
        out << "inline constexpr bool k" << id << "VariableSized = true;\n";
    }
    else
    {
        out << "inline constexpr uint32_t k" << id << "BlockSize = " << size << ";\n";
    }

    for (size_t k = 0; k < typeIt->second.members.size(); ++k)
    {
        const uint32_t memberType = typeIt->second.members[k];
        std::string memberName;
        auto mn = m.memberNames.find(std::make_pair(structId, static_cast<uint32_t>(k)));
        if (mn != m.memberNames.end() && !mn->second.empty())
            memberName = mn->second;
        else
            memberName = "member" + std::to_string(k);
        auto off = m.memberOffset.find((structId << 16) | (static_cast<uint32_t>(k) & 0xFFFFu));
        if (off == m.memberOffset.end())
            continue;
        const std::string memberId = id + pascal(memberName);
        out << "inline constexpr uint32_t k" << memberId << "Offset = " << off->second
            << ";  // " << typeName(m, memberType) << " " << memberName << "\n";

        auto mt = m.types.find(memberType);
        if (mt != m.types.end() && mt->second.kind == Kind::Array)
        {
            auto c = m.constants.find(mt->second.lengthId);
            if (c != m.constants.end())
                out << "inline constexpr uint32_t k" << memberId << "Count = "
                    << static_cast<uint32_t>(c->second) << ";\n";
        }
    }
}

bool emit(const Module& m, const std::string& outputPath, const std::string& ns, const std::string& shaderName)
{
    std::vector<Descriptor> descriptors;
    std::vector<const Variable*> buffers;  // uniform / storage buffer
    std::vector<const Variable*> pushConstants;

    for (const Variable& v : m.variables)
    {
        auto bindIt = m.bindingOf.find(v.id);
        if (bindIt != m.bindingOf.end())
        {
            Descriptor d;
            d.binding = bindIt->second;
            auto setIt = m.setOf.find(v.id);
            d.set = (setIt == m.setOf.end()) ? 0u : setIt->second;
            d.count = descriptorCount(m, v.typeId);
            d.name = v.name.empty() ? ("binding" + std::to_string(d.binding)) : v.name;
            if (v.storageClass == 2u)
                d.kind = "uniformBuffer";
            else if (v.storageClass == 12u)
                d.kind = "storageBuffer";
            else
                d.kind = descriptorKind(m, v.typeId);
            d.kindCode = descriptorKindCode(d.kind);
            descriptors.push_back(d);
        }
        // 关键：带 binding 的 buffer 有两个身份 —— 描述符（set / binding）与块（大小 / 偏移）。
        // 所以这里不能像图像那样 continue，否则块布局段会漏掉最常见的 uniform buffer。
        if (v.storageClass == 2u || v.storageClass == 12u)
            buffers.push_back(&v);
        else if (v.storageClass == 9u)
            pushConstants.push_back(&v);
    }

    // 排序：先 set 再 binding，保证同一份 .spv 每次生成同样的字节。
    for (size_t i = 0; i + 1 < descriptors.size(); ++i)
    {
        for (size_t j = 0; j + 1 < descriptors.size() - i; ++j)
        {
            const Descriptor& a = descriptors[j];
            const Descriptor& b = descriptors[j + 1];
            if (a.set > b.set || (a.set == b.set && a.binding > b.binding))
                std::swap(descriptors[j], descriptors[j + 1]);
        }
    }

    std::ofstream out(outputPath, std::ios::binary);
    if (!out)
        return false;

    out << "// 本文件由 tools/SpirvReflect.cpp 从 " << shaderName << " 生成 —— 不要手改。\n";
    out << "//\n";
    out << "// 这里只放编译器给出的事实：set / binding / 数组长度 / 块的成员偏移与总大小。\n";
    out << "// C++ 侧用这些常量写描述符，再用 static_assert 校验自己那份结构体；\n";
    out << "// 两边不一致时构建失败，而不是等运行期验证层报「descriptor 类型不匹配」。\n";
    out << "#pragma once\n";
    out << "#include <cstdint>\n\n";
    out << "namespace tiny::reflect::" << ns << " {\n\n";
    out << "inline constexpr uint32_t kStage = " << stageFlag(m.executionModel)
        << ";  // VK_SHADER_STAGE_*_BIT\n";
    out << "inline constexpr char kEntryPoint[] = \"" << m.entryPoint << "\";\n";

    uint32_t lastSet = 0xFFFFFFFFu;
    for (const Descriptor& d : descriptors)
    {
        if (d.set != lastSet)
        {
            out << "\n// ---- set = " << d.set << " ----\n";
            lastSet = d.set;
        }
        const std::string id = pascal(d.name);
        out << "\n// " << d.kind << "\n";
        out << "inline constexpr uint32_t k" << id << "Set = " << d.set << ";\n";
        out << "inline constexpr uint32_t k" << id << "Binding = " << d.binding << ";\n";
        out << "inline constexpr uint32_t k" << id << "Count = " << d.count << ";\n";
        out << "inline constexpr uint32_t k" << id << "Kind = " << d.kindCode << ";  // " << d.kind
            << "（0 = 未映射，见 src/RHI/DescriptorReflect.h）\n";
    }

    for (const Variable* v : buffers)
    {
        const std::string id = pascal(v->name.empty() ? ("block" + std::to_string(v->id)) : v->name);
        auto setIt = m.setOf.find(v->id);
        auto bindIt = m.bindingOf.find(v->id);
        const uint32_t set = (setIt == m.setOf.end()) ? 0u : setIt->second;
        const uint32_t binding = (bindIt == m.bindingOf.end()) ? 0xFFFFFFFFu : bindIt->second;
        emitBlock(out, m, set, binding, id, v->typeId,
                  v->storageClass == 12u ? "storage buffer" : "uniform buffer");
    }

    for (const Variable* v : pushConstants)
    {
        const std::string id = pascal(v->name.empty() ? "pushConstant" : v->name);
        emitBlock(out, m, 0u, 0xFFFFFFFFu, id, v->typeId, "push constant");
    }

    out << "\n}  // namespace tiny::reflect::" << ns << "\n";
    out.flush();
    return out.good();
}
}  // namespace

static int runTool(Module& module, char** argv);

int main(int argc, char** argv)
{
    // CRT 断言默认不写 stderr（静默 abort，退出码 3），排查时非常难看清原因。
    // 构建期工具跑在构建输出里，把断言重定向到 stderr 才能看到具体是哪一条。
#if defined(_WIN32)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    if (argc < 4)
    {
        std::fprintf(stderr, "用法：SpirvReflect <input.spv> <output.reflect.h> <namespace>\n");
        return 2;
    }

    Module module;
    try
    {
        return runTool(module, argv);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[SpirvReflect] 异常：%s\n", e.what());
        return 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "[SpirvReflect] 未知异常\n");
        return 1;
    }
}

static int runTool(Module& module, char** argv)
{
    if (!parse(module, argv[1]))
    {
        std::fprintf(stderr, "[SpirvReflect] 无法解析 %s\n", argv[1]);
        return 1;
    }
    if (!emit(module, argv[2], sanitize(argv[3]), argv[3]))
    {
        std::fprintf(stderr, "[SpirvReflect] 无法写入 %s\n", argv[2]);
        return 1;
    }
    return 0;
}