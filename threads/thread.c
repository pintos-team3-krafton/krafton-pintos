#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif
#include "devices/timer.h"

/* 스레드 구조체의 'magic' 멤버에 대한 임의의 값.
스택 오버플로우를 감지하는 데 사용됩니다. 자세한 내용은 thread.h 상단의 큰 주석을 참조하십시오. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드에 대한 임의의 값
이 값을 수정하지 마세요. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태에 있는 프로세스들의 리스트입니다.
즉, 실행 가능하지만 현재 실행 중이지는 않은 프로세스들입니다. */
static struct list ready_list;

static struct list sleep_list;

/* Idle 스레드입니다. */
static struct thread *idle_thread;

/* 초기 스레드, init.c:main()을 실행하는 스레드입니다. */
static struct thread *initial_thread;

/* allocate_tid()에서 사용하는 락입니다. */
static struct lock tid_lock;

/* 스레드 소멸 요청 */
static struct list destruction_req;

/* 통계 정보 */
static long long idle_ticks; 		/* 유휴 상태에서 사용한 타이머 틱 수. */
static long long kernel_ticks; 		/* 커널 스레드에서 사용한 타이머 틱 수. */
static long long user_ticks; 		/* 사용자 프로그램에서 사용한 타이머 틱 수. */

/* 스케줄링 */
#define TIME_SLICE 4 				/* 각 스레드에 할당되는 타이머 틱 수. */
static unsigned thread_ticks; 		/* 마지막 양보 이후의 타이머 틱 수. */

/* 만약 false (기본값)이라면, 라운드로빈 스케줄러를 사용합니다.
true라면, 다단계 피드백 큐 스케줄러를 사용합니다.
커널의 "-o mlfqs" 커맨드라인 옵션으로 제어됩니다. */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);
void thread_sleep(int64_t tick);
void thread_wakeup(int64_t tick);
void change_list(void);
bool sort_priority(const struct list_elem *a, const struct list_elem *b, void *aux);
bool sort_wakeup_time(const struct list_elem *a, const struct list_elem *b, void *aux);
/* T가 유효한 스레드를 가리키는지 확인하고 true를 반환합니다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 실행 중인 스레드를 반환합니다.
1. CPU의 스택 포인터 `rsp'를 읽은 다음,
2. 페이지의 시작으로 내림해줍니다.
3. `struct thread'가 항상 페이지의 시작 부분에 위치하고,
스택 포인터는 중간 어딘가에 있으므로 현재 스레드를 찾습니다. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))

// thread_start를 위한 전역 서술자 테이블.
// gdt는 thread_init 이후에 설정될 예정이므로,
// 일단 임시 gdt를 설정해야 합니다.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };


/* 스레딩 시스템을 초기화합니다. 현재 실행 중인 코드를 스레드로 변환하는 작업입니다.
   이 작업은 일반적으로 동작하지 않지만, 이 경우에만 가능하며 
   loader.S 파일이 스택의 맨 아래를 페이지 경계에 맞추었습니다.

   실행 대기열(run queue)과 tid 락(tid lock)도 초기화합니다.

   이 함수를 호출한 후에는 스레드를 생성하기 전에 페이지 할당자를 초기화해야 합니다.

   이 함수가 완료될 때까지는 thread_current()를 호출하지 않는 것이 안전합니다. */
