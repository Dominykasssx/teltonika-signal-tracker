#include "harness.h"

int test_failures;
int test_checks;
long long test_now;

static long long fake_clock(void)
{
	return test_now;
}

void harness_init(void)
{
	ev_set_clock(fake_clock);
}

void advance(long long ms)
{
	long long end = test_now + ms;

	for (;;) {
		int t = ev_next_timeout();

		if (t < 0 || test_now + t > end)
			break;
		test_now += t;
		ev_run_due();
	}
	test_now = end;
	ev_run_due();
}

int harness_report(void)
{
	printf("%d checks, %d failures\n", test_checks, test_failures);
	return test_failures ? 1 : 0;
}
