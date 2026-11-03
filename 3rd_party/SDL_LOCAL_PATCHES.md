# SDL 本地修补

2026-10-29，经用户明确授权，仅应用于 Solongo 内置 SDL，不向 SDL 上游提交。

`SDL/src/video/windows/SDL_windowsmessagebox.c` 的两条 Windows 路径统一支持关闭但不选择按钮：

- TaskDialog：允许取消，无 Escape 默认按钮时关闭映射为 `-1`；有 Escape 默认按钮时保留原映射。
- DialogBox 回退：增加 `WS_SYSMENU`，使现有 `IDCLOSED` 处理可由标题栏 X 触发。

Solongo Debug 不指定 Escape 默认按钮，因此 `取消` 是普通按钮文字，X 是 Lua `nil`。
生产代码仍使用 `SDL_ShowMessageBox`，没有另一套原生弹窗框架。
人工/桌面自动化入口：`tests/debug/lua/Interactive.lua`。替换 SDL 版本时应重新审查这三行及实际关闭行为。
