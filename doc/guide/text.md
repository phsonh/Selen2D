# 普通文字

字体用 `Resource.LoadFont(name, path)` 或 Loader 加载并等到 Ready；字号属于每次文字调用。文字绘制与测量属于 `Graphic`，`Text` 模块仍负责 JSON/JSONC 和时长转换。

```lua
Graphic.SetTextAnchor("top_left")
Graphic.SetTextAlign("start")
Graphic.SetTextWritingMode("horizontal")
Graphic.SetTextLetterSpacing(0)
Graphic.SetTextLineSpacing(0)
Graphic.SetTextOutline(1, 0xFF202020)

local text = Graphic.GenTextWrap("ui_font", "普通文字\n第二行", 24, 400, 200)
if text then
    local width = Graphic.GetTextWidth("ui_font", text, 24)
    local height = Graphic.GetTextHeight("ui_font", text, 24)
    -- 在允许绘制的回调中，8 个显式参数均必填。
    Graphic.RenderText("ui_font", text, -200, 100, 24, 0xFFFFFFFF, "mul+alpha", 0)
    -- canvas:RenderText 使用相同参数和状态，不改变默认绘制目标。
end
```

六组状态由当前 Lua 运行实例共享，每组都有对应 Getter。Setter 返回零个值；普通 Getter 返回一个值，`GetTextOutline()` 返回宽度和颜色。`SetTextOutline(0)` 关闭描边并保留颜色；提供颜色时必须为 uint32 ARGB。颜色与宽度先一起校验，失败不会部分更新。锚点、颜色、位置和 layer 不改变测量结果。每次绘制保存当前状态，后续设置不影响已记录的命令。

Anchor 是 `top_left/top/top_right/left/center/right/bottom_left/bottom/bottom_right`，默认 `center`。Align 是 `start/center/end/left/right/top/bottom`，默认 `start`；横排不能使用 top/bottom，竖排不能使用 left/right，该组合在布局时检查。WritingMode 是 `horizontal/vertical_rl/vertical_lr`，默认横排。两个竖排模式均从上向下推进，分别向左或向右排列列。横排 start/end 按段落双向基方向解释；中性段落默认 LTR。

文字状态不会在帧首、Clear、目标切换、最小化或字体卸载时重置。字距与行距允许有限负数；实际默认行/列推进加行距必须大于零。字距加在塑形后安全的 cluster 边界，不拆开组合符、必要连接或连字。包络同时包含逻辑占位、空行、空格、字形外伸和描边，宽始终是横向跨度，高始终是纵向跨度。合法空串为 0×0；换行不是空串。

所有文字入口严格校验 UTF-8，保留 NUL 后面的字节，不执行路径 NFC 转换。CRLF/CR 归一为 LF；连续和末尾换行保留。Tab 按四个普通空格布局，但 Wrap 返回值仍保留 Tab。没有 HTML、字体回退、原生富文本、自动字号、3D 文字或文字独立旋转。缺字使用所选字体的 `.notdef`；不会查询系统字体。

`GenTextWrap` 返回一个完整字符串，放不下时返回一个 nil。它保留全部正文、空格和已有换行，只插入 LF；使用确定的最远可容纳合法断点，并重新塑形每个候选。必要时只在安全字素/连接边界紧急断行，NBSP 和 Word Joiner 不因此失效。负字距不采用单调宽度假设。最终结果再次用完整布局检查宽高；字体、编码、状态、内存或工作预算错误会抛错，不伪装成 nil。

描边宽度是逻辑单位中的外扩距离，采用真实轮廓的圆角外描边。正文与扩张轮廓使用相同采样网格，外环为 `max(expanded-fill, 0)`，避免透明正文被描边填实。正文和描边使用独立颜色及同一混合模式；透明描边仍影响逻辑包络。字体支持范围为单色轮廓型独立 TTF/OTF；当前文字使用阶段明确拒绝彩色图层字体、可变字体、字体集合和不可用轮廓。

实现固定 FreeType 2.14.1、HarfBuzz 12.3.0、SheenBidi 3.0.0、libunibreak 8.0 与 Unicode 17.0.0 数据，语言使用 `und`，不读取 OS locale。横排行框来自字体 ascent/descent，默认推进来自字体 height（无有效 height 时使用行框高度）。有有效 vhea 的竖排使用其 ascent/descent/line-gap；缺少有效垂直行度量时固定使用居中一 em 的列框和推进，字形竖排原点/advance 仍由 HarfBuzz 的 OpenType 度量与固定回退产生。CPU 度量使用未 hint 的设计单位，栅格尺度与逻辑字号分开。

布局缓存未命中及需要塑形的操作拥有独立的 FreeType face/library 与塑形状态，字体字节保持 Resource 原来的强引用和预算归属。布局和准备结果保留字体代次；同名重载不会替换已接受命令的字体。字形覆盖缓存按字体完整代次、glyph、字号、方向、描边宽度和栅格尺度区分。缓存淘汰不复用仍被命令引用的覆盖数据；GPU 上传与 fence 保活由 Renderer 承担。Width/Height/Wrap 不调用 GPU 或等待 Present。

`RenderText`、`GetTextWidth` 和 `GetTextHeight` 共用有界的内容布局缓存。键包含字体所有权与完整代次、原始文本、字号和全部文字状态（包括描边颜色）；命中仍检查当前字体及本次参数。相同内容可避免重复解码与塑形，不需要游戏维护额外句柄。缓存最多保留 512 项，按最近使用顺序淘汰；计入文本、布局容量、管理节点及去重后的字体字节，保留数据额度为 32 MiB。这是缓存的核算上限，不是整个进程的内存上限。超出单项额度或缓存分配失败时，正常布局仍可返回而不进入缓存。

字体卸载后，新调用仍须取得有效字体；缓存及已接受的绘制可暂时保留旧代次的不可变数据。布局缓存持有的引用在淘汰或文字上下文销毁时释放，已在使用的布局不失效。Wrap 算法保持原样；没有跨调用的 PreparedText 缓存。栅格尺度、Canvas 范围和绘制坐标仍在本次准备与绘制时处理。

GPU 字形资源命中通过索引定位后核对共享所有权；在途 lease 可复用，同一次文字的描边和填充复用已取得的资源。缓存本身不强行保留 lease 或 CPU 字形覆盖图；回收在帧/fence 边界及预算压力下执行。仍先绘制整段描边，再绘制整段填充，不按缓存位置改变文字顺序。

默认实现上限：输入 UTF-8 1 MiB，262,144 个 glyph，16,384 行/列；Wrap 最多 65,536 个候选及累计 64 MiB 候选塑形输入。单次 HarfBuzz 塑形缓冲上限 262,144 个点位/glyph，内部操作预算 8,388,608；超限明确报错。每个字体 Face 实例的 FreeType 分配器最多核算 32 MiB 同时存活的分配（含其分配头），不是每次操作单独获得 32 MiB；该额度不包含字体字节、HarfBuzz 或 C++ 容器内存。单 glyph 最大边 8,192 像素、覆盖数据 16 MiB，覆盖缓存 64 MiB/8,192 项。仍被 Draw 引用的覆盖数据计入缓存预算；不足时明确报错。临时新栅格最多额外占用一个 glyph 的准备额度，不等待正在引用缓存的调用自己释放。

CPU 度量与覆盖图验证不能替代实际平台上的 GPU 混合、绘制顺序和 DPI 验证。
