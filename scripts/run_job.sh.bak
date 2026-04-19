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
LIB_FILE="${WORK_DIR}/lib/NangateOpenCellLibrary_typical.lib"
LEF_FILE="${WORK_DIR}/lib/NangateOpenCellLibrary.lef"

if [ -z "$MODULE_FILENAME" ]; then
    echo "Error: Please provide a module name"
    exit 1
fi

cd $WORK_DIR

# =========================================================
# 1. 运行 Yosys (纯净低功耗工艺映射)
# =========================================================
yosys -p "
read_liberty -lib $LIB_FILE;
read_blif ${MODULE_FILENAME}.blif;
hierarchy -check -auto-top;
synth -auto-top -noabc; 
# [魔法点 1] -D 10000 极度放宽时序约束，强制 ABC 使用最低功耗的 X1 单元
abc -liberty $LIB_FILE -D 10000; 
opt_clean;
write_verilog -noattr ${MODULE_FILENAME}.v
" > yosys.log 2>&1

# =========================================================
# 2. 修正模块名
# =========================================================
REAL_TOP_NAME=$(grep -m 1 "^module" ${MODULE_FILENAME}.v | awk '{print $2}' | cut -d'(' -f1 | tr -d ';')
[ -z "$REAL_TOP_NAME" ] && REAL_TOP_NAME=$MODULE_FILENAME
if [ "$MODULE_FILENAME" != "$REAL_TOP_NAME" ]; then
    mv "${MODULE_FILENAME}.v" "${REAL_TOP_NAME}.v"
    [ -f "${MODULE_FILENAME}.act.tcl" ] && mv "${MODULE_FILENAME}.act.tcl" "${REAL_TOP_NAME}.act.tcl"
fi

# =========================================================
# 3. 动态生成 SDC (补全真实的物理时序特征)
# =========================================================
SDC_FILE="top.sdc"
cat <<EOF > $SDC_FILE
create_clock -name virtual_clk -period 10.0
# 加入翻转时间，为后续的 Slew 传播提供基准源头
set_input_transition 0.1 [all_inputs]
set_max_transition 0.5 [current_design]
set_input_delay 0.2 [all_inputs] -clock virtual_clk
set_output_delay 0.2 [all_outputs] -clock virtual_clk
EOF

# =========================================================
# 4. 生成 MMMC 视图
# =========================================================
MMMC_FILE="mmmc.view"
cat <<EOF > $MMMC_FILE
create_library_set -name typical_lib_set -timing [list $LIB_FILE]
create_constraint_mode -name default_constraint_mode -sdc_files [list $SDC_FILE]
create_delay_corner -name typical_corner -library_set typical_lib_set
create_analysis_view -name typical_view -constraint_mode default_constraint_mode -delay_corner typical_corner
set_analysis_view -setup {typical_view} -hold {typical_view}
EOF

# =========================================================
# 5. 生成主 TCL 脚本 (激活完整物理评估链)
# =========================================================
INNOVUS_SCRIPT="top.run.tcl"
cat <<EOF > $INNOVUS_SCRIPT
setMultiCpuUsage -localCpu 2
set init_mmmc_file $MMMC_FILE
set init_lef_file  $LEF_FILE
set init_verilog   ${REAL_TOP_NAME}.v
set init_top_cell  ${REAL_TOP_NAME}
set init_pwr_net   "VDD"
set init_gnd_net   "VSS"
init_design

# 显式激活统计线负载模型
set_wire_load_mode enclosed
EOF

# 读入翻转率或设置默认值
if [ -f "${REAL_TOP_NAME}.act.tcl" ]; then
    echo "source ${REAL_TOP_NAME}.act.tcl" >> $INNOVUS_SCRIPT
else
    echo "set_default_switching_activity -input_activity 0.1 -duty 0.5" >> $INNOVUS_SCRIPT
fi

# [魔法点 2] 强制建立时序图并执行 Pre-Place STA，让 PONO 优化出的优秀 Slew 传播到全网
cat <<EOF >> $INNOVUS_SCRIPT
buildTimingGraph
timeDesign -prePlace

# 配置静态功耗分析模式
setPowerAnalysisMode -method static -corner max -create_binary_db true -write_static_currents true

report_area
report_timing
report_power
exit
EOF

# =========================================================
# 6. 运行 Innovus
# =========================================================
echo "exit" | $INNOVUS_BIN -nowin -init $INNOVUS_SCRIPT -log innovus.log -overwrite

# 7. 清理僵尸进程并强制退出
cat innovus.log > /dev/null 2>&1
killall -9 -u joker cdsNameServer oa_server 2>/dev/null || true
exit 0