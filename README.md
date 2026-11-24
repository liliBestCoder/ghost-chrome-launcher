# Chrome 多开

Ghost Proxifier 原生 Windows x64 插件。每个实例对应一个 Ghost Target，由宿主保存别名和上游节点，并用 Chrome 的 `--user-data-dir` 参数启动。插件会阻止多个自有 Target 配置同一个数据目录。

## 使用

1. 在支持 v1 Target API 的 Ghost Proxifier 1.2.2 或更高版本中，打开插件开发者模式，在插件中心选择「从本地安装」，导入 `com.ghostproxifier.chrome-launcher-1.1.1.gpkg`。升级时直接导入新版包即可，已有实例保存在 Ghost Target 列表中。
2. 启用插件，授予 Target 管理、上游管理和代理出网权限，并勾选该插件可以使用的上游节点。
3. 打开「Chrome 多开」，点击「添加实例」。插件在 Program Files、Program Files (x86) 和 LocalAppData 下检查常见安装路径。找到 `chrome.exe` 时会自动填入，可以手动修改。填写 Target 别名和独立数据目录，选择上游节点或直连，然后保存。
4. 在实例列表点击「启动」。如果宿主弹出启动审批，请在 Ghost 窗口确认。运行中的实例可点「停止」，由 Ghost 结束该 Target 的进程。停止后可以编辑目录、别名或节点。列表在页面可见时自动同步。

数据目录填写 Windows 本地绝对路径，例如 `D:\Chrome Profiles\Work`。应为每个实例使用专用目录，避免 Chrome 将启动请求交给已经占用该目录的会话。配置保存在 Ghost 的 Target 列表，Chrome 自行管理用户数据目录。

## 构建本地安装包

在仓库根目录用 Visual Studio 2022 C++ 工具和项目打包脚本执行以下命令。可执行程序使用静态 C++ 运行库。

```powershell
cmake -S . -B build/chrome-launcher -G "Visual Studio 17 2022" -A x64
cmake --build build/chrome-launcher --config Release
cmake --install build/chrome-launcher --config Release --prefix build/chrome-launcher/stage
python tools/plugin/gpkg.py pack --src build/chrome-launcher/stage --out build/chrome-launcher/dist --min-app-version 1.2.2
```

打包后的 `.gpkg` 在 `build/chrome-launcher/dist`。C++ 进程负责目录校验、Ghost API 调用和宿主握手，嵌入式面板通过当前进程的回环地址访问它。
