#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* 하드웨어 세부 사항은 [8254] 8254 타이머 칩을 참조하세요. */
#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* OS 부팅 이후 타이머 틱의 수. */
static int64_t ticks;

/* 타이머 틱당 루프 수.
   timer_calibrate()에 의해 초기화됩니다. */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);


/* 8254 Programmable Interval Timer (PIT)을 설정하여
   초당 PIT_FREQ 번 인터럽트하고, 해당 인터럽트를 등록합니다. */
void
timer_init (void) {
	/* 8254 입력 주파수를 TIMER_FREQ로 나누고, 
	   가장 가까운 수로 반올림합니다. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: 카운터 0, LSB 다음에 MSB, 모드 2, 바이너리. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}


/* loops_per_tick를 보정하여 짧은 지연을 구현하는데 사용합니다. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* loops_per_tick를 타이머 틱보다 작은 가장 큰 2의 거듭제곱으로 근사합니다. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* loops_per_tick의 다음 8비트를 미세 조정합니다. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}


/* OS 부팅 이후 타이머 틱의 수를 반환합니다. */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}


/* THEN 이후에 경과한 타이머 틱의 수를 반환합니다.
   THEN은 timer_ticks()에 의해 한 번 반환된 값이어야 합니다. */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/*-------------------------------------------------------------------------------------------------*/

/* 대략적으로 타이머 틱을 대기합니다. */
void timer_sleep (int64_t ticks) {
    ASSERT (intr_get_level () == INTR_ON);  		// 인터럽트가 활성화되어 있는지 확인합니다.

    int64_t wake_up_time = timer_ticks() + ticks;  	// 깨어날 시간을 계산합니다.

    thread_sleep(wake_up_time);  					// 현재 쓰레드를 잠재웁니다.
}

/* 타이머 인터럽트 핸들러. */
static void timer_interrupt (struct intr_frame *args UNUSED) {
    ticks++; 				// 시스템 전체의 타이머 틱 수를 증가시킵니다.
    thread_tick (); 		// 현재 스레드에 대한 타이머 틱 처리를 수행합니다. 

    // 깨어날 시간이 지난 쓰레드를 깨웁니다. 즉, 각 스레드의 wake_up_time을 확인하여
	// 현재 시간(ticks)이 그 시간 이후라면 해당 스레드를 깨우는 처리를 수행합니다.
    thread_wakeup(ticks);
	
}

/*-------------------------------------------------------------------------------------------------*/

/* 대략적으로 MS 밀리초 동안 실행을 중단합니다. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}


/* 대략적으로 US 마이크로초 동안 실행을 중단합니다. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}


/* 대략적으로 NS 나노초 동안 실행을 중단합니다. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* 타이머 통계를 출력합니다. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* LOOPS 반복이 하나 이상의 타이머 틱을 기다리면 true를 반환하고, 
그렇지 않으면 false를 반환합니다. */
static bool
too_many_loops (unsigned loops) {
	/* 타이머 틱을 기다립니다. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* LOOPS 회 반복합니다. */
	start = ticks;
	busy_wait (loops);

	/* 틱 수가 변경되면, 우리는 너무 오래 반복했습니다. */
	barrier ();
	return start != ticks;
}


/* 간단한 루프를 LOOPS 번 반복하여
   짧은 지연을 구현합니다.

   코드 정렬이 타이밍에 상당히 영향을 미치므로,
   이 함수가 다른 위치에서 다르게 인라인 될 경우 
   결과를 예측하기 어렵기 때문에 NO_INLINE으로 표시했습니다. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}


/* 대략 NUM/DENOM 초 동안 잠자기. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* NUM/DENOM 초를 타이머 틱으로 변환하고, 내림하여 반올림합니다.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM 틱.
	   1 s / TIMER_FREQ 틱
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* 우리는 적어도 하나 이상의 전체 타이머 틱을 기다리고 있습니다. 
		다른 프로세스에 CPU를 양보하므로 timer_sleep()을 사용하십시오. */
		timer_sleep (ticks);
	} else {
		/* 그렇지 않으면, 더 정확한
		부분 틱 타이밍을 위해 바쁜 대기 루프를 사용합니다. 
		오버플로우 가능성을 피하기 위해 분자와 분모를 
		1000으로 나누어 축소합니다. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}

