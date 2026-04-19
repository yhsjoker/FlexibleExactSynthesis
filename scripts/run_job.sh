#!/bin/bash
# 路径: /home/joker/remote_work/run_job.sh

# =========================================================
# 【配置区域】
# =========================================================
export LM_LICENSE_FILE=/home/joker/cadence/installs/License/cadence.dat
export CDS_LIC_FILE=$LM_LICENSE_FILE
INNOVUS_BIN="/home/joker/cadence/installs/INNOVUS201/bin/innovus"

source /home/joker/.bashrc 2>/dev/null || true

MODULE_FILENAME=$1
WORK_DIR="/home/joker/remote_work"

LIB_FILE="${WORK_DIR}/lib/NanGate_15nm_OCL_typical_conditional_nldm.lib"
TECH_LEF="${WORK_DIR}/lib/NanGate_15nm_OCL.tech.lef"
MACRO_LEF="${WORK_DIR}/lib/NanGate_15nm_OCL.macro.lef"

cd $WORK_DIR

# =========================================================
# 1. Yosys 综合 (保持原有逻辑)
# =========================================================
yosys -p "
read_liberty -lib $LIB_FILE;
read_blif ${MODULE_FILENAME}.blif;
hierarchy -check -auto-top;
synth -auto-top -noabc; 
abc -D 100000 -liberty $LIB_FILE; 
opt_clean;
write_verilog -noattr top.v
" > yosys.log 2>&1

# =========================================================
# 2. 生成 Innovus TCL (最稳健版本)
# =========================================================
INNOVUS_SCRIPT="top.run.tcl"
cat <<EOF > $INNOVUS_SCRIPT
setMultiCpuUsage -localCpu 2
set init_mmmc_file mmmc.view
set init_lef_file [list $TECH_LEF $MACRO_LEF]
set init_verilog  top.v
set init_top_cell top
set init_pwr_net  VDD
set init_gnd_net  VSS
init_design

if {[file exists ${MODULE_FILENAME}.act.tcl]} {
    source ${MODULE_FILENAME}.act.tcl
} else {
    set_default_switching_activity -input_activity 0.1 -duty 0.5
}

# 建立时序图
catch { buildTimingGraph }
catch { timeDesign -prePlace }

# 功耗分析 (你之前验证过这部分能正常出 log)
setPowerAnalysisMode -method static -corner max
report_power

# 🚀 面积抓取：既然 report_area 经常不打印，我们用这种最土但最准的方法
# 遍历所有实例，把面积累加起来并手动打印一个标签
set total_area 0
set insts [dbGet top.insts]
foreach inst \$insts {
    set a [dbGet \$inst.cell.area]
    set total_area [expr \$total_area + \$a]
}
puts "MY_FINAL_AREA_RESULT: \$total_area"

report_timing
exit
EOF

# =========================================================
# 3. 运行 Innovus (不重定向，让 stdout 自由飞翔)
# =========================================================
$INNOVUS_BIN -nowin -init $INNOVUS_SCRIPT -log innovus.log -overwrite

# 清理
killall -9 -u joker cdsNameServer oa_server 2>/dev/null || true
exit 0