void
thread_init (void) {
    ASSERT (intr_get_level () == INTR_OFF);

    /* 커널을 위한 임시 GDT(Globl Descriptor Table)를 다시 로드합니다.
     * 이 GDT는 사용자 컨텍스트를 포함하지 않습니다.
     * 커널은 gdt_init() 함수에서 사용자 컨텍스트를 포함하여 GDT를 다시 구성합니다. */
    struct desc_ptr gdt_ds = {
        .size = sizeof (gdt) - 1,
        .address = (uint64_t) gdt
    };
    lgdt (&gdt_ds);

    /* 전역 스레드 컨텍스트를 초기화합니다. */
    lock_init (&tid_lock);
    list_init (&ready_list);
    list_init (&destruction_req);

	// alarm clockㅇ
	list_init (&sleep_list);

    /* 실행 중인 스레드를 위한 스레드 구조체를 설정합니다. */
    initial_thread = running_thread ();
    init_thread (initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid ();
}

/* 인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작하고, idle 스레드를 생성합니다. */
void
thread_start (void) {
    /* idle 스레드를 생성합니다. */
    struct semaphore idle_started;
    sema_init (&idle_started, 0);
    thread_create ("idle", PRI_MIN, idle, &idle_started);

    /* 선점형 스레드 스케줄링을 시작합니다. */
    intr_enable ();

    /* idle 스레드가 idle_thread를 초기화할 때까지 대기합니다. */
    sema_down (&idle_started);
}

/* 타이머 인터럽트 핸들러에 의해 각 타이머 틱마다 호출됩니다.
   따라서, 이 함수는 외부 인터럽트 컨텍스트에서 실행됩니다. */
void
thread_tick (void) {
    struct thread *t = thread_current ();

    /* 통계 정보를 업데이트합니다. */
    if (t == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (t->pml4 != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    /* 선점을 적용합니다. */
    if (++thread_ticks >= TIME_SLICE)
        intr_yield_on_return ();
}

/* 스레드 통계 정보를 출력합니다. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/*-------------------------------------------------------------------------------------------------*/


/* NAME이라는 이름을 가진 새로운 커널 스레드를 생성하고, 초기 우선순위를 설정합니다.
   이 스레드는 FUNCTION을 실행하며, AUX를 인자로 받습니다. 이 스레드는 준비 큐에 추가됩니다.
   이 함수는 새 스레드의 스레드 식별자를 반환하거나, 생성이 실패하면 TID_ERROR를 반환합니다.

   thread_start() 함수가 호출되었을 경우, 
   새 스레드는 thread_create()가 반환되기 이전에 스케줄링될 수 있습니다.
   심지어 새 스레드는 thread_create()가 반환되기 이전에 종료될 수도 있습니다.
   반면에, 원래 스레드는 새 스레드가 스케줄링되기 전까지 얼마든지 실행될 수 있습니다.
   순서를 보장하려면 세마포어나 다른 동기화 기법을 사용하십시오.

   제공된 코드는 새 스레드의 `priority' 멤버를 PRIORITY로 설정하지만, 
   실제로는 우선순위 스케줄링이 구현되어 있지 않습니다.
   우선순위 스케줄링은 Problem 1-3의 목표입니다. */
tid_t thread_create (const char *name, int priority, thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* 스레드 할당 */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* 스레드 초기화 */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* 스케줄링 되면 커널 스레드를 호출
	 * 참고) rdi는 첫 번째 인수이고, rsi는 두 번째 인수입니다. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* 실행 큐에 추가 */
	thread_unblock (t);

	/* 현재 실행 중인 스레드와 새로 삽입된 스레드의 우선 순위를 비교합니다. 
	새로 들어온 스레드가 더 높은 우선 순위를 가지고 있다면, CPU를 양보합니다 */
	 
	change_list();

	return tid;
}

/* 현재 스레드를 잠재웁니다. 
   스레드는 thread_unblock()에 의해 깨어날 때까지 다시 스케줄되지 않습니다.

   이 함수는 인터럽트가 꺼진 상태에서 호출되어야 합니다. 
   보통은 synch.h에 있는 동기화 기능을 사용하는 것이 더 나은 선택입니다. */
void thread_block (void) {
    ASSERT (!intr_context ());                   // 현재 코드가 인터럽트 핸들러 내부에서 실행되지 않음을 확인합니다.
    ASSERT (intr_get_level () == INTR_OFF);      // 현재 인터럽트가 비활성화되어 있음을 확인합니다.

    thread_current ()->status = THREAD_BLOCKED;  // 현재 스레드의 상태를 BLOCKED로 변경합니다.
                                                 // 이로 인해 스케줄러는 이 스레드를 실행 대기열에서 제외합니다.

    schedule ();                                 // 스케줄러를 호출하여 다음 실행할 스레드를 선택하고 실행합니다.
                                                 // 이 호출로 인해 현재 스레드의 실행이 중지되고, 선택된 스레드가 실행됩니다.
}

/* 차단된 스레드 T를 실행 준비 상태로 전환합니다.
   만약 T가 차단 상태가 아니라면 이는 오류입니다. 
   (실행 중인 스레드를 실행 준비 상태로 만들려면 thread_yield()를 사용하세요.)

   이 함수는 실행 중인 스레드를 선점하지 않습니다. 이는 중요한 점인데,
   만약 호출자가 스스로 인터럽트를 비활성화했다면, 
   스레드를 원자적으로 언블록하고 다른 데이터를 업데이트할 것으로 기대할 수 있습니다. */
void thread_unblock (struct thread *t) {

	enum intr_level old_level;

	ASSERT (is_thread (t));
	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	
	// list_push_back (&ready_list, &t->elem);
	list_insert_ordered(&ready_list, &t->elem, sort_priority, NULL);
	t->status = THREAD_READY;

	intr_set_level (old_level);

}

/*-------------------------------------------------------------------------------------------------*/


/* 실행 중인 스레드의 이름을 반환합니다. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* 실행 중인 스레드를 반환합니다.
   이 함수는 running_thread()에 몇 가지 정확성 검사를 더한 것입니다.
   자세한 내용은 thread.h 상단의 큰 주석을 참조하십시오. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* T가 정말로 스레드인지 확인합니다.
	   이 두 가지 주장 중 어느 것이라도 실패하면, 스레드가 스택을 넘칠 수 있습니다.
	   각 스레드는 4 kB 미만의 스택을 가지므로,
	   몇 개의 큰 자동 배열이나 중간 정도의 재귀는 스택 오버플로를 일으킬 수 있습니다. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* 실행 중인 스레드의 tid를 반환합니다. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* 현재 스레드를 비스케줄링하고 파괴합니다. 
   절대로 호출자에게 반환되지 않습니다. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* 우리의 상태를 죽는 상태로 설정하고 다른 프로세스를 스케줄링합니다.
	   우리는 schedule_tail()을 호출하는 동안 파괴될 것입니다. */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU를 양보합니다. 
   현재 스레드는 잠재우지 않고 스케줄러의 임의에 따라 즉시 다시 스케줄될 수 있습니다. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		// list_push_back (&ready_list, &curr->elem);
		list_insert_ordered (&ready_list, &curr->elem, sort_priority, NULL);
	// curr->status = THREAD_READY;
    // schedule ();

	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* 현재 스레드의 우선순위를 NEW_PRIORITY로 설정합니다. */
// 현재 스레드의 우선순위를 변경한 후에는 준비 큐(ready queue)를 재정렬해야 합니다. 
// 그 이유는 준비 큐에 있는 스레드들이 우선 순위에 따라 정렬되어야 하고, 
// 우선 순위가 변경되면 이 정렬 순서에 영향을 미치기 때문입니다.
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;

	change_list();


}

/* 현재 스레드의 우선순위를 반환합니다. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* 현재 스레드의 nice 값으로 NICE를 설정합니다. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* 현재 스레드의 nice 값을 반환합니다. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* 시스템 평균 부하의 100배 값을 반환합니다. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* 현재 스레드의 recent_cpu 값의 100배를 반환합니다. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* 유휴 스레드입니다. 다른 스레드가 실행 준비가 되지 않았을 때 실행됩니다.

   유휴 스레드는 초기에 thread_start()에 의해 ready list에 넣어집니다. 
   이 스레드는 초기에 한 번 스케줄링되며, 이 시점에서 idle_thread를 초기화하고, 
   thread_start()가 계속 진행되게 하기 위해 전달된 세마포어를 "up"합니다, 
   그리고 즉시 block 상태가 됩니다. 그 이후로, 유휴 스레드는 ready list에 나타나지 않습니다. 
   ready list가 비어 있을 때 next_thread_to_run()에서 특별한 경우로 반환됩니다. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* 인터럽트를 다시 활성화하고 다음 인터럽트를 기다립니다.

		   `sti` 명령어는 다음 명령어가 완료될 때까지 인터럽트를 비활성화합니다. 
		   따라서 이 두 명령어는 원자적으로 실행됩니다. 
		   이 원자성은 중요합니다. 그렇지 않으면, 인터럽트를 다시 활성화하고 
		   다음 인터럽트가 발생하는 것을 기다리는 사이에 인터럽트가 처리될 수 있으며, 
		   이로 인해 클럭 틱 가치의 시간을 낭비할 수 있습니다.

		   [IA32-v2a] "HLT", [IA32-v2b] "STI", 
		   그리고 [IA32-v3a] 7.11.1 "HLT 명령어"를 참조하세요. */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* 커널 스레드의 기반으로 사용되는 함수입니다. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* 스케줄러는 인터럽트를 비활성화한 상태에서 실행됩니다. */
	function (aux);       /* 스레드 함수를 실행합니다. */
	thread_exit ();       /* function()이 반환되면, 스레드를 종료합니다. */
}


