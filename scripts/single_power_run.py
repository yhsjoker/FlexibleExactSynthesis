# import sys
# import os
# import paramiko
# import re

# # ================= 配置区域 =================
# VM_IP = "127.0.0.1"
# VM_PORT = 2222
# VM_USER = "joker"
# VM_PASS = "joker"
# VM_WORK_DIR = "/home/joker/remote_work"
# # ===========================================

# def parse_blif_inputs(blif_path):
#     input_names = []
#     try:
#         with open(blif_path, 'r') as f:
#             content = f.read()
#         content = re.sub(r"#.*", "", content)
#         match = re.search(r"\.inputs\s+(.*?)(?=\n\.)", content, re.DOTALL)
#         if match:
#             raw_inputs = match.group(1).replace("\\", " ").replace("\n", " ")
#             input_names = [name.strip() for name in raw_inputs.split() if name.strip()]
#     except Exception as e:
#         sys.stderr.write(f"[Python Error] BLIF Parse: {e}\n")
#     return input_names

# def generate_activity_tcl(act_file_path, blif_file_path, output_tcl_path):
#     try:
#         pin_names = parse_blif_inputs(blif_file_path)
#         if not pin_names: return False
#         activity_data = []
#         if act_file_path and os.path.exists(act_file_path):
#             with open(act_file_path, 'r') as f:
#                 for line in f:
#                     parts = re.findall(r"[-+]?\d*\.\d+|\d+", line)
#                     if len(parts) >= 2:
#                         activity_data.append((parts[0], parts[1]))
#         with open(output_tcl_path, 'w') as f:
#             f.write("set_default_switching_activity -input_activity 0.1 -duty 0.5\n")
#             for name, (prob, act) in zip(pin_names, activity_data):
#                 safe_name = name.replace("[", r"\[").replace("]", r"\]")
#                 f.write(f'set_switching_activity -activity {act} -duty {prob} -input_port "{safe_name}"\n')
#         return True
#     except Exception as e:
#         sys.stderr.write(f"[Python Error] Generate TCL: {e}\n")
#         return False

# def main():
#     # 错误时的标准输出，供 C++ 解析
#     error_output = "-1.0 -1.0 -1.0 -1.0 -1.0 -1.0"
#     if len(sys.argv) < 2:
#         print(error_output)
#         return

#     local_blif_path = sys.argv[1]
#     local_act_path = sys.argv[2] if len(sys.argv) > 2 else None
#     filename = os.path.basename(local_blif_path)
#     module_name = filename.replace(".blif", "")
#     local_tcl_path = local_blif_path + ".generated.tcl"
    
#     if not generate_activity_tcl(local_act_path, local_blif_path, local_tcl_path):
#         print(error_output)
#         return

#     ssh = paramiko.SSHClient()
#     ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
#     full_output_log = ""

#     try:
#         ssh.connect(VM_IP, port=VM_PORT, username=VM_USER, password=VM_PASS, timeout=60)
#         sftp = ssh.open_sftp()
#         sftp.put(local_blif_path, f"{VM_WORK_DIR}/{filename}")
#         sftp.put(local_tcl_path, f"{VM_WORK_DIR}/{module_name}.act.tcl")
#         local_run_job = os.path.join(os.path.dirname(os.path.abspath(__file__)), "run_job.sh.bak")
#         sftp.put(local_run_job, f"{VM_WORK_DIR}/run_job.sh")
#         sftp.close()

#         run_cmd = f"chmod +x {VM_WORK_DIR}/run_job.sh; {VM_WORK_DIR}/run_job.sh {module_name}"
#         stdin, stdout, stderr = ssh.exec_command(run_cmd, get_pty=True)
        
#         # 【核心改动】所有 Innovus 的日志都输出到 sys.stderr
#         # 这样你在终端运行能看到，但 C++ 的 popen 会忽略它们
#         for line in iter(stdout.readline, ""):
#             sys.stderr.write(line)
#             sys.stderr.flush()
#             full_output_log += line
#             if '--- Ending "Innovus"' in line:
#                 break
#         ssh.close()
#     except Exception as e:
#         sys.stderr.write(f"\n[SSH Error] {e}\n")
#         print(error_output)
#         return
#     finally:
#         if os.path.exists(local_tcl_path): os.remove(local_tcl_path)

#     p_total = p_int = p_sw = p_lk = area = delay = -1.0
#     try:
#         m = re.search(r"Total Power:\s+([0-9\.e\-]+)", full_output_log)
#         if m: p_total = float(m.group(1))
#         m = re.search(r"Total Internal Power:\s+([0-9\.e\-]+)", full_output_log)
#         if m: p_int = float(m.group(1))
#         m = re.search(r"Total Switching Power:\s+([0-9\.e\-]+)", full_output_log)
#         if m: p_sw = float(m.group(1))
#         m = re.search(r"Total Leakage Power:\s+([0-9\.e\-]+)", full_output_log)
#         if m: p_lk = float(m.group(1))
#         m = re.search(r"top\s+\d+\s+([0-9\.]+)", full_output_log)
#         if m: area = float(m.group(1))
#         m = re.search(r"-\s+Arrival Time\s+([0-9\.e\-]+)", full_output_log)
#         if m: delay = float(m.group(1))
#     except:
#         pass

#     # 【唯一留在 stdout 的内容】
#     print(f"{p_total} {p_int} {p_sw} {p_lk} {area} {delay}")

# if __name__ == "__main__":
#     main()

import sys
import os
import paramiko
import re
from pathlib import Path

# ================= 配置区域 =================
VM_IP = "127.0.0.1"
VM_PORT = 2222
VM_USER = "joker"
VM_PASS = "joker"
VM_WORK_DIR = "/home/joker/remote_work"
# ===========================================

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SSH_DEBUG_LOG_DIR = PROJECT_ROOT / "logs" / "ssh_debug"

