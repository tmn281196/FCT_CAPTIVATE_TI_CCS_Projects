################################################################################
# Automatically-generated file. Do not edit!
################################################################################

SHELL = cmd.exe

# Each subdirectory must supply rules for building sources it contributes
%.obj: ../%.c $(GEN_OPTS) | $(GEN_FILES) $(GEN_MISC_FILES)
	@echo 'Building file: "$<"'
	@echo 'Invoking: MSP430 Compiler'
	"C:/ti/ccs2041/ccs/tools/compiler/ti-cgt-msp430_21.6.1.LTS/bin/cl430" -vmspx --code_model=small --data_model=small -O3 --opt_for_speed=0 --use_hw_mpy=F5 --include_path="C:/ti/ccs2041/ccs/ccs_base/msp430/include" --include_path="C:/Users/Com26/workspace_ccstheia/8TX4RX_BTN01" --include_path="C:/Users/Com26/workspace_ccstheia/8TX4RX_BTN01/driverlib" --include_path="C:/Users/Com26/workspace_ccstheia/8TX4RX_BTN01/driverlib/MSP430FR2xx_4xx" --include_path="C:/Users/Com26/workspace_ccstheia/8TX4RX_BTN01/mathlib" --include_path="C:/Users/Com26/workspace_ccstheia/8TX4RX_BTN01/captivate" --include_path="C:/Users/Com26/workspace_ccstheia/8TX4RX_BTN01/captivate/ADVANCED" --include_path="C:/Users/Com26/workspace_ccstheia/8TX4RX_BTN01/captivate/BASE" --include_path="C:/Users/Com26/workspace_ccstheia/8TX4RX_BTN01/captivate/COMM" --include_path="C:/Users/Com26/workspace_ccstheia/8TX4RX_BTN01/captivate_app" --include_path="C:/Users/Com26/workspace_ccstheia/8TX4RX_BTN01/captivate_config" --include_path="C:/ti/ccs2041/ccs/tools/compiler/ti-cgt-msp430_21.6.1.LTS/include" --advice:power="none" --advice:hw_config=all --define=__MSP430FR2633__ --define=TARGET_IS_MSP430FR2XX_4XX -g --gcc --printf_support=minimal --diag_warning=225 --diag_wrap=off --display_error_number --silicon_errata=CPU21 --silicon_errata=CPU22 --silicon_errata=CPU40 --preproc_with_compile --preproc_dependency="$(basename $(<F)).d_raw" $(GEN_OPTS__FLAG) "$<"
	@echo 'Finished building: "$<"'
	@echo ' '