/* T를 NAME이라는 이름의 차단된 스레드로 기본 초기화를 수행합니다. */
static void init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
}

/* 스케줄링할 다음 스레드를 선택하고 반환합니다. 
   실행 대기열이 비어 있지 않으면 실행 대기열에서 스레드를 반환해야 합니다. 
   (실행 중인 스레드가 계속 실행할 수 있으면, 그 스레드는 실행 대기열에 있을 것입니다.) 
   만약 실행 대기열이 비어 있다면, idle_thread를 반환합니다. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* iretq를 사용하여 스레드를 실행시킵니다. */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* 새 스레드의 페이지 테이블을 활성화함으로써 스레드를 전환하고, 
   이전 스레드가 종료되면 이를 제거합니다.

   이 함수가 호출될 때, 우리는 방금 스레드 PREV에서 전환했으며, 
   새 스레드는 이미 실행 중이며, 인터럽트는 여전히 비활성화되어 있습니다.

   스레드 전환이 완료될 때까지 printf()를 호출하는 것은 안전하지 않습니다. 
   실제로 이는 printf()가 함수의 끝에 추가되어야 함을 의미합니다. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* 주요 전환 로직입니다.
	 * 우리는 먼저 전체 실행 컨텍스트를 intr_frame으로 복원하고 
	 * 그런 다음 do_iret을 호출하여 다음 스레드로 전환합니다.
	 * 주의하십시오, 전환이 완료될 때까지 여기서부터 스택을 사용해서는 안됩니다. */
	__asm __volatile (
			/* 사용할 레지스터를 저장합니다. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* 새 프로세스를 스케줄링합니다. 입장시, 인터럽트는 꺼져 있어야 합니다.
 * 이 함수는 현재 스레드의 상태를 status로 변경하고 다른 스레드를 찾아 실행하고 전환합니다.
 * schedule()에서 printf()를 호출하는 것은 안전하지 않습니다. */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* 우리를 실행 중으로 표시합니다. */
	next->status = THREAD_RUNNING;

	/* 새로운 타임 슬라이스를 시작합니다. */
	thread_ticks = 0;

#ifdef USERPROG
	/* 새 주소 공간을 활성화합니다. */
	process_activate (next);
#endif

	if (curr != next) {
		/* 만약 우리가 전환한 스레드가 종료 중이라면, 그 스레드의 구조를 제거합니다.
		   이는 thread_exit()이 자기 자신을 기반으로 작동하지 않게 하기 위해 늦게 발생해야 합니다.
		   우리는 여기서 페이지 free 요청을 큐에 넣기만 합니다.
		   왜냐하면 페이지는 현재 스택에 의해 사용되고 있기 때문입니다.
		   실제 파괴 로직은 schedule()의 시작에서 호출될 것입니다. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* 스레드를 전환하기 전에, 우리는 먼저 현재 실행 중인 정보를 저장합니다. */
		thread_launch (next);
	}
}