def parse_blif_inputs(blif_path):
    input_names = []
    try:
        with open(blif_path, 'r') as f:
            content = f.read()
        content = re.sub(r"#.*", "", content)
        match = re.search(r"\.inputs\s+(.*?)(?=\n\.)", content, re.DOTALL)
        if match:
            raw_inputs = match.group(1).replace("\\", " ").replace("\n", " ")
            input_names = [name.strip() for name in raw_inputs.split() if name.strip()]
    except Exception as e:
        sys.stderr.write(f"[Python Error] BLIF Parse: {e}\n")
    return input_names

def generate_activity_tcl(act_file_path, blif_file_path, output_tcl_path):
    try:
        pin_names = parse_blif_inputs(blif_file_path)
        if not pin_names: return False
        activity_data = []
        if act_file_path and os.path.exists(act_file_path):
            with open(act_file_path, 'r') as f:
                for line in f:
                    parts = re.findall(r"[-+]?\d*\.\d+|\d+", line)
                    if len(parts) >= 2:
                        activity_data.append((parts[0], parts[1]))
        with open(output_tcl_path, 'w') as f:
            f.write("set_default_switching_activity -input_activity 0.1 -duty 0.5\n")
            for name, (prob, act) in zip(pin_names, activity_data):
                safe_name = name.replace("[", r"\[").replace("]", r"\]")
                f.write(f'set_switching_activity -activity {act} -duty {prob} -input_port "{safe_name}"\n')
        return True
    except Exception as e:
        sys.stderr.write(f"[Python Error] Generate TCL: {e}\n")
        return False

def main():
    # 错误时的标准输出，供 C++ 解析
    error_output = "-1.0 -1.0 -1.0 -1.0 -1.0 -1.0"
    if len(sys.argv) < 2:
        print(error_output)
        return

    local_blif_path = sys.argv[1]
    local_act_path = sys.argv[2] if len(sys.argv) > 2 else None
    filename = os.path.basename(local_blif_path)
    module_name = filename.replace(".blif", "")
    local_tcl_path = local_blif_path + ".generated.tcl"
    
    # 调试日志统一收纳到项目根目录下的 logs/ssh_debug。
    SSH_DEBUG_LOG_DIR.mkdir(parents=True, exist_ok=True)
    debug_log_path = SSH_DEBUG_LOG_DIR / f"debug_ssh_{module_name}.log"

    if not generate_activity_tcl(local_act_path, local_blif_path, local_tcl_path):
        print(error_output)
        return

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    full_output_log = ""

    try:
        ssh.connect(VM_IP, port=VM_PORT, username=VM_USER, password=VM_PASS, timeout=60)
        sftp = ssh.open_sftp()
        sftp.put(local_blif_path, f"{VM_WORK_DIR}/{filename}")
        sftp.put(local_tcl_path, f"{VM_WORK_DIR}/{module_name}.act.tcl")
        local_run_job = os.path.join(os.path.dirname(os.path.abspath(__file__)), "run_job.sh.bak")
        sftp.put(local_run_job, f"{VM_WORK_DIR}/run_job.sh.bak")
        sftp.close()

        # 🚀 包装 bash -c 以确保环境变量 (如 Innovus 路径) 生效
        run_cmd = f"bash -c 'chmod +x {VM_WORK_DIR}/run_job.sh.bak && {VM_WORK_DIR}/run_job.sh.bak {module_name}'"
        stdin, stdout, stderr = ssh.exec_command(run_cmd, get_pty=True)
        
        for line in iter(stdout.readline, ""):
            full_output_log += line
            if '--- Ending "Innovus"' in line:
                break
        ssh.close()
    except Exception as e:
        with open(debug_log_path, "w") as f:
            f.write(f"[Python SSH Exception] {e}\n")
        print(error_output)
        return
    finally:
        if os.path.exists(local_tcl_path): os.remove(local_tcl_path)

    # 🚀 【核心改动 1】强制保存远端回传的所有日志
    try:
        with open(debug_log_path, "w") as f:
            f.write(full_output_log)
    except:
        pass

    p_total = p_int = p_sw = p_lk = area = delay = -1.0
    try:
        m = re.search(r"Total Power:\s+([0-9\.e\-]+)", full_output_log)
        if m: p_total = float(m.group(1))
        
        m = re.search(r"Total Internal Power:\s+([0-9\.e\-]+)", full_output_log)
        if m: p_int = float(m.group(1))
        
        m = re.search(r"Total Switching Power:\s+([0-9\.e\-]+)", full_output_log)
        if m: p_sw = float(m.group(1))
        
        m = re.search(r"Total Leakage Power:\s+([0-9\.e\-]+)", full_output_log)
        if m: p_lk = float(m.group(1))
        
        # 🚀 【核心改动 2】面积正则修复：不再硬编码 "top"，适配真实模块名
        area_pattern = r"(?:top|" + re.escape(module_name) + r")\s+\d+\s+([0-9\.]+)"
        m = re.search(area_pattern, full_output_log, re.IGNORECASE)
        # 如果上一个没抓到，尝试泛用型抓取（报告分界线后的第一行数据）
        if not m:
            m = re.search(r"-\n\s*\S+\s+\d+\s+([0-9\.]+)", full_output_log)
        if m: area = float(m.group(1))
        
        m = re.search(r"-\s+Arrival Time\s+([0-9\.e\-]+)", full_output_log)
        if m: delay = float(m.group(1))
    except:
        pass

    # 【唯一留在 stdout 的内容，安全输送给 C++】
    print(f"{p_total} {p_int} {p_sw} {p_lk} {area} {delay}")

if __name__ == "__main__":
    main()
