# STD 目录结构

src/
├── basic/        # 基础共用类型、错误处理等
├── collections/  # 集合及容器，Map/Set/List/Span 等数据结构
├── fs/           # 文件系统，专职搞定文件、目录控制、路径切片
├── io/           # 核心 IO 契约定义、比如 Stream，以及核心 Stdio
├── net/          # 网络，套接字、IP寻址、DNS、TCP/UDP会话生命周期
├── numeric/      # 标量扩展、数值计算、基础数学操作
├── platform/     # 宿主环境相关（OS, Arch, CPU核心数静态常量）
├── process/      # 进程（PID, 环境变量, 信号捕获, 派生子进程）
├── text/         # String 类型扩展、零拷贝的 StringSpan 视窗、高效切片与解析工具
├── thread/       # 进程内共享内存的多线程、锁与异步状态通知
└── time/         # 单调纳秒时钟、自研纯 feng 历法公式
