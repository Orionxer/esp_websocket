# ESP-IDF v5.5.3

通用规则：

1. 在 `tmux` 中执行相关操作时，尽可能复用已有的 `session`、`window` 和已激活的虚拟环境
2. 只有在无法复用时，才创建新的 `tmux session`、`window` 或重新激活虚拟环境
3. 如果当前处于 `idf.py monitor` 监控中，而用户又要求执行其他 `idf.py` 命令，则先发送 `Ctrl + ]` 退出 monitor，再执行对应的 `idf.py` 相关命令
4. 不允许执行命令：`idf.py fullclean`

当用户要求编译项目时，按以下流程执行：

1. 使用当前工作目录名作为 `tmux` session 名称；例如当前目录为 `my_websocket` 时，执行：`tmux new-session -d -s my_websocket`
2. 在该 `tmux` session 中激活虚拟环境：`source ~/.espressif/tools/activate_idf_v5.5.3.sh`
3. 等待虚拟环境激活成功后，再执行编译命令；不要用 `&&` 将激活和编译写在同一条命令中
4. 执行编译前，先执行 `clear`，避免旧输出干扰判断
5. 在虚拟环境中执行编译：`idf.py build`

当用户要求烧录项目、烧录固件或烧录时，按以下流程执行：

1. 确保当前 shell 已处于虚拟环境中
2. 执行烧录命令：`idf.py -b 6000000 flash`
3. 如果烧录失败，则把报错全部汇报出来
4. 如果烧录失败，同时提示用户可能有以下三种情况：
   1. 设备未挂载或挂载失败
   2. 设备已挂载但是没有读写权限
   3. 设备被其他进程占用
5. 如果烧录失败，再补充以下两个命令供用户排查或处理：
   `usbipd.exe attach --wsl --busid 4-3`
   `sudo chmod 666 /dev/ttyACM0 && ll /dev/ttyACM0`
6. 同时提示用户：以上命令中的 `busid 4-3` 和 `ttyACM0` 需要根据自己的实际情况修改

当用户要求分析大小、分析固件大小、分析内存占用、分析 RAM 或分析 ROM 时，按以下流程执行：

1. 确保当前 shell 已处于虚拟环境中
2. 执行命令：`idf.py size`
3. 不需要完整汇报 `idf.py size` 输出
4. 只需要简洁汇报当前的内存占用与固件大小信息给用户
5. 使用一个表格汇报，列为：`项目 | 总大小 | 当前大小 | 使用率 | 剩余大小`
6. 只保留两行：`固件分区` 和 `HP SRAM`
7. 单位规则如下：超过 `1 KB` 用 `KB`，超过 `1 MB` 用 `MB`

当用户要求监控串口、查看日志、启动监控或执行 monitor 时，按以下流程执行：

1. 确保当前 shell 已处于虚拟环境中
2. 执行命令：`idf.py monitor`
3. 尽可能在已有的 `tmux session` 或 `window` 中执行 monitor，避免重复创建新的会话
4. 如果 monitor 启动失败，则把报错全部汇报出来
5. 如果 monitor 启动失败，同时提示用户可能有以下三种情况：
   1. 设备未挂载或挂载失败 `usbipd.exe attach --wsl --busid 4-3`
   2. 设备已挂载但是没有读写权限 `sudo chmod 666 /dev/ttyACM0 && ll /dev/ttyACM0`
   3. 设备被其他进程占用 `lsof /dev/ttyACM0`
6. 同时提示用户：以上命令中的 `busid 4-3` 和 `ttyACM0` 需要根据自己的实际情况修改

当用户要求一键构建时，按以下流程执行：

1. 确保当前 shell 已处于虚拟环境中
2. 执行一键构建前，先执行 `clear`，避免旧输出干扰判断
3. 执行命令：`idf.py build && idf.py -b 6000000 flash && idf.py monitor`
4. 如果烧录失败，则把报错全部汇报出来
5. 如果烧录失败，同时提示用户可能有以下三种情况：
   1. 设备未挂载或挂载失败 `usbipd.exe attach --wsl --busid 4-3`
   2. 设备已挂载但是没有读写权限 `sudo chmod 666 /dev/ttyACM0 && ll /dev/ttyACM0`
   3. 设备被其他进程占用 `lsof /dev/ttyACM0`
6. 同时提示用户：以上命令中的 `busid 4-3` 和 `ttyACM0` 需要根据自己的实际情况修改