/* 새 스레드를 위해 사용할 tid를 반환합니다. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

bool wake_up_time_less(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
    struct thread *ta = list_entry(a, struct thread, elem);
    struct thread *tb = list_entry(b, struct thread, elem);

    return ta->wake_up_time < tb->wake_up_time;
}

/*-------------------------------------------------------------------------------------------------*/

// thread_sleep 함수의 구현
void thread_sleep(int64_t tick) {

	struct thread *cur = thread_current (); 			// 현재 실행중인 스레드를 가져옵니다.
	enum intr_level old_level;

	old_level = intr_disable ();  						// 인터럽트를 비활성화하여 임계 영역을 보호합니다.

	if (cur != idle_thread) {  							// 현재 스레드가 idle 스레드가 아닌 경우에만 다음 작업을 수행합니다.

		cur->wake_up_time = tick;  						// 현재 스레드의 깨어날 시간을 설정합니다.
		// list_push_back (&sleep_list, &cur->elem);  		// 현재 스레드를 sleep_list에 추가합니다.
		list_insert_ordered(&sleep_list, &cur->elem, sort_wakeup_time, 0);
		
		// list_sort(&sleep_list, sort_func, 0);  			// sleep_list를 sort_func에 따라 정렬합니다.

		thread_block();  	// 현재 스레드를 BLOCKED 상태로 변경합니다. 이 상태는 다른 스레드가 실행되도록 합니다.

	}

	intr_set_level(old_level); // 원래의 인터럽트 수준을 복원합니다. 이를 통해 이전에 비활성화된 인터럽트가 다시 활성화됩니다.

}

