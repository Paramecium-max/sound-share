# SoundFlow v1.1 界面设计

依据本机 frontend-design、原对话保存的 apple-liquid-glass 技能和 Apple 2026-11-17 公开指南。Android WebView 实现的是视觉与交互适配，不是 UIKit 原生 UIGlassEffect。

主色：分组背景 #F2F2F7、内容 #FFFFFF、正文 #161619、次要文字 #68686F、主操作 #007AFF；深色内容 #1C1C1E。系统无衬线字族，Android 使用系统中文字体，不下载 SF。字号 34/28/22/17/15/13，正常与中等字重。

接收页以无线音箱插画与接收状态为中心。横屏图文并排，竖屏垂直排列；播放后歌词自动铺满屏幕。导航与操作只有一层浮动玻璃，内容为不透明分组表面。调谐面板和底部控制栏各占真实高度，控制栏不因弹出而移动。设置按歌词、接收、外观分组；平板采用两栏。控件触区至少 44 CSS px，主要控件 48 px。

审查：旧版满屏彩色模糊光球和半透明内容卡片破坏层级，也增加重合成开销，故移除。玻璃限于工具栏、页签、舞台控制和弹层。舞台为媒体背景采用轻量预绘光影；页面主体不使用持续背景动画。玻璃可在通透与着色间连续调整。支持减少动态、减少透明度、高对比及无 backdrop-filter 降级。

资料：
- https://www.apple.com/os/ios/ （iOS 27 对比度、折射和通透度滑块）
- https://developer.apple.com/design/human-interface-guidelines/materials （内容层与功能层、regular / clear）
- https://developer.android.com/reference/android/media/audiofx/Visualizer （仅 DLNA 可选频谱权限；不采集麦克风）
