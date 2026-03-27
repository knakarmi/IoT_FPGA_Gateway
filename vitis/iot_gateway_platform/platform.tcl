# 
# Usage: To re-create this platform project launch xsct with below options.
# xsct C:\URI\ELE598\iot_gateway_project\vitis\iot_gateway_platform\platform.tcl
# 
# OR launch xsct and run below command.
# source C:\URI\ELE598\iot_gateway_project\vitis\iot_gateway_platform\platform.tcl
# 
# To create the platform in a different location, modify the -out option of "platform create" command.
# -out option specifies the output directory of the platform project.

platform create -name {iot_gateway_platform}\
-hw {C:\URI\ELE598\iot_gateway_project\vivado\bitstream\iot_gateway_top.xsa}\
-proc {ps7_cortexa9_0} -os {standalone} -out {C:/URI/ELE598/iot_gateway_project/vitis}

platform write
platform generate -domains 
platform active {iot_gateway_platform}
platform generate
