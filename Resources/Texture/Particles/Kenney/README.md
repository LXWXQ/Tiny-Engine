# Kenney Particle Pack (CC0)

本目录的 14 张 sprite 来自 Kenney 的 **Particle Pack**：

- 上游页面：<https://kenney.nl/assets/particle-pack>
- 镜像/下载页：<https://opengameart.org/content/particle-pack-80-sprites>
- 作者：Kenney Vleugels（[kenney.nl](https://kenney.nl)）
- 许可：**Creative Commons Zero (CC0 1.0)**，可商用、可修改、无需署名；
  原始许可见同目录 [License.txt](License.txt)。

## 本目录保留的 sprite

| 文件 | 用途建议 |
|---|---|
| fire_01.png / fire_02.png | 火焰柱、篝火 |
| flame_01.png / flame_02.png / flame_05.png | 火球、爆发 |
| flare_01.png | 光斑、太阳耀斑 |
| magic_01.png | 魔法/能量 |
| smoke_01.png / smoke_04.png / smoke_06.png / smoke_08.png | 烟、尘土、蒸汽 |
| spark_01.png / spark_05.png | 火花、电火花 |
| star_01.png | 星形闪光 |

所有文件都是 512x512 PNG。引擎只读取 `Resources/Texture/Particles` 下的
图片，文件名可自由重命名；`ParticlePass` 启动时会递归扫描该目录（最多 64 张），
新增或替换文件后需要**重启**编辑器/引擎。

## alpha 转换说明

上游 PNG 是黑底灰度图，alpha 通道原本全为 255。为了让粒子 shader 直接使用
纹理 alpha 做透明，本目录的副本已就地转换为 RGBA：保留 RGB，令 **A = 亮度**。
这样黑色背景透明，火焰/烟雾的亮部保持不透明，边缘自然衰减。

若从上游重新下载并替换，请对每张图片执行一次同样的转换，例如：

```python
from PIL import Image

path = "Resources/Texture/Particles/Kenney/fire_01.png"
image = Image.open(path).convert("RGB")
rgba = image.copy()
rgba.putalpha(image.convert("L"))
rgba.save(path)
```

转换会覆盖原图；如要保留原始素材，请先备份。粒子组件的 `texture` 为空时仍使用
程序化圆形，不依赖本目录。
