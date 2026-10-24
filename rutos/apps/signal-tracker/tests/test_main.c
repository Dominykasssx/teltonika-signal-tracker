#include "harness.h"

void run_unit_tests(void);
void run_scenario_tests(void);
void run_firmware_tests(void);

int main(void)
{
	harness_init();
	run_unit_tests();
	run_scenario_tests();
#ifdef WITH_FIRMWARE_TESTS
	run_firmware_tests();
#endif
	return harness_report();
}
