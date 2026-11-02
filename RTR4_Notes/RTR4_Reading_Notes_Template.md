RTR4 作为计算机图形学界的“圣经”，内容特点是**硬核数学公式多、专业术语密、紧跟工业界前沿（如现代 GPU 架构、PBR、光追等）**。因此，普通的通用笔记模板并不适用。我为你设计的这份模板加入了大量图形学专属模块。

### 💡 模板核心亮点设计：
1. **核心概念与术语表 (Glossary)：** 图形学有很多简称（BRDF, BSSRDF, TAA, HZB 等），看书时随手填入表格，久而久之就拥有了你私人的图形学字典。
2. **数学原理与推导 (LaTeX 支持)：** 内置了标准的 LaTeX 渲染示例（如经典的渲染方程渲染）。在支持 LaTeX 的编辑器中能完美展现优雅的公式，方便你拆解每个物理变量的含义。
3. **管线与算法流程 (Mermaid 流程图)：** 内置了 Markdown 原生支持的 `Mermaid` 流程图语法，你可以直接用纯文本画出渲染管线、数据流转或某种 Culling（剔除）算法的逻辑。
4. **工业界引擎对应 (Practice)：** 读 RTR4 最忌讳“死读理论”。专门留出模块让你把书中的理论与 **Unreal Engine / Unity / DirectX 12 / Vulkan** 的实际落地技术（如 Nanite, VSM, Cluster Shading）进行对照。

### 📂 目录结构：
文件夹结构：
```text
📂 RTR4_Notes/
├── 📄 RTR4_Reading_Notes_Template.md
├── 📂 chapters/                     
│   ├── 📄 Ch01....md
│   └── 📄 Ch02....md
└── 📂 images/                       
    ├── 📄 pbr_reflection.png
    └── 📄 shadow_map_bias.png