// Implementation for thread_wakeup
void thread_wakeup(int64_t tick){

    enum intr_level old_level;

    old_level = intr_disable();  // Disable interrupts.

	while (!list_empty(&sleep_list)){

		struct list_elem *e = list_begin(&sleep_list);
		struct thread *t = list_entry (e, struct thread, elem);

		if (tick >= t->wake_up_time){

			e = list_pop_front(&sleep_list);
			thread_unblock(t);
		
		}
		else{

			break;

		}
	
	}

	intr_set_level(old_level);  // Restore old interrupt level.

}

/*-------------------------------------------------------------------------------------------------*/

bool sort_priority(const struct list_elem *a, const struct list_elem *b, void *aux){

	if(list_entry (a, struct thread, elem)->priority 
		> list_entry (b, struct thread, elem)->priority){

		return true;

	}
	else{

		return false;

	}

} 

bool sort_wakeup_time(const struct list_elem *a, const struct list_elem *b, void *aux){

	if(list_entry (a, struct thread, elem)->wake_up_time 
		< list_entry (b, struct thread, elem)->wake_up_time){

		return true;

	}
	else{

		return false;

	}

}

/*-------------------------------------------------------------------------------------------------*/


void change_list(void){

	struct thread *cur = thread_current();

	if(!list_empty(&ready_list)){

		if(cur->priority < list_entry(list_front(&ready_list), struct thread, elem)->priority) {
        	
			thread_yield();

		}
		
    }

}

// // Implementation for thread_wakeup
// void thread_wakeup(int64_t tick) {

//     struct list_elem *e;
// 	  struct list_elem *next_e;
//     enum intr_level old_level;


//     old_level = intr_disable ();  // Disable interrupts.
//     for (e = list_begin (&sleep_list); e != list_end (&sleep_list); e = next_e) {
        
// 			struct thread *t = list_entry (e, struct thread, elem);
// 			next_e = list_next(e);
// 			// printf("%d ", t->priority);
			
// 	    if (tick >= t->wake_up_time) {  // Check if thread should be woken up.
	
// 	        list_remove(e);  // Remove thread from sleep list.
// 	        thread_unblock(t);  // Use thread_unblock instead of manually changing the status
				
// 	    }

// 	}

// 	intr_set_level (old_level);  // Restore old interrupt level.
// }
