
    import time
    import subprocess
    import os
    
    print("正在监控 FileSearchFastly 扫描进度...")
    print("="*60)
    
    while True:
        try:
            # 检查进程是否存在
            result = subprocess.run(
                ["tasklist", "/FI", "IMAGENAME eq FileSearchFastly.exe", "/FO", "CSV"],
                shell=True,
                capture_output=True,
                text=True,
                encoding="gbk"
            )
            
            if "FileSearchFastly.exe" in result.stdout:
                print(f"[{time.strftime('%H:%M:%S')}] ✅ FileSearchFastly.exe 正在运行")
                
                # 尝试读取可能的日志输出
                log_file = "H:/qtStudy/FileSearchFastly/output.log"
                if os.path.exists(log_file):
                    with open(log_file, "r", encoding="utf-8", errors="ignore") as f:
                        lines = f.readlines()
                        if lines:
                            print(f"   📝 日志文件有 {len(lines)} 行")
                            print(f"   最新内容: {lines[-1].strip()}")
            else:
                print(f"[{time.strftime('%H:%M:%S')}] ❌ FileSearchFastly.exe 已结束")
                break
            
            time.sleep(2)  # 每2秒检查一次
            
        except KeyboardInterrupt:
            print("\n监控已停止")
            break
        except Exception as e:
            print(f"错误: {e}")
            time.sleep(5)
    
    print("="*60)
    print("监控结束")
    